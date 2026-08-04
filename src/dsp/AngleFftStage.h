#pragma once
// AngleFftStage.h — 逐 CFAR 检测的方位角估计。
//
// 对每个检测，从 dopplerCube 的 (dopplerBin, rangeBin) 处提取跨
// 虚拟天线阵列的复数快拍，零填充到 numAngleBins 后做 FFT。
// 对 lambda/2 均匀线阵，阵元间相位为 pi*sin(theta)，因此
// fftshift 后的峰值索引 kk ∈ [-N/2, N/2) 满足 sin(theta) = 2*kk/N。
//
// TDM-MIMO（numTxAnt > 1）需要先做逐 Tx 多普勒相位补偿，虚拟
// 阵列才相干；那是后续扩展。因此本 stage 仅对 1-Tx 配置
// （且 >= 2 Rx）计算角度；否则检测以 angleDeg = NaN 透传。

#include <vector>

#include "core/Interfaces.h"
#include "core/RadarConfig.h"
#include "dsp/Detection.h"
#include "dsp/Fft.h"

namespace radar {

class AngleFftStage : public IStage {
public:
  explicit AngleFftStage(const RadarConfig &cfg);

  const char *name() const override { return "AngleFFT"; }

  // 读 ctx.dopplerCube + ctx.detections，填 Detection::angleDeg。
  bool process(FrameContext &ctx) override;

private:
  RadarConfig cfg_;
  FftPlan plan_;                             // numAngleBins 点
  std::vector<std::complex<float>> scratch_; // 零填充后的快拍
  bool supported_;                           // 仅 1 Tx（见文件头说明）
};

} // namespace radar
