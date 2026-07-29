#pragma once
// CfarStage.h — 1D CA-CFAR along the range axis, run independently per
// Doppler row of the linear-power RD map.
//
// Classic cell-averaging CFAR: noise = mean of training cells on both sides
// of the CUT (guard cells excluded); threshold factor alpha = T*(pfa^(-1/T)-1)
// with T = number of training cells actually used, so edge cells (truncated
// one-sided windows) keep the same design Pfa instead of being skipped.
// A local-peak gate (CUT >= left neighbor, > right neighbor) collapses
// clusters so one target produces one detection.

#include <memory>
#include <vector>

#include "core/Interfaces.h"
#include "core/RadarConfig.h"
#include "dsp/Detection.h"

namespace radar {

class CfarStage : public IStage {
public:
    explicit CfarStage(const RadarConfig& cfg, CfarParams params = {});

    const char* name() const override { return "CA-CFAR"; }

    // Reads ctx.rdMap, fills ctx.detections (possibly empty, never null when
    // rdMap is present and dimensions match).
    bool process(FrameContext& ctx) override;

private:
    RadarConfig cfg_;
    CfarParams p_;
};

}  // namespace radar
