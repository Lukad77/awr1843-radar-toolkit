#pragma once
// FrameSpool.h — 定长原始帧的两级无损 FIFO（Phase 2）。
//
// 第一级：有界 RAM 环（deque，最多 ramCapFrames 帧）。
// 第二级：RAM 满时溢写到磁盘文件，使生产者（socket 排空线程）
//         永不阻塞、永不丢帧。这就是 “record-then-process” 充当安全网的
//         具体落地：容量由磁盘而非 RAM 限定。
//
// FIFO 顺序跨 RAM->磁盘边界保持：读者先排空 RAM（它持有最旧的帧），
// 再读磁盘；一旦开始溢写，新帧持续写磁盘直到磁盘完全排空
// （之后才重新启用 RAM）。
//
// push() 非阻塞且无损；仅在已关闭或磁盘写入错误（需告警的显式
// 过载条件）时返回 false。

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace radar {

class FrameSpool {
public:
  FrameSpool(std::size_t frameBytes, std::size_t ramCapFrames,
             std::string spillPath);
  ~FrameSpool();

  FrameSpool(const FrameSpool &) = delete;
  FrameSpool &operator=(const FrameSpool &) = delete;

  // 非阻塞、无损。frame.size() 必须等于 frameBytes。
  // 仅在已关闭或磁盘写错误（过载）时返回 false。
  bool push(std::vector<std::uint8_t> frame);

  // 阻塞、FIFO。仅在已关闭且完全排空后返回 false。
  bool pop(std::vector<std::uint8_t> &out);

  void close();

  // 指标查询（每个都取锁）。
  std::size_t ramDepth() const;
  std::size_t diskPending() const;
  std::size_t diskPeak() const; // 磁盘积压历史峰值（>0 => 发生过溢写）

private:
  bool diskWrite(const std::vector<std::uint8_t> &f); // 调用方持锁
  bool diskRead(std::vector<std::uint8_t> &out);      // 调用方持锁

  mutable std::mutex m_;
  std::condition_variable cv_;
  std::size_t frameBytes_;
  std::size_t ramCap_;
  std::deque<std::vector<std::uint8_t>> ram_;
  bool spilling_ = false;
  bool closed_ = false;

  std::string path_;
  std::fstream file_;
  std::size_t writeOff_ = 0; // 单位：帧
  std::size_t readOff_ = 0;  // 单位：帧
  std::size_t diskPeak_ = 0;
};

} // namespace radar
