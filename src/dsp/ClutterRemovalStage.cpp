#include "dsp/ClutterRemovalStage.h"

#include <algorithm>

namespace radar {

ClutterRemovalStage::ClutterRemovalStage(float alpha)
    : alpha_(std::min(std::max(alpha, 0.f), 1.f)) {}

bool ClutterRemovalStage::process(FrameContext &ctx) {
  if (!ctx.valid || !ctx.rangeCube)
    return true;

  FrameBuffer &cube = *ctx.rangeCube; // 原位修改（见头文件顺序约束）
  const FrameShape s = cube.shape();  // {chirps, rx, rangeBins}
  if (s.chirps == 0 || s.rx == 0 || s.samples == 0)
    return true;

  // 形状漂移（如重新配置）使已积累的杂波图失效。
  if (s.rx != mapRx_ || s.samples != mapBins_) {
    mapRx_ = s.rx;
    mapBins_ = s.samples;
    map_.assign(s.rx * s.samples, {0.f, 0.f});
    mean_.assign(s.rx * s.samples, {0.f, 0.f});
    init_ = false;
  }

  // 按 (rx, bin) 算 chirp 均值：零多普勒（静态）分量的估计。
  const float invC = 1.f / static_cast<float>(s.chirps);
  for (std::size_t r = 0; r < s.rx; ++r) {
    for (std::size_t b = 0; b < s.samples; ++b) {
      std::complex<float> acc{0.f, 0.f};
      for (std::size_t c = 0; c < s.chirps; ++c)
        acc += cube.at(c, r, b);
      mean_[r * s.samples + b] = acc * invC;
    }
  }

  if (!init_) {
    map_ = mean_; // 播种：bin-0 泄漏从第一帧起即被抑制
    init_ = true;
  }

  // 先减去（更新前的）杂波图，再让它随本帧均值自适应。
  for (std::size_t c = 0; c < s.chirps; ++c)
    for (std::size_t r = 0; r < s.rx; ++r) {
      const std::complex<float> *m = map_.data() + r * s.samples;
      FrameBuffer::Sample *row = cube.data() + cube.index(c, r, 0);
      for (std::size_t b = 0; b < s.samples; ++b)
        row[b] -= m[b];
    }
  for (std::size_t i = 0; i < map_.size(); ++i)
    map_[i] = (1.f - alpha_) * map_[i] + alpha_ * mean_[i];

  return true;
}

} // namespace radar
