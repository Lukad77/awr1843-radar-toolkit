#pragma once
// Detection.h — CFAR detection record + CA-CFAR tuning parameters.
//
// A Detection is born in CfarStage (range/doppler/snr filled from the RD map)
// and enriched in AngleFftStage (angleDeg). Physical units are derived from
// RadarConfig (rangeIdxToMeters / dopplerResolutionMps), so a Detection is
// self-contained for sinks and does not need the config downstream.

#include <limits>

namespace radar {

struct Detection {
    int rangeBin = 0;    // index into the (non-shifted) range axis
    int dopplerBin = 0;  // index into the fftshifted doppler axis (center = 0 m/s)
    float rangeM = 0.f;
    float velocityMps = 0.f;
    float snrDb = 0.f;
    // Azimuth from the angle FFT; NaN until AngleFftStage runs (or when the
    // config cannot support angle estimation, e.g. single Rx).
    float angleDeg = std::numeric_limits<float>::quiet_NaN();
};

struct CfarParams {
    int guardCells = 2;      // one-sided guard cells around the CUT
    int trainingCells = 8;   // one-sided training cells beyond the guards
    float pfa = 1e-4f;       // design false-alarm probability
    int maxDetections = 128; // hard cap per frame (keeps worst-case work bounded)
};

}  // namespace radar
