#pragma once
// SeqNum.h — 回绕安全的 32 位序列号算术（RFC 1982 风格）。
//
// DCA1000 每包的 `seqNum` 是 uint32，按*包*计数。高数据率下连续采集
// 几天内就会回绕，因此缺口/乱序检测绝不能比较绝对值。我们把 32 位
// 空间视为环形，比较有符号差值（与 TCP 序列号完全同理）。
//
// 应用层帧计数器（`frameSeq`）是 uint64，实际上永不溢出，所以正常比较即可。

#include <cstdint>

namespace radar {

// 32 位序列号空间内 a - b 的有符号环形距离。
// 正值 => a 在 b “之前（领先）”，负值 => a 在 b “之后（落后）”。
inline int32_t seq_diff(uint32_t a, uint32_t b) noexcept {
  return static_cast<int32_t>(a - b);
}

inline bool seq_lt(uint32_t a, uint32_t b) noexcept {
  return seq_diff(a, b) < 0;
}
inline bool seq_le(uint32_t a, uint32_t b) noexcept {
  return seq_diff(a, b) <= 0;
}
inline bool seq_gt(uint32_t a, uint32_t b) noexcept {
  return seq_diff(a, b) > 0;
}
inline bool seq_ge(uint32_t a, uint32_t b) noexcept {
  return seq_diff(a, b) >= 0;
}

// 期望序号与实收序号之间的回绕安全缺口。
//   > 0  => `received` 领先 `expected` N 个（若期望连续编号，
//           即中间丢了 N-1 个包）。
//   == 0 => 按序到达。
//   < 0  => `received` 是过期/重复包（落后于期望值）。
inline int64_t seq_gap(uint32_t expected, uint32_t received) noexcept {
  return static_cast<int64_t>(seq_diff(received, expected));
}

} // namespace radar
