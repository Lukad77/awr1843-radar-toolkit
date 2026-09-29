#include "transport/Dca1000UdpSource.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <utility>

namespace radar {

Dca1000UdpSource::Dca1000UdpSource(Options opts) : o_(std::move(opts)) {}

Dca1000UdpSource::~Dca1000UdpSource() { close(); }

void Dca1000UdpSource::setError(const std::string &msg) {
  std::lock_guard<std::mutex> lk(errM_);
  if (err_.empty()) err_ = msg; // 保留首个（最具体）原因
}

std::string Dca1000UdpSource::lastError() const {
  std::lock_guard<std::mutex> lk(errM_);
  return err_;
}

bool Dca1000UdpSource::open() {
  if (o_.frameBytes == 0) {
    setError("frameBytes is required (derive it from the radar .cfg)");
    return false;
  }
  if (o_.spillPath.empty()) {
    setError("spillPath is required (second-tier lossless buffer)");
    return false;
  }
  if (o_.dataPort < 1 || o_.dataPort > 65535) {
    setError("dataPort must be in 1..65535");
    return false;
  }

  sockaddr_in bindAddr{};
  bindAddr.sin_family = AF_INET;
  bindAddr.sin_port = htons(static_cast<std::uint16_t>(o_.dataPort));
  if (::inet_pton(AF_INET, o_.bindIp.c_str(), &bindAddr.sin_addr) != 1) {
    setError("invalid bind address: " + o_.bindIp);
    return false;
  }

  fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd_ < 0) {
    setError(std::string("data socket failed: ") + std::strerror(errno));
    return false;
  }
  // 请求值可能被内核 net.core.rmem_max 截断（验收文档第 5 节），尽力而为。
  const int rcvbuf = static_cast<int>(o_.rcvbuf);
  ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  if (::bind(fd_, reinterpret_cast<const sockaddr *>(&bindAddr),
             sizeof(bindAddr)) != 0) {
    setError(std::string("data port bind failed: ") + std::strerror(errno));
    ::close(fd_);
    fd_ = -1;
    return false;
  }

  spool_ = std::make_unique<FrameSpool>(o_.frameBytes, o_.ramCapFrames,
                                        o_.spillPath);
  reassembler_ = std::make_unique<Dca1000Reassembler>(
      o_.frameBytes, [this](std::uint64_t wireFrame,
                            const std::vector<std::uint8_t> &frame) {
        // 持 pair_ 期间完成入队：消费者绝不会看到「有帧但配不上帧号」。
        std::lock_guard<std::mutex> lk(pair_);
        if (!spool_->push(
                std::vector<std::uint8_t>(frame.begin(), frame.end()))) {
          ++dropped_; // 磁盘写失败（显式过载条件），由调用方告警
          return true;
        }
        wireSeqs_.push_back(static_cast<std::uint32_t>(wireFrame));
        return true;
      });

  closing_.store(false);
  rx_ = std::thread([this] { rxLoop(); });
  return true;
}

void Dca1000UdpSource::rxLoop() {
  const bool hasFilter = !o_.dcaIp.empty();
  sockaddr_in filter{};
  if (hasFilter) {
    filter.sin_family = AF_INET;
    ::inet_pton(AF_INET, o_.dcaIp.c_str(), &filter.sin_addr);
  }

  std::vector<std::uint8_t> packet(65536);
  while (!closing_.load()) {
    if (o_.stopFlag != nullptr && *o_.stopFlag) break; // Ctrl-C：快速退出
    pollfd p{fd_, POLLIN, 0};
    const int ready = ::poll(&p, 1, o_.pollTimeoutMs);
    if (ready < 0) {
      if (errno == EINTR) continue;
      setError(std::string("data poll failed: ") + std::strerror(errno));
      break;
    }
    if (ready == 0) continue;

    sockaddr_in from{};
    socklen_t fromLen = sizeof(from);
    const ssize_t n = ::recvfrom(fd_, packet.data(), packet.size(), 0,
                                 reinterpret_cast<sockaddr *>(&from), &fromLen);
    if (n < 0) {
      if (errno == EINTR) continue;
      setError(std::string("data receive failed: ") + std::strerror(errno));
      break;
    }
    if (hasFilter && from.sin_addr.s_addr != filter.sin_addr.s_addr) continue;
    if (!reassembler_->consume(packet.data(), static_cast<std::size_t>(n)))
      break;
  }
  if (spool_) spool_->close(); // 让阻塞中的 next() 排空后返回 false
}

void Dca1000UdpSource::close() {
  closing_.store(true);
  if (rx_.joinable()) rx_.join();
  if (spool_) spool_->close();
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

bool Dca1000UdpSource::next(FrameContext &out) {
  if (!spool_) return false;
  std::vector<std::uint8_t> bytes;
  if (!spool_->pop(bytes)) return false; // 已关闭且排空

  std::uint32_t wire = 0;
  {
    std::lock_guard<std::mutex> lk(pair_);
    if (!wireSeqs_.empty()) {
      wire = wireSeqs_.front();
      wireSeqs_.pop_front();
    }
  }

  FrameContext ctx;
  ctx.frameSeq = nextSeq_++;
  ctx.wireSeqStart = wire;
  ctx.valid = true;
  ctx.raw = std::make_shared<std::vector<std::uint8_t>>(std::move(bytes));
  ctx.tCaptured = std::chrono::steady_clock::now();
  out = std::move(ctx);
  return true;
}

std::uint64_t Dca1000UdpSource::framesReceived() const {
  return reassembler_ ? reassembler_->frames() : 0;
}

std::uint64_t Dca1000UdpSource::missingPackets() const {
  return reassembler_ ? reassembler_->missingPackets() : 0;
}

std::uint64_t Dca1000UdpSource::latePackets() const {
  return reassembler_ ? reassembler_->latePackets() : 0;
}

std::uint64_t Dca1000UdpSource::malformedPackets() const {
  return reassembler_ ? reassembler_->malformedPackets() : 0;
}

std::uint64_t Dca1000UdpSource::discardedFrames() const {
  return reassembler_ ? reassembler_->discardedFrames() : 0;
}

std::size_t Dca1000UdpSource::spillPeak() const {
  return spool_ ? spool_->diskPeak() : 0;
}

std::size_t Dca1000UdpSource::ramDepth() const {
  return spool_ ? spool_->ramDepth() : 0;
}

} // namespace radar
