#pragma once
// PhaseCsvSink.h — appends "frameSeq,unwrappedPhaseRad,displacementMm" per
// frame so the respiration/heartbeat waveform can be plotted directly.
// Runs on the pipeline worker (IResultSink fan-out), so no locking needed.

#include <cmath>
#include <fstream>
#include <string>

#include "core/Interfaces.h"

namespace radar {

class PhaseCsvSink : public IResultSink {
public:
  explicit PhaseCsvSink(const std::string &path) : out_(path) {
    if (out_)
      out_ << "frameSeq,unwrappedPhaseRad,displacementMm\n";
  }

  void consume(const FrameContext &ctx) override {
    if (!out_ || !ctx.valid || std::isnan(ctx.unwrappedPhaseRad))
      return;
    out_ << ctx.frameSeq << ',' << ctx.unwrappedPhaseRad << ','
         << ctx.displacementMm << '\n';
  }

  void flush() override { out_.flush(); }

private:
  std::ofstream out_;
};

} // namespace radar
