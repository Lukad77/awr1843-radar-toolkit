#pragma once
// AngleFftStage.h — azimuth estimation per CFAR detection.
//
// For each detection, the complex snapshot across the virtual antenna array
// is taken from dopplerCube at (dopplerBin, rangeBin), zero-padded to
// numAngleBins and FFT'ed. For a lambda/2 uniform linear array the inter-
// element phase is pi*sin(theta), so a shifted peak index kk in
// [-N/2, N/2) maps to sin(theta) = 2*kk/N.
//
// TDM-MIMO (numTxAnt > 1) requires per-Tx Doppler phase compensation before
// the virtual array is coherent; that is a later extension. This stage
// therefore only computes angles for 1-Tx configs (and >= 2 Rx); otherwise
// detections pass through with angleDeg = NaN.

#include <vector>

#include "core/Interfaces.h"
#include "core/RadarConfig.h"
#include "dsp/Detection.h"
#include "dsp/Fft.h"

namespace radar {

class AngleFftStage : public IStage {
public:
    explicit AngleFftStage(const RadarConfig& cfg);

    const char* name() const override { return "AngleFFT"; }

    // Reads ctx.dopplerCube + ctx.detections, fills Detection::angleDeg.
    bool process(FrameContext& ctx) override;

private:
    RadarConfig cfg_;
    FftPlan plan_;                              // numAngleBins
    std::vector<std::complex<float>> scratch_;  // zero-padded snapshot
    bool supported_;                            // 1 Tx only (see header note)
};

}  // namespace radar
