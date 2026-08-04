#pragma once
// Pipeline.h — 保序、无损的流水线执行器。
//
// 单个 worker 线程从有界阻塞输入环中按 FIFO 顺序取帧，依次运行
// 各 IStage，再把帧扇出给每个 IResultSink。因为单 worker 按序消费
// FIFO，输出顺序 == 提交顺序（现阶段无需 Resequencer）。环满时
// submit() 阻塞（背压），因此数据路径上永远不丢帧。
//
// stage 返回 false 表示显式丢弃（仅限 best-effort 旁路）；数据路径
// 上的 stage 必须返回 true。逐 stage 独立线程 + 数据并行 stage 的
// Resequencer 是后续优化，可在同一接口后实现。

#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

#include "core/Interfaces.h"
#include "core/SpscRing.h"

namespace radar {

class Pipeline {
public:
  explicit Pipeline(std::size_t inputCapacity = 64);
  ~Pipeline();

  Pipeline(const Pipeline &) = delete;
  Pipeline &operator=(const Pipeline &) = delete;

  // 在 start() 之前完成配置。
  void addStage(std::shared_ptr<IStage> stage);
  void addSink(std::shared_ptr<IResultSink> sink);

  void start();
  // 阻塞式提交（无损背压）。已停止时返回 false。
  bool submit(FrameContext ctx);
  // 关闭输入、排空剩余帧、join worker、flush 各 sink。
  void stop();

  std::size_t backlog() const { return in_.size(); }

private:
  void worker();

  SpscRing<FrameContext> in_;
  std::vector<std::shared_ptr<IStage>> stages_;
  std::vector<std::shared_ptr<IResultSink>> sinks_;
  std::thread th_;
  std::atomic<bool> running_{false};
};

} // namespace radar
