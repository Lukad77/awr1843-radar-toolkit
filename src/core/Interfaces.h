#pragma once
// Interfaces.h — 支撑依赖倒置与可插拔性的接缝。
//
//   IFrameSource     : 产生有序、无损的原始帧（实时 DCA1000、文件回放
//                      或仿真器）—— 所有来源共用同一条流水线。
//   IStage           : 一个流水线步骤（Parse、RangeFFT、CFAR、推理...）。
//                      流水线并行模型天然保序；stage 返回 false 表示
//                      丢弃该帧（仅限显示旁路）。
//   IResultSink      : 扇出消费者（文件、CSV、WebSocket、指标）。
//   IInferenceEngine : NN 后端抽象（ONNX Runtime / TensorRT）。

#include <cstddef>
#include <string>

#include "core/FrameContext.h"

namespace radar {

class IFrameSource {
public:
  virtual ~IFrameSource() = default;
  virtual bool open() = 0;
  virtual void close() = 0;
  // 阻塞：取下一个有序原始帧。停止时返回 false。
  virtual bool next(FrameContext &out) = 0;
};

class IStage {
public:
  virtual ~IStage() = default;
  virtual const char *name() const = 0;
  // 原位处理该帧。返回 false 表示丢弃（应当罕见，且仅允许出现在
  // best-effort/显示旁路上，数据路径上绝不允许）。
  virtual bool process(FrameContext &ctx) = 0;
};

class IResultSink {
public:
  virtual ~IResultSink() = default;
  virtual void consume(const FrameContext &ctx) = 0;
  virtual void flush() {}
};

class IInferenceEngine {
public:
  virtual ~IInferenceEngine() = default;
  virtual bool load(const std::string &modelPath) = 0;
  virtual bool infer(const float *input, std::size_t inN, float *output,
                     std::size_t outN) = 0;
};

} // namespace radar
