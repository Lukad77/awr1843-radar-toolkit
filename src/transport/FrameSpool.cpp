#include "transport/FrameSpool.h"

#include <cstdio>
#include <ios>
#include <utility>

namespace radar {

FrameSpool::FrameSpool(std::size_t frameBytes, std::size_t ramCapFrames,
                       std::string spillPath)
    : frameBytes_(frameBytes), ramCap_(ramCapFrames == 0 ? 1 : ramCapFrames),
      path_(std::move(spillPath)) {}

FrameSpool::~FrameSpool() {
  close();
  if (file_.is_open())
    file_.close();
  std::remove(path_.c_str());
}

bool FrameSpool::push(std::vector<std::uint8_t> frame) {
  if (frame.size() != frameBytes_)
    return false;
  std::lock_guard<std::mutex> lk(m_);
  if (closed_)
    return false;

  if (!spilling_ && ram_.size() < ramCap_) {
    ram_.push_back(std::move(frame)); // 第一级：RAM
  } else {
    spilling_ = true; // 第二级：磁盘（永不阻塞/不丢）
    if (!diskWrite(frame))
      return false;
    const std::size_t pending = writeOff_ - readOff_;
    if (pending > diskPeak_)
      diskPeak_ = pending;
  }
  cv_.notify_one();
  return true;
}

bool FrameSpool::pop(std::vector<std::uint8_t> &out) {
  std::unique_lock<std::mutex> lk(m_);
  cv_.wait(lk,
           [&] { return !ram_.empty() || (writeOff_ > readOff_) || closed_; });

  if (!ram_.empty()) { // RAM 里是最旧的帧 -> 先排空 RAM
    out = std::move(ram_.front());
    ram_.pop_front();
    return true;
  }
  if (writeOff_ > readOff_) { // 再取磁盘溢写的帧，保持顺序
    if (!diskRead(out))
      return false;
    if (readOff_ == writeOff_) { // 磁盘完全排空 -> 重新启用 RAM
      readOff_ = writeOff_ = 0;
      spilling_ = false;
    }
    return true;
  }
  return false; // 已关闭且已排空
}

void FrameSpool::close() {
  {
    std::lock_guard<std::mutex> lk(m_);
    closed_ = true;
  }
  cv_.notify_all();
}

bool FrameSpool::diskWrite(const std::vector<std::uint8_t> &f) {
  if (!file_.is_open()) {
    file_.open(path_, std::ios::in | std::ios::out | std::ios::binary |
                          std::ios::trunc);
    if (!file_)
      return false;
  }
  file_.clear();
  file_.seekp(static_cast<std::streamoff>(writeOff_ * frameBytes_),
              std::ios::beg);
  file_.write(reinterpret_cast<const char *>(f.data()),
              static_cast<std::streamsize>(frameBytes_));
  file_.flush();
  if (!file_)
    return false;
  ++writeOff_;
  return true;
}

bool FrameSpool::diskRead(std::vector<std::uint8_t> &out) {
  if (!file_.is_open())
    return false;
  out.resize(frameBytes_);
  file_.clear();
  file_.seekg(static_cast<std::streamoff>(readOff_ * frameBytes_),
              std::ios::beg);
  file_.read(reinterpret_cast<char *>(out.data()),
             static_cast<std::streamsize>(frameBytes_));
  if (!file_)
    return false;
  ++readOff_;
  return true;
}

std::size_t FrameSpool::ramDepth() const {
  std::lock_guard<std::mutex> lk(m_);
  return ram_.size();
}
std::size_t FrameSpool::diskPending() const {
  std::lock_guard<std::mutex> lk(m_);
  return writeOff_ - readOff_;
}
std::size_t FrameSpool::diskPeak() const {
  std::lock_guard<std::mutex> lk(m_);
  return diskPeak_;
}

} // namespace radar
