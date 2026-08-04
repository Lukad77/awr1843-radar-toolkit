#pragma once
// ClutterRemovalStage.h — 在 range cube 上做 MTI 静态杂波抑制。
//
// 按 (rx, rangeBin) 维护杂波图 = rangeCube 逐帧 chirp 均值的指数
// 滑动平均（EMA），并从每个 chirp 上**原位**减除。真正静止的
// 回波（bin 0 处的天线耦合、墙体、家具）会收敛进杂波图，从
// Doppler/CFAR 路径上消失；带逐 chirp 多普勒旋转的信号 chirp
// 均值近零，原样通过不受影响。
//
// alpha 语义：map <- (1-alpha)*map + alpha*chirpMean，即杂波图以
// ~1/alpha 帧的时间常数自适应。首帧直接播种杂波图
// （因此 bin-0 泄漏从第 1 帧起就被抑制）。
//
// 顺序约束：本 stage 原位修改 rangeCube。减除后的残差相量不再
// 满足 phi = 4*pi*d/lambda，因此 PhaseUnwrapStage（生命体征）
// 必须在本 stage 之前运行：
//   RangeFFT -> PhaseUnwrap -> ClutterRemoval -> DopplerFFT -> CFAR -> ...
//
// 有状态（杂波图跨帧保持）但单写者安全：Pipeline 在单个 worker
// 线程上运行全部 stage。

#include <complex>
#include <vector>

#include "core/Interfaces.h"

namespace radar {

class ClutterRemovalStage : public IStage {
public:
  // alpha ∈ [0, 1]：每帧 EMA 更新权重（自适应时间常数 ~1/alpha 帧）。
  explicit ClutterRemovalStage(float alpha = 0.02f);

  const char *name() const override { return "ClutterRemoval"; }

  // 从 ctx.rangeCube 原位减去杂波图，再用本帧 chirp 均值更新
  // 杂波图。形状变化时重置杂波图。
  bool process(FrameContext &ctx) override;

private:
  float alpha_;
  bool init_ = false;
  std::size_t mapRx_ = 0, mapBins_ = 0;   // 形状守卫
  std::vector<std::complex<float>> map_;  // [rx * rangeBins]
  std::vector<std::complex<float>> mean_; // 逐帧暂存
};

} // namespace radar
