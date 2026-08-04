#pragma once
// RangeFftStage.h — 逐 chirp/逐 rx 的距离 FFT：parsed [chirp][rx][sample]
// -> rangeCube [chirp][rx][rangeBin]。
//
// 每行处理：可选 DC 去除（减均值以抑制零距离泄漏）、Hann 加窗、
// 零填充到 numRangeBins（2 的幂）、原位 radix-2 FFT。sample 轴是
// FrameBuffer 的最内（连续）维，因此每次变换都在连续内存上进行。
// 窗/plan/输出池均在构造期准备好：process() 稳态零分配。

#include <memory>
#include <vector>

#include "core/BufferPool.h"
#include "core/FrameBuffer.h"
#include "core/Interfaces.h"
#include "core/RadarConfig.h"
#include "dsp/Fft.h"

namespace radar {

class RangeFftStage : public IStage {
public:
  explicit RangeFftStage(
      const RadarConfig &cfg,
      std::shared_ptr<BufferPool<FrameBuffer>> pool = nullptr,
      bool removeDc = true);

  const char *name() const override { return "RangeFFT"; }

  // 读 ctx.parsed，填 ctx.rangeCube。无效/缺失的输入原样透传
  // （上游已标记，绝不静默丢弃）。
  bool process(FrameContext &ctx) override;

private:
  RadarConfig cfg_;
  FftPlan plan_;           // numRangeBins 点
  std::vector<float> win_; // 覆盖 numAdcSamples 的 Hann 窗
  std::shared_ptr<BufferPool<FrameBuffer>> pool_;
  bool removeDc_;
};

} // namespace radar
