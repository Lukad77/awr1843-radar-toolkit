#include "transport/Dca1000Reassembler.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "core/SeqNum.h"

namespace radar {
namespace {
std::uint64_t readLe(const std::uint8_t *p, unsigned n) {
  std::uint64_t v = 0;
  for (unsigned i = 0; i < n; ++i)
    v |= std::uint64_t(p[i]) << (8 * i);
  return v;
}
} // namespace

Dca1000Reassembler::Dca1000Reassembler(std::size_t frameBytes,
                                       FrameHandler handler)
    : frameBytes_(frameBytes), handler_(std::move(handler)) {
  if (!frameBytes_ || !handler_)
    throw std::invalid_argument("frame size and handler are required");
  frame_.reserve(frameBytes_);
}

bool Dca1000Reassembler::consume(const std::uint8_t *packet,
                                 std::size_t size) {
  if (!packet || size <= 10) {
    ++malformedPackets_;
    return true;
  }
  const auto seq = static_cast<std::uint32_t>(readLe(packet, 4));
  std::uint64_t offset = readLe(packet + 4, 6);
  const std::size_t payloadSize = size - 10;
  if (!started_) {
    started_ = true;
    expectedSeq_ = seq;
    expectedByte_ = offset;
    // A capture begun mid-frame must first reach the next frame boundary.
    if (offset % frameBytes_)
      ++discardedFrames_;
  }
  const auto seqDistance = seq_gap(expectedSeq_, seq);
  if (seqDistance < 0 || offset < expectedByte_) {
    ++latePackets_;
    return true;
  }
  if (seqDistance > 0)
    missingPackets_ += static_cast<std::uint64_t>(seqDistance);
  expectedSeq_ = seq + 1;
  if (offset > expectedByte_) {
    // Count affected frame indexes, including one already partially buffered.
    const auto first = (expectedByte_ - frame_.size()) / frameBytes_;
    const auto last = (offset - 1) / frameBytes_;
    discardedFrames_ += last - first + 1;
    frame_.clear();
    expectedByte_ = offset;
  }
  const std::uint8_t *data = packet + 10;
  std::size_t left = payloadSize;
  while (left) {
    const std::size_t pos = static_cast<std::size_t>(offset % frameBytes_);
    const std::size_t take = std::min(left, frameBytes_ - pos);
    if (pos == 0 && frame_.empty())
      frame_.reserve(frameBytes_);
    // Skip a partial frame following a gap or a mid-stream start.
    if (pos == frame_.size()) {
      frame_.insert(frame_.end(), data, data + take);
      if (frame_.size() == frameBytes_) {
        if (!handler_(offset / frameBytes_, frame_))
          return false;
        ++frames_;
        frame_.clear();
      }
    }
    offset += take;
    data += take;
    left -= take;
  }
  expectedByte_ = offset;
  return true;
}
} // namespace radar
