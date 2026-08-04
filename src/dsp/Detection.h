#pragma once
// Detection.h — CFAR 检测记录 + CA-CFAR 调参参数。
//
// Detection 在 CfarStage 中产生（range/doppler/snr 由 RD 图填充），
// 在 AngleFftStage 中补充 angleDeg。物理单位由 RadarConfig 派生
// （rangeIdxToMeters / dopplerResolutionMps），因此 Detection 对 sink
// 而言自洽，下游无需再拿配置。

#include <limits>

namespace radar {

struct Detection {
  int rangeBin = 0;   // （未 fftshift 的）距离轴索引
  int dopplerBin = 0; // 已 fftshift 的多普勒轴索引（中心 = 0 m/s）
  float rangeM = 0.f;
  float velocityMps = 0.f;
  float snrDb = 0.f;
  // 角度 FFT 给出的方位角；AngleFftStage 运行前为 NaN（或配置
  // 不支持测角时，如单 Rx）。
  float angleDeg = std::numeric_limits<float>::quiet_NaN();
};

struct CfarParams {
  int guardCells = 2;      // CUT 两侧各自的保护单元数
  int trainingCells = 8;   // 保护单元外两侧各自的训练单元数
  float pfa = 1e-4f;       // 设计虚警概率
  int maxDetections = 128; // 每帧硬上限（限制最坏情况计算量）
};

} // namespace radar
