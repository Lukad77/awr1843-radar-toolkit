#pragma once
// SeqNum.h — wrap-safe 32-bit sequence-number arithmetic (RFC 1982 style).
//
// The DCA1000 per-packet `seqNum` is uint32 and counts *packets*. At high data
// rates it wraps around within days of continuous capture, so gap/order
// detection MUST NOT compare absolute values. We treat the 32-bit space as
// circular and compare signed differences (exactly like TCP sequence numbers).
//
// The application-level frame counter (`frameSeq`) is uint64 and, for all
// practical purposes, never overflows — so it is compared normally.

#include <cstdint>

namespace radar {

// Signed circular distance a - b in the 32-bit sequence space.
// Positive  => a is "ahead of" b, Negative => a is "behind" b.
inline int32_t seq_diff(uint32_t a, uint32_t b) noexcept {
    return static_cast<int32_t>(a - b);
}

inline bool seq_lt(uint32_t a, uint32_t b) noexcept { return seq_diff(a, b) < 0; }
inline bool seq_le(uint32_t a, uint32_t b) noexcept { return seq_diff(a, b) <= 0; }
inline bool seq_gt(uint32_t a, uint32_t b) noexcept { return seq_diff(a, b) > 0; }
inline bool seq_ge(uint32_t a, uint32_t b) noexcept { return seq_diff(a, b) >= 0; }

// Wrap-safe gap between an expected and a received sequence number.
//   > 0  => `received` is ahead of `expected` by N (i.e. N-1 packets missing
//           if consecutive numbering is expected).
//   == 0 => in order.
//   < 0  => `received` is a stale/duplicate (behind expected).
inline int64_t seq_gap(uint32_t expected, uint32_t received) noexcept {
    return static_cast<int64_t>(seq_diff(received, expected));
}

} // namespace radar
