#pragma once
// DopplerFftStage.h — 跨 chirp 的慢时间 FFT：rangeCube [chirp][rx][bin]
// -> dopplerCube [doppler][rx][bin]（已 fftshift，零多普勒居中），
// 并生成供 CFAR 使用的非相干积累 RD 图。
//
// chirp 轴在内存中是跨步的；每个 (rx, rangeBin) 列先 gather 到小
// scratch 向量（numChirps 个元素，缓存开销可忽略），加窗、变换，
// 再带着 fftshift scatter 回去。rdMap 存线性功率（|.|^2 沿 rx 求和）
// —— CFAR 需要线性域；仅在显示时才转 dB。

#include <memory>
#include <vector>

#include "core/BufferPool.h"
#include "core/FrameBuffer.h"
#include "core/Interfaces.h"
#include "core/RadarConfig.h"
#include "dsp/Fft.h"

namespace radar {

class DopplerFftStage : public IStage {
public:
  explicit DopplerFftStage(
      const RadarConfig &cfg,
      std::shared_ptr<BufferPool<FrameBuffer>> pool = nullptr);

  const char *name() const override { return "DopplerFFT"; }

  // 读 ctx.rangeCube，填 ctx.dopplerCube + ctx.rdMap。
  bool process(FrameContext &ctx) override;

private:
  RadarConfig cfg_;
  FftPlan plan_;                             // numChirpsPerFrame 点
  std::vector<float> win_;                   // 覆盖 chirp 维的 Hann 窗
  std::vector<std::complex<float>> scratch_; // 单个 chirp 列的暂存
  std::shared_ptr<BufferPool<FrameBuffer>> pool_;
};

} // namespace radar
