#include "dsp/PhaseUnwrapStage.h"

#include <algorithm>
#include <cmath>

namespace radar {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

PhaseUnwrapStage::PhaseUnwrapStage(const RadarConfig& cfg, PhaseUnwrapParams params)
    : cfg_(cfg), p_(params) {
    if (p_.targetRangeBin >= 0) bin_ = p_.targetRangeBin;
}

int PhaseUnwrapStage::selectBin(const FrameBuffer& cube) const {
    const FrameShape s = cube.shape();  // {chirps, rx, rangeBins}
    const std::size_t rx = std::min<std::size_t>(
        static_cast<std::size_t>(std::max(p_.rxIdx, 0)), s.rx - 1);

    // Range gate in bins; DC bin excluded. Falls back to the full band when
    // the config has no profile info (rangeIdxToMeters == 0, e.g. unit tests).
    int lo = 1, hi = static_cast<int>(s.samples) - 1;
    if (cfg_.rangeIdxToMeters > 0.f) {
        lo = std::max(1, static_cast<int>(p_.minRangeM / cfg_.rangeIdxToMeters));
        hi = std::min(hi, static_cast<int>(p_.maxRangeM / cfg_.rangeIdxToMeters));
        if (lo > hi) { lo = 1; hi = static_cast<int>(s.samples) - 1; }
    }

    int best = lo;
    double bestMag = -1.0;
    for (int b = lo; b <= hi; ++b) {
        double m = 0.0;
        for (std::size_t c = 0; c < s.chirps; ++c)
            m += std::abs(cube.at(c, rx, static_cast<std::size_t>(b)));
        if (m > bestMag) {
            bestMag = m;
            best = b;
        }
    }
    return best;
}

bool PhaseUnwrapStage::process(FrameContext& ctx) {
    if (!ctx.valid || !ctx.rangeCube) return true;

    const FrameBuffer& cube = *ctx.rangeCube;
    const FrameShape s = cube.shape();
    if (s.chirps == 0 || s.rx == 0 || s.samples == 0) return true;

    const std::size_t rx = std::min<std::size_t>(
        static_cast<std::size_t>(std::max(p_.rxIdx, 0)), s.rx - 1);

    // (Re-)lock the tracked bin when unset or when the relock period elapses.
    const bool relockDue = p_.relockPeriodFrames > 0 &&
                           framesSeen_ % static_cast<std::uint64_t>(p_.relockPeriodFrames) == 0;
    if (p_.targetRangeBin < 0 && (bin_ < 0 || relockDue)) bin_ = selectBin(cube);
    ++framesSeen_;
    if (bin_ < 0 || static_cast<std::size_t>(bin_) >= s.samples) return true;

    double sumUnwrapped = 0.0;
    for (std::size_t c = 0; c < s.chirps; ++c) {
        const FrameBuffer::Sample v = cube.at(c, rx, static_cast<std::size_t>(bin_));
        const double phi = std::atan2(static_cast<double>(v.imag()),
                                      static_cast<double>(v.real()));
        if (!hasPrev_) {
            unwrapped_ = phi;
            hasPrev_ = true;
        } else {
            // Wrap the increment into (-pi, pi]: remainder() rounds to nearest.
            unwrapped_ += std::remainder(phi - prevRaw_, 2.0 * kPi);
        }
        prevRaw_ = phi;
        sumUnwrapped += unwrapped_;
    }

    const double mean = sumUnwrapped / static_cast<double>(s.chirps);
    if (!hasBase_) {
        baseMean_ = mean;
        hasBase_ = true;
    }

    ctx.unwrappedPhaseRad = static_cast<float>(mean);
    ctx.displacementMm = static_cast<float>(
        (mean - baseMean_) * static_cast<double>(cfg_.lambdaM) / (4.0 * kPi) * 1000.0);
    return true;
}

}  // namespace radar
