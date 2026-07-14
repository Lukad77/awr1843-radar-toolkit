#include "pipeline/Pipeline.h"

namespace radar {

Pipeline::Pipeline(std::size_t inputCapacity) : in_(inputCapacity) {}

Pipeline::~Pipeline() { stop(); }

void Pipeline::addStage(std::shared_ptr<IStage> stage) { stages_.push_back(std::move(stage)); }
void Pipeline::addSink(std::shared_ptr<IResultSink> sink) { sinks_.push_back(std::move(sink)); }

void Pipeline::start() {
    if (running_.exchange(true)) return;  // already running
    th_ = std::thread(&Pipeline::worker, this);
}

bool Pipeline::submit(FrameContext ctx) { return in_.push(std::move(ctx)); }

void Pipeline::stop() {
    if (!running_.exchange(false)) return;  // not running
    in_.close();
    if (th_.joinable()) th_.join();
    for (auto& s : sinks_) s->flush();
}

void Pipeline::worker() {
    FrameContext ctx;
    while (in_.pop(ctx)) {  // FIFO order preserved by the single worker
        bool dropped = false;
        for (auto& stage : stages_) {
            if (!stage->process(ctx)) {  // explicit drop (best-effort branches only)
                dropped = true;
                break;
            }
        }
        if (dropped) continue;
        for (auto& sink : sinks_) sink->consume(ctx);
    }
}

}  // namespace radar
