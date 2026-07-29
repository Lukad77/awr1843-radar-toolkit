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
    if (p_.dcWindowFrames < 2) p_.dcWindowFrames = 2;
    dcBuf_.resize(static_cast<std::size_t>(p_.dcWindowFrames));
}

int PhaseUnwrapStage::selectBin(const FrameBuffer& cube, std::size_t rx) const {
    const FrameShape s = cube.shape();  // {chirps, rx, rangeBins}

    // Range gate in bins; DC bin excluded. Falls back to the full band when
    // the config has no profile info (rangeIdxToMeters == 0, e.g. unit tests).
    int lo = 1, hi = static_cast<int>(s.samples) - 1;
    if (cfg_.rangeIdxToMeters > 0.f) {
        lo = std::max(1, static_cast<int>(p_.minRangeM / cfg_.rangeIdxToMeters));
        hi = std::min(hi, static_cast<int>(p_.maxRangeM / cfg_.rangeIdxToMeters));
        if (lo > hi) {
            lo = 1;
            hi = static_cast<int>(s.samples) - 1;
        }
    }

    int best = lo;
    double bestMag = -1.0;
    for (int b = lo; b <= hi; ++b) {
        const double m = meanAmp(cube, rx, b);
        if (m > bestMag) {
            bestMag = m;
            best = b;
        }
    }
    return best;
}

double PhaseUnwrapStage::meanAmp(const FrameBuffer& cube, std::size_t rx, int b) const {
    const FrameShape s = cube.shape();
    if (b < 0 || static_cast<std::size_t>(b) >= s.samples || s.chirps == 0) return 0.0;
    double m = 0.0;
    for (std::size_t c = 0; c < s.chirps; ++c)
        m += std::abs(cube.at(c, rx, static_cast<std::size_t>(b)));
    return m / static_cast<double>(s.chirps);
}

void PhaseUnwrapStage::fitDcCenter() {
    // Kasa least-squares circle fit in mean-centered coordinates:
    //   [Suu Suv][uc]   [ (Suuu + Suvv)/2 ]
    //   [Suv Svv][vc] = [ (Svuu + Svvv)/2 ],  center = pointMean + (uc, vc).
    // Degenerate (identical points / collinear / tiny arc): keep old center.
    const std::size_t n = dcCount_;
    if (n < 3) return;

    double mx = 0.0, my = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        mx += dcBuf_[i].real();
        my += dcBuf_[i].imag();
    }
    mx /= static_cast<double>(n);
    my /= static_cast<double>(n);

    double Suu = 0, Suv = 0, Svv = 0, Suuu = 0, Svvv = 0, Suvv = 0, Svuu = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double u = dcBuf_[i].real() - mx;
        const double v = dcBuf_[i].imag() - my;
        Suu += u * u;
        Suv += u * v;
        Svv += v * v;
        Suuu += u * u * u;
        Svvv += v * v * v;
        Suvv += u * v * v;
        Svuu += v * u * u;
    }

    const double det = Suu * Svv - Suv * Suv;
    const double spread = Suu + Svv;
    // Relative conditioning gate: a thin/degenerate arc gives det << spread^2.
    if (spread <= 0.0 || det <= 1e-3 * spread * spread) return;  // keep old center

    const double b1 = 0.5 * (Suuu + Suvv);
    const double b2 = 0.5 * (Svvv + Svuu);
    const double uc = (b1 * Svv - b2 * Suv) / det;
    const double vc = (b2 * Suu - b1 * Suv) / det;

    dcCenter_ = {mx + uc, my + vc};
    dcValid_ = true;
}

bool PhaseUnwrapStage::process(FrameContext& ctx) {
    if (!ctx.valid || !ctx.rangeCube) return true;

    const FrameBuffer& cube = *ctx.rangeCube;
    const FrameShape s = cube.shape();
    if (s.chirps == 0 || s.rx == 0 || s.samples == 0) return true;

    const std::size_t rx = std::min<std::size_t>(
        static_cast<std::size_t>(std::max(p_.rxIdx, 0)), s.rx - 1);

    // ---- Bin selection: first lock, then hysteresis-gated peak following ----
    if (bin_ < 0) {
        bin_ = selectBin(cube, rx);  // first lock: no bridge needed
    } else if (p_.targetRangeBin < 0 && p_.followPeak) {
        const int cand = selectBin(cube, rx);
        const bool qualifies =
            cand != bin_ &&
            meanAmp(cube, rx, cand) >
                static_cast<double>(p_.switchRatio) * meanAmp(cube, rx, bin_);
        if (qualifies) {
            if (cand == candBin_) ++candStreak_;
            else { candBin_ = cand; candStreak_ = 1; }
            if (candStreak_ >= p_.switchHoldFrames) {
                bin_ = cand;
                ++switches_;
                bridgePending_ = true;  // new observation point: zero increment
                candBin_ = -1;
                candStreak_ = 0;
                dcHead_ = dcCount_ = 0;  // new bin has a different static phasor
                dcValid_ = false;
            }
        } else {
            candBin_ = -1;
            candStreak_ = 0;
        }
    }
    if (bin_ < 0 || static_cast<std::size_t>(bin_) >= s.samples) return true;

    // ---- Quality metrics + DC window update (raw, uncompensated) ----
    std::complex<double> frameMean{0.0, 0.0};
    double ampSum = 0.0;
    for (std::size_t c = 0; c < s.chirps; ++c) {
        const FrameBuffer::Sample v = cube.at(c, rx, static_cast<std::size_t>(bin_));
        frameMean += std::complex<double>(v.real(), v.imag());
        ampSum += std::abs(v);
    }
    frameMean /= static_cast<double>(s.chirps);
    const double trackAmp = ampSum / static_cast<double>(s.chirps);

    if (p_.dcCompensation) {
        dcBuf_[dcHead_] = frameMean;
        dcHead_ = (dcHead_ + 1) % dcBuf_.size();
        dcCount_ = std::min(dcCount_ + 1, dcBuf_.size());
        fitDcCenter();
    }
    const bool compensate = p_.dcCompensation && dcValid_ && dcCount_ >= 16;

    // ---- Unwrap across chirps (and across frames via prevRaw_) ----
    double sumUnwrapped = 0.0;
    for (std::size_t c = 0; c < s.chirps; ++c) {
        const FrameBuffer::Sample v = cube.at(c, rx, static_cast<std::size_t>(bin_));
        std::complex<double> z(v.real(), v.imag());
        if (compensate) z -= dcCenter_;
        const double phi = std::atan2(z.imag(), z.real());

        if (!hasPrev_) {
            unwrapped_ = phi;
            hasPrev_ = true;
        } else if (bridgePending_ && c == 0) {
            // Bin switch bridge: re-seed the raw reference, zero increment —
            // changing the observation point must not fabricate a phase step.
            bridgePending_ = false;
        } else {
            // Wrap the increment into (-pi, pi]: remainder() rounds to nearest.
            unwrapped_ += std::remainder(phi - prevRaw_, 2.0 * kPi);
        }
        prevRaw_ = phi;
        sumUnwrapped += unwrapped_;
    }
    bridgePending_ = false;

    const double mean = sumUnwrapped / static_cast<double>(s.chirps);
    if (!hasBase_) {
        baseMean_ = mean;
        hasBase_ = true;
    }

    ctx.unwrappedPhaseRad = static_cast<float>(mean);
    ctx.displacementMm = static_cast<float>(
        (mean - baseMean_) * static_cast<double>(cfg_.lambdaM) / (4.0 * kPi) * 1000.0);
    ctx.phaseTrackBin = bin_;
    ctx.phaseTrackAmp = static_cast<float>(trackAmp);
    return true;
}

}  // namespace radar
