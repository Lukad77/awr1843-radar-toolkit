#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace radar {

// DCA1000 raw UDP: uint32 sequence, uint48 byte count, then ADC bytes.
// Only complete frames are emitted. A byte gap discards every touched frame.
class Dca1000Reassembler {
public:
  using FrameHandler = std::function<bool(std::uint64_t, const std::vector<std::uint8_t> &)>;
  explicit Dca1000Reassembler(std::size_t frameBytes, FrameHandler handler);
  bool consume(const std::uint8_t *packet, std::size_t size);
  std::uint64_t frames() const { return frames_; }
  std::uint64_t discardedFrames() const { return discardedFrames_; }
  std::uint64_t missingPackets() const { return missingPackets_; }
  std::uint64_t latePackets() const { return latePackets_; }
  std::uint64_t malformedPackets() const { return malformedPackets_; }

private:
  std::size_t frameBytes_;
  FrameHandler handler_;
  std::vector<std::uint8_t> frame_;
  std::uint64_t expectedByte_ = 0;
  std::uint32_t expectedSeq_ = 0;
  bool started_ = false;
  std::uint64_t frames_ = 0, discardedFrames_ = 0, missingPackets_ = 0;
  std::uint64_t latePackets_ = 0, malformedPackets_ = 0;
};

} // namespace radar
