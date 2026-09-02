#pragma once
// WireProtocol.h — Web 实时显示的二进制帧协议 v1（纯头文件，无网络依赖）。
//
// 每处理帧编码为一条 WebSocket binary 消息，前端用 DataView/TypedArray
// 零拷贝解码。所有多字节字段小端；本项目全部目标平台（x86/ARM 的
// Windows/Linux/macOS）均为小端，直接 memcpy 写出（文件级假设，若日后
// 支持大端平台需在此处做字节交换）。
//
// 布局（v1，合计 36 + 2*2*nWave + 4*nBins 字节）：
//   偏移  类型        字段
//   0     u32         magic = 0x31574452 ('RDW1' 小端)
//   4     u16         version = 1
//   6     u16         flags（bit0: ctx.valid）
//   8     u64         frameSeq
//   16    f32         unwrappedPhaseRad
//   20    f32         displacementMm
//   24    i32         phaseTrackBin
//   28    f32         phaseTrackAmp
//   32    u16         nWave（原始 ADC 采样点数）
//   34    u16         nBins（距离谱 bin 数）
//   36    i16[nWave]  原始 ADC I（chirp0 / rx0，取自 ctx.parsed）
//   ...   i16[nWave]  原始 ADC Q
//   ...   f32[nBins]  距离谱 dB = 20*log10(|chirp 相干均值| + 1e-6)（rx0）
//
// 扩展约定：新增消息种类时递增 version 或新增独立 magic；前端对不识别
// 的 magic/version 直接丢弃该消息（见 web/app.js）。

#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "core/FrameContext.h"
#include "core/RadarConfig.h"

namespace radar {
namespace wire {

constexpr std::uint32_t kMagic = 0x31574452u; // 'RDW1'
constexpr std::uint16_t kVersion = 1;

// 头部字段偏移（与上文布局一致，供编码与单测双侧使用）。
constexpr std::size_t kOffMagic = 0;
constexpr std::size_t kOffVersion = 4;
constexpr std::size_t kOffFlags = 6;
constexpr std::size_t kOffFrameSeq = 8;
constexpr std::size_t kOffPhaseRad = 16;
constexpr std::size_t kOffDispMm = 20;
constexpr std::size_t kOffTrackBin = 24;
constexpr std::size_t kOffTrackAmp = 28;
constexpr std::size_t kOffNWave = 32;
constexpr std::size_t kOffNBins = 34;
constexpr std::size_t kHeaderBytes = 36;

static_assert(sizeof(float) == 4, "wire 协议假设 IEEE-754 单精度 float");

namespace detail {
template <class T>
inline void put(std::vector<std::uint8_t> &buf, std::size_t off, T v) {
  std::memcpy(buf.data() + off, &v, sizeof(T)); // 小端平台假设见文件头
}

inline std::int16_t clampToI16(float v) {
  if (v > 32767.f)
    return 32767;
  if (v < -32768.f)
    return -32768;
  return static_cast<std::int16_t>(v);
}
} // namespace detail

// 把一帧编码为 wire 包。cfg 用于一致性校验：解析/距离谱的实际形状与
// 配置漂移时（如重配置瞬间）返回空 vector，调用方跳过该帧 —— 绝不
// 发送形状与 meta 声明不符的包。ctx 无效或产物缺失时同样返回空。
inline std::vector<std::uint8_t> encodeFramePacket(const RadarConfig &cfg,
                                                   const FrameContext &ctx) {
  if (!ctx.valid || !ctx.parsed || !ctx.rangeCube)
    return {};

  const FrameShape ps = ctx.parsed->shape();     // {chirps, rx, samples}
  const FrameShape rs = ctx.rangeCube->shape();  // {chirps, rx, rangeBins}
  if (ps.chirps == 0 || ps.rx == 0 || ps.samples == 0 || rs.chirps == 0 ||
      rs.rx == 0 || rs.samples == 0)
    return {};
  if (ps.samples != static_cast<std::size_t>(cfg.numAdcSamples) ||
      rs.samples != static_cast<std::size_t>(cfg.numRangeBins))
    return {};

  const std::size_t nWave = ps.samples;
  const std::size_t nBins = rs.samples;
  std::vector<std::uint8_t> buf(kHeaderBytes + 2 * 2 * nWave + 4 * nBins);

  detail::put(buf, kOffMagic, kMagic);
  detail::put(buf, kOffVersion, kVersion);
  detail::put(buf, kOffFlags,
              static_cast<std::uint16_t>(ctx.valid ? 0x0001u : 0x0000u));
  detail::put(buf, kOffFrameSeq, static_cast<std::uint64_t>(ctx.frameSeq));
  detail::put(buf, kOffPhaseRad, ctx.unwrappedPhaseRad);
  detail::put(buf, kOffDispMm, ctx.displacementMm);
  detail::put(buf, kOffTrackBin, static_cast<std::int32_t>(ctx.phaseTrackBin));
  detail::put(buf, kOffTrackAmp, ctx.phaseTrackAmp);
  detail::put(buf, kOffNWave, static_cast<std::uint16_t>(nWave));
  detail::put(buf, kOffNBins, static_cast<std::uint16_t>(nBins));

  // 原始 ADC 波形：chirp0 / rx0。解析值本就来自 int16，clamp 仅防御。
  const std::size_t offI = kHeaderBytes;
  const std::size_t offQ = offI + 2 * nWave;
  for (std::size_t s = 0; s < nWave; ++s) {
    const std::complex<float> v = ctx.parsed->at(0, 0, s);
    detail::put(buf, offI + 2 * s, detail::clampToI16(v.real()));
    detail::put(buf, offQ + 2 * s, detail::clampToI16(v.imag()));
  }

  // 距离谱：rx0 逐 bin 的 chirp 相干均值 → dB（仅显示用途才转 dB）。
  const std::size_t offP = offQ + 2 * nWave;
  const float invC = 1.f / static_cast<float>(rs.chirps);
  for (std::size_t b = 0; b < nBins; ++b) {
    std::complex<float> acc{0.f, 0.f};
    for (std::size_t c = 0; c < rs.chirps; ++c)
      acc += ctx.rangeCube->at(c, 0, b);
    const float mag = std::abs(acc) * invC;
    detail::put(buf, offP + 4 * b, 20.f * std::log10(mag + 1e-6f));
  }
  return buf;
}

// 客户端接入时下发一次的元数据（文本消息）。数值全部来自 RadarConfig，
// 前端据此换算坐标轴（bin→米）与滤波器系数（fps）。
inline std::string buildMetaJson(const RadarConfig &cfg, float fps,
                                 const std::string &sourceName) {
  std::ostringstream o;
  o << "{\"type\":\"meta\",\"version\":" << kVersion
    << ",\"nWave\":" << cfg.numAdcSamples << ",\"nBins\":" << cfg.numRangeBins
    << ",\"rangeIdxToMeters\":" << cfg.rangeIdxToMeters
    << ",\"fps\":" << fps << ",\"lambdaM\":" << cfg.lambdaM
    << ",\"source\":\"" << sourceName << "\"}";
  return o.str();
}

} // namespace wire
} // namespace radar
