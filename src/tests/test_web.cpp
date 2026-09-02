// test_web.cpp — Web wire 协议单测：encodeFramePacket 的布局回读对拍
// （按偏移用 memcpy 解码，模拟前端 DataView 读法）、防御分支（!valid、
// 产物缺失、形状与配置漂移）与 buildMetaJson 字段。纯 CPU，无网络。

#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

#include "core/FrameContext.h"
#include "core/RadarConfig.h"
#include "web/WireProtocol.h"

static int g_fail = 0;
static int g_total = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    ++g_total;                                                                 \
    if (!(cond)) {                                                             \
      ++g_fail;                                                                \
      std::cerr << "FAIL: " << #cond << " @ " << __FILE__ << ":" << __LINE__   \
                << "\n";                                                       \
    }                                                                          \
  } while (0)

using namespace radar;

// 按偏移读小端标量（与 detail::put 对拍的解码侧）。
template <class T> static T get(const std::vector<std::uint8_t> &b,
                                std::size_t off) {
  T v{};
  std::memcpy(&v, b.data() + off, sizeof(T));
  return v;
}

// 小配置 + 合成帧：2 chirps × 1 rx × 8 samples，rangeCube 同形状。
static RadarConfig makeCfg() {
  RadarConfig cfg;
  cfg.numRxAnt = 1;
  cfg.numTxAnt = 1;
  cfg.numAdcSamples = 8;
  cfg.chirpStartIdx = 0;
  cfg.chirpEndIdx = 0;
  cfg.numLoops = 2;
  cfg.startFreqGHz = 77.f;
  cfg.derive(); // numRangeBins = nextPow2(8) = 8
  return cfg;
}

static FrameContext makeCtx() {
  FrameContext ctx;
  ctx.frameSeq = 0x0123456789abcdefull;
  ctx.unwrappedPhaseRad = 1.5f;
  ctx.displacementMm = -0.25f;
  ctx.phaseTrackBin = 5;
  ctx.phaseTrackAmp = 42.f;

  auto parsed = std::make_shared<FrameBuffer>(FrameShape{2, 1, 8});
  auto cube = std::make_shared<FrameBuffer>(FrameShape{2, 1, 8});
  for (std::size_t c = 0; c < 2; ++c)
    for (std::size_t s = 0; s < 8; ++s) {
      // 波形取 chirp0：I = 100+s、Q = -(int)s；chirp1 放不同值以确认
      // 编码只取 chirp0。
      parsed->at(c, 0, s) = {static_cast<float>(100 + s + 1000 * c),
                             static_cast<float>(-static_cast<int>(s))};
      // 距离谱：chirp 相干均值 = (1+j0)*(b+1)（两 chirp 同值）。
      cube->at(c, 0, s) = {static_cast<float>(s + 1), 0.f};
    }
  ctx.parsed = parsed;
  ctx.rangeCube = cube;
  return ctx;
}

static void test_encode_roundtrip() {
  const RadarConfig cfg = makeCfg();
  const FrameContext ctx = makeCtx();

  const auto pkt = wire::encodeFramePacket(cfg, ctx);
  const std::size_t expect = wire::kHeaderBytes + 2 * 2 * 8 + 4 * 8;
  CHECK(pkt.size() == expect);

  CHECK(get<std::uint32_t>(pkt, wire::kOffMagic) == wire::kMagic);
  CHECK(get<std::uint16_t>(pkt, wire::kOffVersion) == wire::kVersion);
  CHECK(get<std::uint16_t>(pkt, wire::kOffFlags) == 0x0001u);
  CHECK(get<std::uint64_t>(pkt, wire::kOffFrameSeq) == 0x0123456789abcdefull);
  CHECK(get<float>(pkt, wire::kOffPhaseRad) == 1.5f);
  CHECK(get<float>(pkt, wire::kOffDispMm) == -0.25f);
  CHECK(get<std::int32_t>(pkt, wire::kOffTrackBin) == 5);
  CHECK(get<float>(pkt, wire::kOffTrackAmp) == 42.f);
  CHECK(get<std::uint16_t>(pkt, wire::kOffNWave) == 8);
  CHECK(get<std::uint16_t>(pkt, wire::kOffNBins) == 8);

  // 波形数组：chirp0 的 I/Q 逐元素一致。
  bool waveOk = true;
  const std::size_t offI = wire::kHeaderBytes;
  const std::size_t offQ = offI + 2 * 8;
  for (std::size_t s = 0; s < 8; ++s) {
    if (get<std::int16_t>(pkt, offI + 2 * s) !=
        static_cast<std::int16_t>(100 + s))
      waveOk = false;
    if (get<std::int16_t>(pkt, offQ + 2 * s) !=
        static_cast<std::int16_t>(-static_cast<int>(s)))
      waveOk = false;
  }
  CHECK(waveOk);

  // 距离谱：|均值| = b+1 => dB = 20*log10(b+1+1e-6)。
  bool profOk = true;
  const std::size_t offP = offQ + 2 * 8;
  for (std::size_t b = 0; b < 8; ++b) {
    const float want = 20.f * std::log10(static_cast<float>(b + 1) + 1e-6f);
    if (std::abs(get<float>(pkt, offP + 4 * b) - want) > 1e-4f)
      profOk = false;
  }
  CHECK(profOk);
}

static void test_encode_guards() {
  const RadarConfig cfg = makeCfg();

  { // !valid => 空
    FrameContext ctx = makeCtx();
    ctx.valid = false;
    CHECK(wire::encodeFramePacket(cfg, ctx).empty());
  }
  { // 产物缺失 => 空
    FrameContext ctx = makeCtx();
    ctx.parsed = nullptr;
    CHECK(wire::encodeFramePacket(cfg, ctx).empty());
    ctx = makeCtx();
    ctx.rangeCube = nullptr;
    CHECK(wire::encodeFramePacket(cfg, ctx).empty());
  }
  { // 形状与配置漂移 => 空（绝不发送与 meta 不符的包）
    FrameContext ctx = makeCtx();
    RadarConfig other = cfg;
    other.numAdcSamples = 16;
    other.derive();
    CHECK(wire::encodeFramePacket(other, ctx).empty());
  }
}

static void test_meta_json() {
  const RadarConfig cfg = makeCfg();
  const std::string j = wire::buildMetaJson(cfg, 20.f, "adc_raw_data.bin");
  CHECK(j.find("\"type\":\"meta\"") != std::string::npos);
  CHECK(j.find("\"nWave\":8") != std::string::npos);
  CHECK(j.find("\"nBins\":8") != std::string::npos);
  CHECK(j.find("\"fps\":20") != std::string::npos);
  CHECK(j.find("\"source\":\"adc_raw_data.bin\"") != std::string::npos);
}

int main() {
  test_encode_roundtrip();
  test_encode_guards();
  test_meta_json();

  std::cout << (g_total - g_fail) << "/" << g_total << " checks passed\n";
  if (g_fail) {
    std::cerr << g_fail << " CHECK(s) FAILED\n";
    return 1;
  }
  std::cout << "ALL PASS\n";
  return 0;
}
