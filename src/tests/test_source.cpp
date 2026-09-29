// test_source.cpp — Dca1000UdpSource 无硬件单测（Linux）。
//
// 不接硬件也能验证「真机 UDP 源」：向 127.0.0.1 的真实 UDP 端口注入按
// DCA1000 线上格式构造的包，检查源的取帧顺序、帧号配对、两级无损缓冲
// （RAM 满 -> 磁盘溢写）、缺口计数与停机排空语义。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "core/FrameContext.h"
#include "transport/Dca1000UdpSource.h"

namespace {

int failures = 0;

void check(bool ok, const std::string &name) {
  if (!ok) {
    std::cerr << "FAIL " << name << '\n';
    ++failures;
  }
}

void checkEq(long long got, long long want, const std::string &name) {
  if (got != want) {
    std::cerr << "FAIL " << name << "\n  got:  " << got << "\n  want: " << want
              << '\n';
    ++failures;
  }
}

// DCA1000 线上包：u32 seq + u48 byte count + payload。
std::vector<std::uint8_t> makePacket(std::uint32_t seq, std::uint64_t offset,
                                     std::uint8_t fill, std::size_t bytes) {
  std::vector<std::uint8_t> p(10 + bytes, fill);
  for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(seq >> (8 * i));
  for (int i = 0; i < 6; ++i)
    p[4 + i] = static_cast<std::uint8_t>(offset >> (8 * i));
  return p;
}

// 端点：一个本地 UDP 发送 socket + 停机标志（模拟 Ctrl-C）。
struct Peer {
  int tx = -1;
  volatile std::sig_atomic_t stop = 0;
  sockaddr_in to{};

  explicit Peer(int port) {
    tx = ::socket(AF_INET, SOCK_DGRAM, 0);
    to.sin_family = AF_INET;
    to.sin_port = htons(static_cast<std::uint16_t>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
  }
  ~Peer() {
    if (tx >= 0) ::close(tx);
  }
  void send(const std::vector<std::uint8_t> &p) {
    ::sendto(tx, p.data(), p.size(), 0,
             reinterpret_cast<const sockaddr *>(&to), sizeof(to));
  }
};

radar::Dca1000UdpSource::Options makeOptions(int port, std::size_t frameBytes,
                                             std::size_t ramCap,
                                             const volatile std::sig_atomic_t *stop) {
  radar::Dca1000UdpSource::Options o;
  o.bindIp = "127.0.0.1";
  o.dataPort = port;
  o.frameBytes = frameBytes;
  o.dcaIp = "127.0.0.1";
  o.ramCapFrames = ramCap;
  o.spillPath = "radar_source_test.spill";
  o.pollTimeoutMs = 20;
  o.stopFlag = stop;
  return o;
}

// 排空语义：置停机标志后，已收帧必须全部取出，然后 next() 返回 false。
std::vector<radar::FrameContext> drain(radar::Dca1000UdpSource &src) {
  std::vector<radar::FrameContext> frames;
  radar::FrameContext ctx;
  while (src.next(ctx)) frames.push_back(ctx);
  return frames;
}

void testOrderedDeliveryAndSpill() {
  const int port = 47011;
  Peer peer(port);
  radar::Dca1000UdpSource src(
      makeOptions(port, /*frameBytes=*/8, /*ramCap=*/1, &peer.stop));
  check(src.open(), "open source: " + src.lastError());

  // 帧 0 跨两个包（offset 0..4 / 5..7），帧 1 也跨两个包。
  peer.send(makePacket(0, 0, 0xAA, 5));
  peer.send(makePacket(1, 5, 0xAA, 3));
  peer.send(makePacket(2, 8, 0xBB, 4));
  peer.send(makePacket(3, 12, 0xBB, 4));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  peer.stop = 1; // 模拟 Ctrl-C：源线程退出 -> 关闭 spool -> 排空后 next() 返回 false
  const std::vector<radar::FrameContext> frames = drain(src);

  checkEq(static_cast<long long>(frames.size()), 2, "two complete frames");
  if (frames.size() == 2) {
    checkEq(static_cast<long long>(frames[0].frameSeq), 0, "frameSeq 0");
    checkEq(static_cast<long long>(frames[1].frameSeq), 1, "frameSeq 1");
    checkEq(static_cast<long long>(frames[0].wireSeqStart), 0, "frame0 wire seq");
    checkEq(static_cast<long long>(frames[1].wireSeqStart), 1, "frame1 wire seq");
    check(frames[0].valid && frames[0].raw && frames[0].raw->size() == 8,
          "frame0 payload size");
    check(frames[0].raw->at(0) == 0xAA && frames[0].raw->at(7) == 0xAA,
          "frame0 bytes");
    check(frames[1].raw->at(0) == 0xBB && frames[1].raw->at(7) == 0xBB,
          "frame1 bytes");
  }
  checkEq(static_cast<long long>(src.missingPackets()), 0, "no missing packets");
  checkEq(static_cast<long long>(src.discardedFrames()), 0, "no discarded frames");
  checkEq(static_cast<long long>(src.framesReceived()), 2, "frames received");
  // RAM 环容量 1 => 第二帧必须走磁盘溢写（两级无损缓冲生效）。
  check(src.spillPeak() >= 1, "second frame spilled to disk");
  checkEq(static_cast<long long>(src.droppedFrames()), 0, "no spool write failure");
  src.close();
}

void testGapDropsPartialFrame() {
  const int port = 47012;
  Peer peer(port);
  radar::Dca1000UdpSource src(
      makeOptions(port, /*frameBytes=*/8, /*ramCap=*/4, &peer.stop));
  check(src.open(), "open source (gap case): " + src.lastError());

  peer.send(makePacket(0, 0, 0xCC, 4));  // 帧 0 只到一半
  peer.send(makePacket(2, 8, 0xDD, 4));  // seq 1 缺失 -> 缺口，帧 0 作废
  peer.send(makePacket(3, 12, 0xDD, 4)); // 帧 1 完整
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  peer.stop = 1;
  const std::vector<radar::FrameContext> frames = drain(src);

  checkEq(static_cast<long long>(frames.size()), 1, "only the complete frame survives");
  if (frames.size() == 1) {
    check(frames[0].raw->at(0) == 0xDD && frames[0].raw->at(7) == 0xDD,
          "surviving frame bytes");
    checkEq(static_cast<long long>(frames[0].wireSeqStart), 1, "surviving wire seq");
  }
  checkEq(static_cast<long long>(src.missingPackets()), 1, "missing packet counted");
  check(src.discardedFrames() >= 1, "partial frame discarded and counted");
  src.close();
}

void testStopWithoutData() {
  const int port = 47013;
  Peer peer(port);
  radar::Dca1000UdpSource src(
      makeOptions(port, /*frameBytes=*/8, /*ramCap=*/4, &peer.stop));
  check(src.open(), "open source (idle case): " + src.lastError());
  peer.stop = 1; // 没有数据时也必须能从阻塞的 next() 退出
  const std::vector<radar::FrameContext> frames = drain(src);
  check(frames.empty(), "no frames when nothing was sent");
  src.close();
}

void testOpenErrors() {
  Peer peer(47014);
  radar::Dca1000UdpSource::Options bad =
      makeOptions(47014, /*frameBytes=*/0, 4, &peer.stop);
  radar::Dca1000UdpSource src(bad);
  check(!src.open(), "zero frameBytes rejected");
  check(!src.lastError().empty(), "open error message");

  // 端口被占用（同一端口开第二个源）应报错而不是静默失败
  Peer busyPeer(47015);
  radar::Dca1000UdpSource first(
      makeOptions(47015, 8, 4, &busyPeer.stop));
  check(first.open(), "first source on port 47015");
  radar::Dca1000UdpSource second(
      makeOptions(47015, 8, 4, &busyPeer.stop));
  check(!second.open(), "second bind on same port rejected");
  check(second.lastError().find("bind") != std::string::npos,
        "bind error is reported");
  first.close();
}

} // namespace

int main() {
  testOrderedDeliveryAndSpill();
  testGapDropsPartialFrame();
  testStopWithoutData();
  testOpenErrors();
  std::cout << (failures ? "source tests failed\n" : "source tests passed\n");
  return failures ? 1 : 0;
}
