#include "dsp/ClutterRemovalStage.h"

#include <algorithm>

namespace radar {

ClutterRemovalStage::ClutterRemovalStage(float alpha)
    : alpha_(std::min(std::max(alpha, 0.f), 1.f)) {}

bool ClutterRemovalStage::process(FrameContext& ctx) {
    if (!ctx.valid || !ctx.rangeCube) return true;

    FrameBuffer& cube = *ctx.rangeCube;  // in-place (see header ordering note)
    const FrameShape s = cube.shape();   // {chirps, rx, rangeBins}
    if (s.chirps == 0 || s.rx == 0 || s.samples == 0) return true;

    // Shape drift (e.g. reconfiguration) invalidates the accumulated map.
    if (s.rx != mapRx_ || s.samples != mapBins_) {
        mapRx_ = s.rx;
        mapBins_ = s.samples;
        map_.assign(s.rx * s.samples, {0.f, 0.f});
        mean_.assign(s.rx * s.samples, {0.f, 0.f});
        init_ = false;
    }

    // Chirp-mean per (rx, bin): the zero-Doppler (static) component estimate.
    const float invC = 1.f / static_cast<float>(s.chirps);
    for (std::size_t r = 0; r < s.rx; ++r) {
        for (std::size_t b = 0; b < s.samples; ++b) {
            std::complex<float> acc{0.f, 0.f};
            for (std::size_t c = 0; c < s.chirps; ++c) acc += cube.at(c, r, b);
            mean_[r * s.samples + b] = acc * invC;
        }
    }

    if (!init_) {
        map_ = mean_;  // seed: bin-0 leakage suppressed from the first frame
        init_ = true;
    }

    // Subtract the (pre-update) map, then let it adapt with this frame's mean.
    for (std::size_t c = 0; c < s.chirps; ++c)
        for (std::size_t r = 0; r < s.rx; ++r) {
            const std::complex<float>* m = map_.data() + r * s.samples;
            FrameBuffer::Sample* row = cube.data() + cube.index(c, r, 0);
            for (std::size_t b = 0; b < s.samples; ++b) row[b] -= m[b];
        }
    for (std::size_t i = 0; i < map_.size(); ++i)
        map_[i] = (1.f - alpha_) * map_[i] + alpha_ * mean_[i];

    return true;
}

}  // namespace radar
