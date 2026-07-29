#pragma once
// PhaseUnwrapStage.h — slow-time phase unwrapping at one tracked range bin
// (vital signs: chest displacement -> respiration/heartbeat waveform).
//
// Stateful stage (single-writer safe: Pipeline runs stages on one worker).
// Three mechanisms fix the plateau/cliff artifacts measured on real data:
//
// 1. Peak following with hysteresis (followPeak, targetRangeBin < 0 only):
//    every frame the strongest bin in the range gate is a switch candidate;
//    we switch only after the SAME candidate beats the current bin by
//    switchRatio for switchHoldFrames consecutive frames (no bin ping-pong).
//
// 2. Phase bridging on a bin switch: the first sample of the new bin
//    contributes a zero increment (prevRaw_ is re-seeded), so switching the
//    observation point never injects a bogus <=pi phase step and the
//    displacement track stays continuous.
//
// 3. DC compensation (Kasa circle fit): the bin signal is v = S + a*e^{j*phi}
//    (static multipath/skirt leakage S + vibrating chest a). arg(v) is pinned
//    to arg(S) when |S| >> a (plateaus) and spins wildly when v sweeps near
//    the origin (cliffs). We keep a sliding window of frame-mean phasors,
//    least-squares fit the circle center (closed-form Kasa), and take
//    atan2 on (v - center): the residual phase is the clean 4*pi*d/lambda.
//    Degenerate fits (window not full, identical points, collinear arc)
//    conservatively keep the previous center — never extrapolate. Enabling
//    compensation mid-track adds a one-time bounded (<=pi) offset; the
//    relative waveform is unaffected. A truly static target stays
//    uncompensated (zero-span window is rejected as degenerate).
//
// Unwrap core: per chirp phi = atan2 at (chirp, rxIdx, bin); increments are
// wrapped into (-pi, pi] via remainder(d, 2*pi) and accumulated (valid while
// inter-sample motion < lambda/4). Frame output = mean of the frame's
// unwrapped chirp phases; displacement = lambda * delta_phi / (4*pi).
//
// Quality fields (emitted in place, per frame): phaseTrackBin + phaseTrackAmp
// (raw chirp-mean magnitude of the tracked bin). Raw amplitude collapse marks
// the frames where the composite phasor passed near the origin — exactly
// where the output phase depends most on center-estimation accuracy — so
// consumers can gate on it.

#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/Interfaces.h"
#include "core/RadarConfig.h"

namespace radar {

struct PhaseUnwrapParams {
    int targetRangeBin = -1;   // -1 => auto: lock + follow strongest bin in gate
    float minRangeM = 0.3f;    // gate (used when cfg.rangeIdxToMeters > 0)
    float maxRangeM = 2.5f;
    int rxIdx = 0;             // antenna used for the phase track

    // Peak following (targetRangeBin < 0 only).
    bool followPeak = true;
    int switchHoldFrames = 5;   // candidate must persist this many frames
    float switchRatio = 1.25f;  // and beat the current bin by this amplitude ratio

    // Kasa circle-fit DC compensation.
    bool dcCompensation = true;
    int dcWindowFrames = 128;   // sliding window of frame-mean phasors
};

class PhaseUnwrapStage : public IStage {
public:
    PhaseUnwrapStage(const RadarConfig& cfg, PhaseUnwrapParams params = {});

    const char* name() const override { return "PhaseUnwrap"; }

    // Reads ctx.rangeCube; fills ctx.unwrappedPhaseRad / ctx.displacementMm /
    // ctx.phaseTrackBin / ctx.phaseTrackAmp.
    bool process(FrameContext& ctx) override;

    // Currently tracked range bin (-1 before the first lock).
    int targetBin() const noexcept { return bin_; }
    // Number of hysteresis-approved bin switches so far.
    int switchCount() const noexcept { return switches_; }

private:
    int selectBin(const FrameBuffer& cube, std::size_t rx) const;
    double meanAmp(const FrameBuffer& cube, std::size_t rx, int b) const;
    void fitDcCenter();  // Kasa LSQ over the window; keeps old center if degenerate

    RadarConfig cfg_;
    PhaseUnwrapParams p_;

    // Tracked bin + hysteresis state.
    int bin_ = -1;
    int candBin_ = -1;
    int candStreak_ = 0;
    int switches_ = 0;
    bool bridgePending_ = false;

    // Unwrap accumulator.
    bool hasPrev_ = false;
    double prevRaw_ = 0.0;    // last wrapped phase (rad)
    double unwrapped_ = 0.0;  // running unwrapped phase (rad)
    double baseMean_ = 0.0;   // first frame's mean (displacement zero point)
    bool hasBase_ = false;

    // DC compensation state (ring of frame-mean phasors at the tracked bin).
    std::vector<std::complex<double>> dcBuf_;
    std::size_t dcHead_ = 0, dcCount_ = 0;
    std::complex<double> dcCenter_{0.0, 0.0};
    bool dcValid_ = false;
};

}  // namespace radar
