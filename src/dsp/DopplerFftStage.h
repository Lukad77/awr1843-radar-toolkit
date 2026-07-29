#pragma once
// DopplerFftStage.h — slow-time FFT across chirps: rangeCube [chirp][rx][bin]
// -> dopplerCube [doppler][rx][bin] (fftshifted, zero Doppler centered) plus
// the non-coherently integrated RD map used by CFAR.
//
// The chirp axis is strided in memory; each (rx, rangeBin) column is gathered
// into a small scratch vector (numChirps elements, cache-trivial), windowed,
// transformed, then scattered back with the fftshift applied. rdMap holds
// linear power (|.|^2 summed over rx) — CFAR needs the linear domain; convert
// to dB only for display.

#include <memory>
#include <vector>

#include "core/BufferPool.h"
#include "core/FrameBuffer.h"
#include "core/Interfaces.h"
#include "core/RadarConfig.h"
#include "dsp/Fft.h"

namespace radar {

class DopplerFftStage : public IStage {
public:
  explicit DopplerFftStage(
      const RadarConfig &cfg,
      std::shared_ptr<BufferPool<FrameBuffer>> pool = nullptr);

  const char *name() const override { return "DopplerFFT"; }

  // Reads ctx.rangeCube, fills ctx.dopplerCube + ctx.rdMap.
  bool process(FrameContext &ctx) override;

private:
  RadarConfig cfg_;
  FftPlan plan_;                             // numChirpsPerFrame
  std::vector<float> win_;                   // Hann over chirps
  std::vector<std::complex<float>> scratch_; // one chirp column
  std::shared_ptr<BufferPool<FrameBuffer>> pool_;
};

} // namespace radar
