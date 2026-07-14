#pragma once
// Pipeline.h — order-preserving, lossless pipeline executor.
//
// A single worker thread pops frames (in FIFO order) from a bounded blocking
// input ring, runs each IStage in sequence, then fans the frame out to every
// IResultSink. Because a single worker consumes the FIFO in order, output order
// == submit order (no Resequencer needed at this stage). submit() blocks when
// the ring is full (backpressure) so nothing is ever dropped on the data path.
//
// A stage returning false marks an explicit drop (best-effort branches only);
// on the data path stages return true. Per-stage threads + Resequencer for
// data-parallel stages are a later optimization behind this same interface.

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

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    // Configure before start().
    void addStage(std::shared_ptr<IStage> stage);
    void addSink(std::shared_ptr<IResultSink> sink);

    void start();
    // Blocking submit (lossless backpressure). Returns false if stopped.
    bool submit(FrameContext ctx);
    // Close input, drain remaining frames, join worker, flush sinks.
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

}  // namespace radar
