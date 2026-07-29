#include "dsp/DopplerFftStage.h"

namespace radar {

DopplerFftStage::DopplerFftStage(const RadarConfig& cfg,
                                 std::shared_ptr<BufferPool<FrameBuffer>> pool)
    : cfg_(cfg),
      plan_(static_cast<std::size_t>(cfg.numChirpsPerFrame)),
      win_(makeHannWindow(static_cast<std::size_t>(cfg.numChirpsPerFrame))),
      scratch_(static_cast<std::size_t>(cfg.numChirpsPerFrame)),
      pool_(std::move(pool)) {}

bool DopplerFftStage::process(FrameContext& ctx) {
    if (!ctx.valid || !ctx.rangeCube) return true;

    const FrameBuffer& in = *ctx.rangeCube;
    const FrameShape s = in.shape();  // {chirps, rx, rangeBins}
    const std::size_t C = plan_.size();
    if (s.chirps != C) {
        ctx.valid = false;
        return true;
    }

    auto out = pool_ ? pool_->acquire() : std::make_shared<FrameBuffer>();
    out->resize(FrameShape{C, s.rx, s.samples});

    const std::size_t half = C / 2;
    for (std::size_t r = 0; r < s.rx; ++r) {
        for (std::size_t b = 0; b < s.samples; ++b) {
            for (std::size_t c = 0; c < C; ++c)
                scratch_[c] = in.at(c, r, b) * win_[c];
            plan_.forward(scratch_.data());
            // Scatter with fftshift: shifted[d] = unshifted[(d + C/2) % C].
            for (std::size_t d = 0; d < C; ++d)
                out->at(d, r, b) = scratch_[(d + half) % C];
        }
    }

    // Non-coherent integration over rx: linear power RD map for CFAR.
    auto rd = std::make_shared<std::vector<float>>(C * s.samples);
    for (std::size_t d = 0; d < C; ++d) {
        for (std::size_t b = 0; b < s.samples; ++b) {
            float p = 0.f;
            for (std::size_t r = 0; r < s.rx; ++r) p += std::norm(out->at(d, r, b));
            (*rd)[d * s.samples + b] = p;
        }
    }

    ctx.dopplerCube = std::move(out);
    ctx.rdMap = std::move(rd);
    return true;
}

}  // namespace radar
