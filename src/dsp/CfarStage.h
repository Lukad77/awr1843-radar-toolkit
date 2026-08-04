#pragma once
// CfarStage.h — 沿距离轴的 1D CA-CFAR，对线性功率 RD 图的每条
// 多普勒行独立运行。
//
// 经典单元平均 CFAR：噪声 = CUT 两侧训练单元的均值（排除保护
// 单元）；门限系数 alpha = T*(pfa^(-1/T)-1)，其中 T = 实际使用的
// 训练单元数，因此边缘单元（单侧截断窗）仍保持同一设计 Pfa
// 而不是被跳过。局部峰门控（CUT >= 左邻、> 右邻）把簇收敛，
// 使一个目标只产生一条检测。

#include <memory>
#include <vector>

#include "core/Interfaces.h"
#include "core/RadarConfig.h"
#include "dsp/Detection.h"

namespace radar {

class CfarStage : public IStage {
public:
  explicit CfarStage(const RadarConfig &cfg, CfarParams params = {});

  const char *name() const override { return "CA-CFAR"; }

  // 读 ctx.rdMap，填 ctx.detections（可能为空；只要 rdMap 存在
  // 且维度匹配就绝不为 null）。
  bool process(FrameContext &ctx) override;

private:
  RadarConfig cfg_;
  CfarParams p_;
};

} // namespace radar
