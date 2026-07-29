#include "dsp/RangeFftStage.h"

namespace radar {

RangeFftStage::RangeFftStage(const RadarConfig &cfg,
                             std::shared_ptr<BufferPool<FrameBuffer>> pool,
                             bool removeDc)
    : cfg_(cfg), plan_(static_cast<std::size_t>(cfg.numRangeBins)),
      win_(makeHannWindow(static_cast<std::size_t>(cfg.numAdcSamples))),
      pool_(std::move(pool)), removeDc_(removeDc) {}

bool RangeFftStage::process(FrameContext &ctx) {
  if (!ctx.valid || !ctx.parsed)
    return true; // flagged upstream, pass through

  const FrameBuffer &in = *ctx.parsed;
  const FrameShape s = in.shape(); // {chirps, rx, samples}
  if (s.samples != static_cast<std::size_t>(cfg_.numAdcSamples)) {
    ctx.valid = false; // config drift: flag, never process garbage
    return true;
  }

  const std::size_t nBins = plan_.size();
  auto out = pool_ ? pool_->acquire() : std::make_shared<FrameBuffer>();
  out->resize(FrameShape{s.chirps, s.rx, nBins});

  for (std::size_t c = 0; c < s.chirps; ++c) {
    for (std::size_t r = 0; r < s.rx; ++r) {
      const FrameBuffer::Sample *src = in.data() + in.index(c, r, 0);
      FrameBuffer::Sample *dst = out->data() + out->index(c, r, 0);

      std::complex<float> mean{0.f, 0.f};
      if (removeDc_) {
        for (std::size_t i = 0; i < s.samples; ++i)
          mean += src[i];
        mean /= static_cast<float>(s.samples);
      }
      for (std::size_t i = 0; i < s.samples; ++i)
        dst[i] = (src[i] - mean) * win_[i];
      for (std::size_t i = s.samples; i < nBins; ++i)
        dst[i] = {0.f, 0.f};

      plan_.forward(dst); // in-place, contiguous row
    }
  }

  ctx.rangeCube = std::move(out);
  return true;
}

} // namespace radar
