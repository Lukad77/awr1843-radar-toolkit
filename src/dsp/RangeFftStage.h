#pragma once
// RangeFftStage.h — per-chirp/per-rx range FFT: parsed [chirp][rx][sample]
// -> rangeCube [chirp][rx][rangeBin].
//
// Per row: optional DC removal (subtract mean to suppress the zero-range
// leakage), Hann window, zero-pad to numRangeBins (pow2), in-place radix-2
// FFT. The sample axis is the innermost (contiguous) dimension of FrameBuffer,
// so every transform runs over contiguous memory. Window + plan + output pool
// are prepared at construction: process() is allocation-free in steady state.

#include <memory>
#include <vector>

#include "core/BufferPool.h"
#include "core/FrameBuffer.h"
#include "core/Interfaces.h"
#include "core/RadarConfig.h"
#include "dsp/Fft.h"

namespace radar {

class RangeFftStage : public IStage {
public:
    explicit RangeFftStage(const RadarConfig& cfg,
                           std::shared_ptr<BufferPool<FrameBuffer>> pool = nullptr,
                           bool removeDc = true);

    const char* name() const override { return "RangeFFT"; }

    // Reads ctx.parsed, fills ctx.rangeCube. Invalid/absent input passes
    // through untouched (flagged upstream, never a silent drop).
    bool process(FrameContext& ctx) override;

private:
    RadarConfig cfg_;
    FftPlan plan_;              // numRangeBins
    std::vector<float> win_;    // Hann over numAdcSamples
    std::shared_ptr<BufferPool<FrameBuffer>> pool_;
    bool removeDc_;
};

}  // namespace radar
