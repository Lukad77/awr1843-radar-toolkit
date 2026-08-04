#pragma once
// FrameContext.h — 流经流水线的工作单元。
//
// 携带单调递增的 uint64 `frameSeq`（端到端保序检查与丢帧检测的依据），
// 以及原始/已解析载荷和用于延迟指标的时间戳。缓冲区用 shared_ptr 持有，
// 便于池化复用（见 BufferPool）并在各 stage 间廉价传递、避免深拷贝。

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "core/FrameBuffer.h"

namespace radar {

// 定义在 dsp/Detection.h；这里保持不完整声明，避免 core/ 反向依赖 dsp/
// （C++17 允许在声明处使用不完整类型的 vector<T>）。
struct Detection;

struct FrameContext {
  // 应用层单调帧计数器。uint64 => 实际上不会溢出。
  std::uint64_t frameSeq = 0;

  // 组成本帧的首个 DCA1000 包 seqNum（回绕感知，uint32）。
  std::uint32_t wireSeqStart = 0;

  // 序号缺口/乱序导致本帧不完整时置 false：必须标记并计数，
  // 绝不允许把残帧静默往下游传。
  bool valid = true;

  // 接收/重组后的原始字节（解析后可能为空）。
  std::shared_ptr<std::vector<std::uint8_t>> raw;

  // 已解析复数张量 [chirp][rx][sample]（解析前为空）。
  std::shared_ptr<FrameBuffer> parsed;

  // ---- DSP 产物（由 dsp/ 各 stage 逐步填充；产出前为空）----
  // 距离 FFT 输出 [chirp][rx][rangeBin]。
  std::shared_ptr<FrameBuffer> rangeCube;
  // 多普勒 FFT 输出 [doppler][rx][rangeBin]，已 fftshift（零多普勒居中）。
  std::shared_ptr<FrameBuffer> dopplerCube;
  // 距离-多普勒图：线性功率（|.|^2 沿 rx 求和），行优先
  // [doppler * numRangeBins + rangeBin]。仅在显示时才转 dB。
  std::shared_ptr<std::vector<float>> rdMap;
  // CA-CFAR 检测结果（angle 由 AngleFftStage 稍后填充）。
  std::shared_ptr<std::vector<Detection>> detections;
  // 被跟踪 range bin 的慢时间解缠相位（生命体征）；
  // PhaseUnwrapStage 运行前为 NaN。
  float unwrappedPhaseRad = std::numeric_limits<float>::quiet_NaN();
  float displacementMm = std::numeric_limits<float>::quiet_NaN();
  // 相位跟踪质量位：跟踪 bin + 该 bin 的原始 chirp 均幅度。
  // 幅度塌陷标记相位不可靠的帧（合成相量靠近原点）=> 消费端据此门控。
  int phaseTrackBin = -1;
  float phaseTrackAmp = std::numeric_limits<float>::quiet_NaN();

  // 用于端到端延迟测量的时间戳。
  std::chrono::steady_clock::time_point tCaptured{};
};

} // namespace radar
