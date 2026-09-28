#include <cstdint>
#include <iostream>
#include <utility>
#include <vector>

#include "transport/Dca1000Reassembler.h"

using radar::Dca1000Reassembler;

static std::vector<std::uint8_t> packet(std::uint32_t seq, std::uint64_t pos,
                                        std::uint8_t value, std::size_t bytes) {
  std::vector<std::uint8_t> p(10 + bytes, value);
  for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(seq >> (8 * i));
  for (int i = 0; i < 6; ++i) p[4 + i] = static_cast<std::uint8_t>(pos >> (8 * i));
  return p;
}

int main() {
  int failures = 0;
  auto check = [&](bool v, const char *name) {
    if (!v) { std::cerr << "FAIL " << name << '\n'; ++failures; }
  };
  std::vector<std::pair<std::uint64_t, std::vector<std::uint8_t>>> frames;
  Dca1000Reassembler r(8, [&](std::uint64_t id, const auto &data) {
    frames.emplace_back(id, data); return true;
  });
  auto feed = [&](const std::vector<std::uint8_t> &p) { check(r.consume(p.data(), p.size()), "consume"); };
  feed(packet(0xfffffffe, 0, 1, 5));
  feed(packet(0xffffffff, 5, 2, 7));
  check(frames.size() == 1 && frames[0].first == 0, "cross-packet frame");
  check(frames[0].second == std::vector<std::uint8_t>({1,1,1,1,1,2,2,2}), "frame bytes");
  feed(packet(0, 12, 3, 4));
  check(frames.size() == 2 && frames[1].first == 1, "sequence wrap");
  feed(packet(2, 20, 5, 4)); // seq=1 missing; frame 2 incomplete
  feed(packet(3, 24, 6, 8));
  check(frames.size() == 3 && frames[2].first == 3, "gap recovery");
  check(r.missingPackets() == 1 && r.discardedFrames() == 1, "gap counters");
  feed(packet(1, 16, 4, 4));
  check(r.latePackets() == 1 && frames.size() == 3, "late packet");
  const std::uint8_t invalid[4] = {};
  check(r.consume(invalid, sizeof(invalid)) && r.malformedPackets() == 1, "malformed");
  std::cout << (failures ? "capture tests failed\n" : "capture tests passed\n");
  return failures ? 1 : 0;
}
