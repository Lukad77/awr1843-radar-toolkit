#pragma once
// Interfaces.h — the seams that enable dependency inversion and pluggability.
//
//   IFrameSource     : produces ordered, lossless raw frames (live DCA1000 or
//                      file replay or simulator) — same pipeline for all.
//   IStage           : one pipeline step (Parse, RangeFFT, CFAR, Inference...).
//                      Pipeline parallelism preserves order for free; a stage
//                      returning false marks the frame dropped (display only).
//   IResultSink      : a fan-out consumer (file, CSV, WebSocket, metrics).
//   IInferenceEngine : NN backend abstraction (ONNX Runtime / TensorRT).

#include <cstddef>
#include <string>

#include "core/FrameContext.h"

namespace radar {

class IFrameSource {
public:
    virtual ~IFrameSource() = default;
    virtual bool open() = 0;
    virtual void close() = 0;
    // Blocking: fetch the next ordered raw frame. Returns false when stopped.
    virtual bool next(FrameContext& out) = 0;
};

class IStage {
public:
    virtual ~IStage() = default;
    virtual const char* name() const = 0;
    // Process the frame in place. Return false to drop it (should be rare and
    // is only acceptable on best-effort/display branches, never on the data path).
    virtual bool process(FrameContext& ctx) = 0;
};

class IResultSink {
public:
    virtual ~IResultSink() = default;
    virtual void consume(const FrameContext& ctx) = 0;
    virtual void flush() {}
};

class IInferenceEngine {
public:
    virtual ~IInferenceEngine() = default;
    virtual bool load(const std::string& modelPath) = 0;
    virtual bool infer(const float* input, std::size_t inN,
                       float* output, std::size_t outN) = 0;
};

} // namespace radar
