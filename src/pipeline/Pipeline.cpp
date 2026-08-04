#include "pipeline/Pipeline.h"

namespace radar {

Pipeline::Pipeline(std::size_t inputCapacity) : in_(inputCapacity) {}

Pipeline::~Pipeline() { stop(); }

void Pipeline::addStage(std::shared_ptr<IStage> stage) {
  stages_.push_back(std::move(stage));
}
void Pipeline::addSink(std::shared_ptr<IResultSink> sink) {
  sinks_.push_back(std::move(sink));
}

void Pipeline::start() {
  if (running_.exchange(true))
    return; // 已在运行
  th_ = std::thread(&Pipeline::worker, this);
}

bool Pipeline::submit(FrameContext ctx) { return in_.push(std::move(ctx)); }

void Pipeline::stop() {
  if (!running_.exchange(false))
    return; // 未在运行
  in_.close();
  if (th_.joinable())
    th_.join();
  for (auto &s : sinks_)
    s->flush();
}

void Pipeline::worker() {
  FrameContext ctx;
  while (in_.pop(ctx)) { // 单 worker 消费，天然保持 FIFO 顺序
    bool dropped = false;
    for (auto &stage : stages_) {
      if (!stage->process(ctx)) { // 显式丢弃（仅限 best-effort 旁路）
        dropped = true;
        break;
      }
    }
    if (dropped)
      continue;
    for (auto &sink : sinks_)
      sink->consume(ctx);
  }
}

} // namespace radar
