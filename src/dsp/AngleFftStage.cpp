#include "dsp/AngleFftStage.h"

#include <algorithm>
#include <cmath>

namespace radar {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

AngleFftStage::AngleFftStage(const RadarConfig &cfg)
    : cfg_(cfg), plan_(static_cast<std::size_t>(cfg.numAngleBins)),
      scratch_(static_cast<std::size_t>(cfg.numAngleBins)),
      supported_(cfg.numTxAnt == 1) {}

bool AngleFftStage::process(FrameContext &ctx) {
  if (!ctx.valid || !ctx.dopplerCube || !ctx.detections)
    return true;
  if (!supported_)
    return true; // TDM-MIMO: angles stay NaN (documented)

  const FrameBuffer &cube = *ctx.dopplerCube;
  const FrameShape s = cube.shape(); // {doppler, rx, rangeBins}
  if (s.rx < 2)
    return true; // single Rx: no aperture, angles stay NaN

  const std::size_t N = plan_.size();
  const std::size_t half = N / 2;

  for (Detection &det : *ctx.detections) {
    if (det.dopplerBin < 0 ||
        static_cast<std::size_t>(det.dopplerBin) >= s.chirps ||
        det.rangeBin < 0 || static_cast<std::size_t>(det.rangeBin) >= s.samples)
      continue;

    std::fill(scratch_.begin(), scratch_.end(), std::complex<float>{0.f, 0.f});
    for (std::size_t r = 0; r < s.rx && r < N; ++r)
      scratch_[r] = cube.at(static_cast<std::size_t>(det.dopplerBin), r,
                            static_cast<std::size_t>(det.rangeBin));
    plan_.forward(scratch_.data());

    // Peak search in shifted order: shifted p maps to unshifted (p+N/2)%N.
    std::size_t bestP = half;
    float bestMag = -1.f;
    for (std::size_t p = 0; p < N; ++p) {
      const float m = std::norm(scratch_[(p + half) % N]);
      if (m > bestMag) {
        bestMag = m;
        bestP = p;
      }
    }

    const double kk = static_cast<double>(bestP) - static_cast<double>(half);
    double sinTheta = 2.0 * kk / static_cast<double>(N);
    sinTheta = std::max(-1.0, std::min(1.0, sinTheta));
    det.angleDeg = static_cast<float>(std::asin(sinTheta) * 180.0 / kPi);
  }
  return true;
}

} // namespace radar
