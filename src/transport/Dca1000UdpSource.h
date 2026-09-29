#pragma once
#if !defined(__linux__)
#error Dca1000UdpSource is Linux-only (POSIX sockets)
#endif
// Dca1000UdpSource.h — DCA1000 原始 UDP 数据源（IFrameSource 实现）。
//
// 数据通路（刻意复刻 README 的「socket 线程绝不能阻塞」语义）：
//
//   rx 线程 : recvfrom -> Dca1000Reassembler（48 位 byte count 重组，只输出
//             完整帧）-> FrameSpool::push（非阻塞、无损；RAM 环满则溢写磁盘）
//   消费者  : next() -> FrameSpool::pop（阻塞、FIFO）-> 填充 FrameContext
//
// 于是生产者永不阻塞也永不丢帧（背压由 FrameSpool 用磁盘容量吸收，而不是
// 丢弃数据），消费者按线上顺序拿到有序完整帧；序号缺口由重组器计数并丢弃
// 受影响的帧，绝不把残帧传进流水线。这样真机 UDP 源与文件回放源可以走
// **同一条** Pipeline。

#include <atomic>
#include <csignal>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/FrameContext.h"
#include "core/Interfaces.h"
#include "transport/Dca1000Reassembler.h"
#include "transport/FrameSpool.h"

namespace radar {

class Dca1000UdpSource : public IFrameSource {
public:
  struct Options {
    std::string bindIp = "0.0.0.0"; // 本机绑定地址
    int dataPort = 4098;            // DCA1000 数据端口
    std::size_t frameBytes = 0;     // 必填：每帧字节数（来自 .cfg）
    std::string dcaIp;              // 非空 => 只接受来自该地址的包
    std::size_t rcvbuf = 8u * 1024u * 1024u;
    std::size_t ramCapFrames = 64;  // FrameSpool 第一级（RAM 环）容量
    std::string spillPath;          // 必填：第二级溢写文件路径
    int pollTimeoutMs = 200;        // rx 轮询粒度，也决定停机响应时延
    // 置位后 rx 线程在 ~pollTimeoutMs 内停止接收并关闭 spool，
    // 使阻塞中的 next() 返回 false（Ctrl-C 用；sig_atomic_t 可安全读）。
    const volatile std::sig_atomic_t *stopFlag = nullptr;
  };

  explicit Dca1000UdpSource(Options opts);
  ~Dca1000UdpSource() override;

  Dca1000UdpSource(const Dca1000UdpSource &) = delete;
  Dca1000UdpSource &operator=(const Dca1000UdpSource &) = delete;

  // 建 socket、bind、启动 rx 线程。失败时 lastError() 给出原因。
  bool open() override;
  // 停止接收、join rx、关闭 spool（幂等）。阻塞中的 next() 随即返回 false。
  void close() override;
  // 阻塞取下一帧；停止且排空后返回 false。
  bool next(FrameContext &out) override;

  std::string lastError() const;

  // ---- 统计。注意：重组器计数由 rx 线程写、本线程读，请在 close() 之后读取。
  std::uint64_t framesReceived() const;
  std::uint64_t missingPackets() const;
  std::uint64_t latePackets() const;
  std::uint64_t malformedPackets() const;
  std::uint64_t discardedFrames() const;
  std::uint64_t droppedFrames() const { return dropped_.load(); } // 溢写失败(磁盘)
  std::size_t spillPeak() const; // >0 => 曾发生 RAM->磁盘溢写
  std::size_t ramDepth() const;

private:
  void rxLoop();
  void setError(const std::string &msg);

  Options o_;
  int fd_ = -1;
  std::thread rx_;
  std::atomic<bool> closing_{false};
  std::unique_ptr<FrameSpool> spool_;
  std::unique_ptr<Dca1000Reassembler> reassembler_;
  // 线上帧号与 FrameSpool 里的帧一一对应；push 到 spool 期间持锁，
  // 保证消费者绝不会出现「有帧但取不到对应帧号」的错配。
  std::mutex pair_;
  std::deque<std::uint32_t> wireSeqs_;
  std::atomic<std::uint64_t> dropped_{0};
  std::uint64_t nextSeq_ = 0;
  mutable std::mutex errM_;
  std::string err_; // rx 线程与调用方都会写/读，用 errM_ 保护
};

} // namespace radar
