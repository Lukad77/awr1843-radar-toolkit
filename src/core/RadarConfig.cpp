#include "core/RadarConfig.h"

#include <cmath>
#include <sstream>

namespace radar {

namespace {
int nextPow2(int n) {
  int p = 1;
  while (p < n)
    p <<= 1;
  return p;
}
} // namespace

void RadarConfig::derive() {
  bytesPerSample = isReal ? 2 : 4;

  // numChirpsPerFrame from frameCfg when available; otherwise keep as-is.
  if (numLoops > 0 && chirpEndIdx >= chirpStartIdx) {
    numChirpsPerFrame = (chirpEndIdx - chirpStartIdx + 1) * numLoops;
  }
  if (numChirpsPerFrame < 0)
    numChirpsPerFrame = 0;

  const int tx = numTxAnt > 0 ? numTxAnt : 1;
  numDopplerBins = numChirpsPerFrame > 0 ? numChirpsPerFrame / tx : 0;
  numRangeBins = numAdcSamples > 0 ? nextPow2(numAdcSamples) : 0;

  bytesPerFrame = static_cast<long>(numChirpsPerFrame) * numRxAnt *
                  numAdcSamples * bytesPerSample;

  // Radar resolution formulas (units: startFreq GHz, times us, slope MHz/us,
  // sample rate ksps). Mirror the legacy AWR1843Controller computation.
  const double c = 3e8;
  const double chirpT = (idleTimeUs + rampEndTimeUs) * 1e-6;

  if (freqSlopeMHzPerUs != 0.f && numAdcSamples > 0) {
    rangeResolutionMeters =
        static_cast<float>((c * digOutSampleRateKsps * 1e3) /
                           (2.0 * freqSlopeMHzPerUs * 1e12 * numAdcSamples));
  }
  if (freqSlopeMHzPerUs != 0.f && numRangeBins > 0) {
    rangeIdxToMeters =
        static_cast<float>((c * digOutSampleRateKsps * 1e3) /
                           (2.0 * freqSlopeMHzPerUs * 1e12 * numRangeBins));
  }
  if (startFreqGHz != 0.f && numDopplerBins > 0 && chirpT != 0.0) {
    dopplerResolutionMps = static_cast<float>(
        c / (2.0 * startFreqGHz * 1e9 * chirpT * numDopplerBins * tx));
  }
  if (freqSlopeMHzPerUs != 0.f) {
    maxRange = static_cast<float>((300.0 * 0.9 * digOutSampleRateKsps) /
                                  (2.0 * freqSlopeMHzPerUs * 1e3));
  }
  if (startFreqGHz != 0.f && chirpT != 0.0) {
    maxVelocity =
        static_cast<float>(c / (4.0 * startFreqGHz * 1e9 * chirpT * tx));
  }

  numVirtualAnt = (numTxAnt > 0 && numRxAnt > 0) ? numTxAnt * numRxAnt : 0;
  lambdaM =
      startFreqGHz != 0.f ? static_cast<float>(c / (startFreqGHz * 1e9)) : 0.f;
}

bool RadarConfig::validate(std::string &err) const {
  std::ostringstream e;
  bool ok = true;
  auto fail = [&](const char *m) {
    ok = false;
    e << m << "; ";
  };

  if (numAdcSamples <= 0 || (numAdcSamples % 2) != 0)
    fail("numAdcSamples must be positive and even");
  if (numRxAnt <= 0)
    fail("numRxAnt must be > 0");
  if (numTxAnt <= 0)
    fail("numTxAnt must be > 0");
  if (rxIdx < 0 || rxIdx >= numRxAnt)
    fail("rxIdx out of range [0, numRxAnt)");
  if (numChirpsPerFrame <= 0)
    fail("numChirpsPerFrame must be > 0 (call derive() first)");
  if (bytesPerFrame <= 0)
    fail("bytesPerFrame must be > 0 (call derive() first)");
  if (!isReal && bytesPerSample != 4)
    fail("complex sample must be 4 bytes");
  if (numAngleBins <= 0 || (numAngleBins & (numAngleBins - 1)) != 0)
    fail("numAngleBins must be a power of two");

  if (!ok)
    err = e.str();
  return ok;
}

} // namespace radar
