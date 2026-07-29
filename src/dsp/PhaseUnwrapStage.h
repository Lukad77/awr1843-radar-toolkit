#pragma once
// PhaseUnwrapStage.h — slow-time phase unwrapping at one tracked range bin
// (vital signs: chest displacement -> respiration/heartbeat waveform).
//
// Stateful stage (the only one): it carries the unwrap accumulator across
// chirps AND across frames so the phase track is continuous over the whole
// recording. Safe because Pipeline runs all stages on one worker thread
// (single writer, no locks needed).
//
// Bin selection: fixed via params.targetRangeBin, or auto = strongest mean
// magnitude inside [minRangeM, maxRangeM] on the first valid frame (bin 0/DC
// excluded; re-lock optionally every relockPeriodFrames).
//
// Unwrap: per chirp phi = atan2 at (chirp, rxIdx, bin); the increment is
// wrapped into (-pi, pi] via remainder(d, 2*pi) and accumulated. Frame output
// = mean of the frame's unwrapped chirp phases; displacement uses
// delta_d = lambda * delta_phi / (4*pi).

#include <cstddef>
#include <cstdint>

#include "core/Interfaces.h"
#include "core/RadarConfig.h"

namespace radar {

struct PhaseUnwrapParams {
    int targetRangeBin = -1;     // -1 => auto-select strongest bin in range gate
    float minRangeM = 0.3f;      // auto-select gate (used when rangeIdxToMeters > 0)
    float maxRangeM = 2.5f;
    int relockPeriodFrames = 0;  // 0 => lock once and keep the bin
    int rxIdx = 0;               // antenna used for the phase track
};

class PhaseUnwrapStage : public IStage {
public:
    PhaseUnwrapStage(const RadarConfig& cfg, PhaseUnwrapParams params = {});

    const char* name() const override { return "PhaseUnwrap"; }

    // Reads ctx.rangeCube, fills ctx.unwrappedPhaseRad / ctx.displacementMm.
    bool process(FrameContext& ctx) override;

    // Currently tracked range bin (-1 before the first lock).
    int targetBin() const noexcept { return bin_; }

private:
    int selectBin(const FrameBuffer& cube) const;

    RadarConfig cfg_;
    PhaseUnwrapParams p_;

    int bin_ = -1;
    std::uint64_t framesSeen_ = 0;
    bool hasPrev_ = false;
    double prevRaw_ = 0.0;       // last wrapped phase (rad)
    double unwrapped_ = 0.0;     // running unwrapped phase (rad)
    double baseMean_ = 0.0;      // first frame's mean (displacement zero point)
    bool hasBase_ = false;
};

}  // namespace radar
