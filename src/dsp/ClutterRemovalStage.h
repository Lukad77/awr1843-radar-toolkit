#pragma once
// ClutterRemovalStage.h — MTI static clutter suppression on the range cube.
//
// Maintains a per-(rx, rangeBin) clutter map = exponential moving average
// (EMA) of the chirp-mean of rangeCube across frames, and subtracts it from
// every chirp IN PLACE. Truly static returns (antenna coupling at bin 0,
// walls, furniture) converge into the map and vanish from the Doppler/CFAR
// path; anything with per-chirp Doppler rotation has a near-zero chirp-mean
// and passes through untouched.
//
// alpha semantics: map <- (1-alpha)*map + alpha*chirpMean, i.e. the map
// adapts with a time constant of ~1/alpha frames. The first frame seeds the
// map directly (so bin-0 leakage is suppressed from frame 1).
//
// ORDERING CONSTRAINT: this stage modifies rangeCube in place. The residual
// phasor after subtraction no longer satisfies phi = 4*pi*d/lambda, so
// PhaseUnwrapStage (vital signs) must run BEFORE this stage:
//   RangeFFT -> PhaseUnwrap -> ClutterRemoval -> DopplerFFT -> CFAR -> ...
//
// Stateful (map persists across frames) but single-writer safe: Pipeline
// runs all stages on one worker thread.

#include <complex>
#include <vector>

#include "core/Interfaces.h"

namespace radar {

class ClutterRemovalStage : public IStage {
public:
    // alpha in [0, 1]: EMA update weight per frame (~1/alpha frames to adapt).
    explicit ClutterRemovalStage(float alpha = 0.02f);

    const char* name() const override { return "ClutterRemoval"; }

    // Subtracts the clutter map from ctx.rangeCube in place, then updates the
    // map with this frame's chirp-mean. Shape change resets the map.
    bool process(FrameContext& ctx) override;

private:
    float alpha_;
    bool init_ = false;
    std::size_t mapRx_ = 0, mapBins_ = 0;           // shape guard
    std::vector<std::complex<float>> map_;          // [rx * rangeBins]
    std::vector<std::complex<float>> mean_;         // per-frame scratch
};

}  // namespace radar
