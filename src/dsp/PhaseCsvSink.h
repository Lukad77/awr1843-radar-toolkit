#pragma once
// PhaseCsvSink.h — 逐帧追加 "frameSeq,unwrappedPhaseRad,displacementMm,
// trackBin,trackAmp"：波形加上幅度门控所需的质量列
// （trackAmp 塌陷 == 相位采样不可靠）。
// 运行在流水线 worker 上（IResultSink 扇出），无需加锁。

#include <cmath>
#include <fstream>
#include <string>

#include "core/Interfaces.h"

namespace radar {

class PhaseCsvSink : public IResultSink {
public:
  explicit PhaseCsvSink(const std::string &path) : out_(path) {
    if (out_)
      out_ << "frameSeq,unwrappedPhaseRad,displacementMm,trackBin,trackAmp\n";
  }

  void consume(const FrameContext &ctx) override {
    if (!out_ || !ctx.valid || std::isnan(ctx.unwrappedPhaseRad))
      return;
    out_ << ctx.frameSeq << ',' << ctx.unwrappedPhaseRad << ','
         << ctx.displacementMm << ',' << ctx.phaseTrackBin << ','
         << ctx.phaseTrackAmp << '\n';
  }

  void flush() override { out_.flush(); }

private:
  std::ofstream out_;
};

} // namespace radar
