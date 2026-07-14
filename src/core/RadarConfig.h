#pragma once
// RadarConfig.h — single source of truth for acquisition + processing params.
//
// Consolidates what used to be three drifting representations (RadarParams,
// AWR1843Controller::ConfigParams, and the loose frameCfg fields). Primary
// fields are set from the .cfg / user; derive() computes every dependent value
// (bytesPerFrame, numRangeBins, resolutions, ...) exactly once, and validate()
// catches inconsistencies before they corrupt frame reassembly/parsing.

#include <string>

namespace radar {

struct RadarConfig {
    // ---- primary: data format ----
    int  numAdcBits = 16;
    bool isReal = false;      // false => complex I/Q (4 bytes/sample)
    int  numRxAnt = 4;
    int  numTxAnt = 1;
    int  numAdcSamples = 256;
    int  rxIdx = 0;           // selected Rx for single-Rx parse (0-based)

    // ---- primary: frameCfg ----
    int   chirpStartIdx = 0;
    int   chirpEndIdx = 0;
    int   numLoops = 0;
    int   numFrames = 0;
    float framePeriodicityMs = 0.f;
    int   triggerSelect = 0;
    float triggerDelay = 0.f;

    // ---- primary: profileCfg ----
    float startFreqGHz = 0.f;
    float idleTimeUs = 0.f;
    float rampEndTimeUs = 0.f;
    float freqSlopeMHzPerUs = 0.f;
    int   digOutSampleRateKsps = 0;

    // ---- derived (filled by derive()) ----
    int  numChirpsPerFrame = 0;
    int  numDopplerBins = 0;
    int  numRangeBins = 0;
    int  bytesPerSample = 4;         // 4 (complex int16) or 2 (real int16)
    long bytesPerFrame = 0;          // chirps * rx * samples * bytesPerSample
    float rangeResolutionMeters = 0.f;
    float rangeIdxToMeters = 0.f;
    float dopplerResolutionMps = 0.f;
    float maxRange = 0.f;
    float maxVelocity = 0.f;

    // Compute all derived fields from primary fields. Idempotent.
    void derive();

    // Returns true if consistent; otherwise fills `err` with the reasons.
    bool validate(std::string& err) const;
};

} // namespace radar
