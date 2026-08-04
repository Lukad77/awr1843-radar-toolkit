#pragma once
// PhaseUnwrapStage.h — 在单个被跟踪 range bin 上做慢时间相位解缠
// （生命体征：胸腔位移 -> 呼吸/心跳波形）。
//
// 有状态 stage（单写者安全：Pipeline 在单 worker 上跑所有 stage）。
// 三个机制修复了真实数据上实测到的平台/陡坎伪影：
//
// 1. 带迟滞的峰值跟随（followPeak，仅 targetRangeBin < 0 时）：
//    每帧在 range gate 内取最强 bin 作为切换候选；只有同一候选
//    连续 switchHoldFrames 帧以 switchRatio 倍幅度胜过当前 bin
//    才切换（防止 bin 乒乓）。
//
// 2. 换 bin 时的相位桥接：新 bin 的首个采样贡献零增量
//    （prevRaw_ 重新播种），因此切换观测点绝不会注入伪造的
//    <=pi 相位阶跃，位移轨迹保持连续。
//
// 3. DC 补偿（Kasa 圆拟合）：bin 信号为 v = S + a*e^{j*phi}
//    （静态多径/裙边泄漏 S + 振动胸壁 a）。|S| >> a 时 arg(v)
//    被钉在 arg(S)（平台）；v 扫过原点附近时 arg 剧烈旋转
//    （陡坎）。我们维护帧均相量的滑窗，闭式 Kasa 最小二乘
//    拟合圆心，对 (v - center) 取 atan2：残差相位才是纯净的
//    4*pi*d/lambda。退化拟合（窗未满、点重合、弧近共线）
//    保守地沿用旧圆心 —— 绝不外推。轨迹中途启用补偿只引入
//    一次性有界（<=pi）偏移，相对波形不受影响。真正静止的
//    目标保持不补偿（零张角窗会被判为退化）。
//
// 解缠核心：逐 chirp 在 (chirp, rxIdx, bin) 处取 phi = atan2；增量
// 用 remainder(d, 2*pi) 折回 (-pi, pi] 后累积（仅当相邻采样间
// 位移 < lambda/4 时有效）。帧输出 = 本帧各 chirp 解缠相位的
// 均值；位移 = lambda * delta_phi / (4*pi)。
//
// 质量字段（就地逐帧输出）：phaseTrackBin + phaseTrackAmp
// （跟踪 bin 的原始 chirp 均幅度）。原始幅度塌陷标记了合成
// 相量扫过原点附近的帧 —— 恰是输出相位最依赖圆心估计精度
// 的地方 —— 消费端可据此门控。

#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/Interfaces.h"
#include "core/RadarConfig.h"

namespace radar {

struct PhaseUnwrapParams {
  int targetRangeBin = -1; // -1 => 自动：在 gate 内锁定并跟随最强 bin
  float minRangeM = 0.3f;  // 距离门（cfg.rangeIdxToMeters > 0 时生效）
  float maxRangeM = 2.5f;
  int rxIdx = 0; // 相位跟踪使用的天线

  // 峰值跟随（仅 targetRangeBin < 0 时）。
  bool followPeak = true;
  int switchHoldFrames = 5;  // 候选必须持续这么多帧
  float switchRatio = 1.25f; // 且幅度超过当前 bin 这个倍数

  // Kasa 圆拟合 DC 补偿。
  bool dcCompensation = true;
  int dcWindowFrames = 128; // 帧均相量的滑动窗长度
};

class PhaseUnwrapStage : public IStage {
public:
  PhaseUnwrapStage(const RadarConfig &cfg, PhaseUnwrapParams params = {});

  const char *name() const override { return "PhaseUnwrap"; }

  // 读 ctx.rangeCube；填 ctx.unwrappedPhaseRad / ctx.displacementMm /
  // ctx.phaseTrackBin / ctx.phaseTrackAmp。
  bool process(FrameContext &ctx) override;

  // 当前跟踪的 range bin（首次锁定前为 -1）。
  int targetBin() const noexcept { return bin_; }
  // 至今经迟滞确认的 bin 切换次数。
  int switchCount() const noexcept { return switches_; }

private:
  int selectBin(const FrameBuffer &cube, std::size_t rx) const;
  double meanAmp(const FrameBuffer &cube, std::size_t rx, int b) const;
  void fitDcCenter(); // 对窗内点做 Kasa 最小二乘；退化时沿用旧圆心

  RadarConfig cfg_;
  PhaseUnwrapParams p_;

  // 跟踪 bin + 迟滞状态。
  int bin_ = -1;
  int candBin_ = -1;
  int candStreak_ = 0;
  int switches_ = 0;
  bool bridgePending_ = false;

  // 解缠累积器。
  bool hasPrev_ = false;
  double prevRaw_ = 0.0;   // 上一个缠绕相位（rad）
  double unwrapped_ = 0.0; // 运行中的解缠相位（rad）
  double baseMean_ = 0.0;  // 首帧均值（位移零点）
  bool hasBase_ = false;

  // DC 补偿状态（跟踪 bin 处帧均相量的环形缓冲）。
  std::vector<std::complex<double>> dcBuf_;
  std::size_t dcHead_ = 0, dcCount_ = 0;
  std::complex<double> dcCenter_{0.0, 0.0};
  bool dcValid_ = false;
};

} // namespace radar
