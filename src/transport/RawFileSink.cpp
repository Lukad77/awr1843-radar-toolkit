#include "transport/RawFileSink.h"

#include <utility>

namespace radar {

RawFileSink::RawFileSink(const std::string &outputPath, std::size_t frameBytes)
    : output_(outputPath), frameBytes_(frameBytes) {
  if (output_.empty()) {
    err_ = "output path is empty";
    return;
  }
  if (frameBytes_ == 0) {
    err_ = "frameBytes must be > 0";
    return;
  }
  raw_.open(output_, std::ios::binary | std::ios::trunc);
  index_.open(output_ + ".frames.csv", std::ios::trunc);
  if (!raw_ || !index_) {
    err_ = "cannot create output files: " + output_;
    return;
  }
  index_ << "file_frame,wire_frame\n";
  ok_ = true;
}

RawFileSink::~RawFileSink() { flush(); } // flush() 幂等

void RawFileSink::consume(const FrameContext &ctx) {
  if (!ok_) return;
  if (!ctx.valid || !ctx.raw) { // 残帧绝不落盘
    ++skipped_;
    return;
  }
  if (ctx.raw->size() != frameBytes_) {
    ++short_;
    return;
  }
  raw_.write(reinterpret_cast<const char *>(ctx.raw->data()),
             static_cast<std::streamsize>(ctx.raw->size()));
  index_ << saved_ << ',' << ctx.wireSeqStart << '\n';
  if (!raw_ || !index_) {
    err_ = "output write failed: " + output_;
    ok_ = false;
    return;
  }
  ++saved_;
}

void RawFileSink::flush() {
  if (flushed_) return;
  flushed_ = true;
  raw_.flush();
  index_.flush();

  std::ofstream stats(output_ + ".stats.txt", std::ios::trunc);
  stats << "savedFrames=" << saved_ << '\n'
        << "frameBytes=" << frameBytes_ << '\n'
        << "skippedFrames=" << skipped_ << '\n'
        << "shortFrames=" << short_ << '\n';
  if (extra_) stats << extra_();
  if (!ok_) stats << "writeError=1\n";
}

} // namespace radar
