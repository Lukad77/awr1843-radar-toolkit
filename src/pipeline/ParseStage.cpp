#include "pipeline/ParseStage.h"

namespace radar {

ParseStage::ParseStage(const RadarConfig &cfg, bool parseAllRx,
                       std::shared_ptr<BufferPool<FrameBuffer>> pool)
    : cfg_(cfg), allRx_(parseAllRx), pool_(std::move(pool)) {
  // 无论单/全 Rx 解析模式，完整线上帧总是携带全部 Rx。
  expectBytes_ = static_cast<std::size_t>(cfg_.numChirpsPerFrame) *
                 cfg_.numRxAnt * cfg_.numAdcSamples *
                 4; // 4 字节/复数采样点（2 个 int16）
}

bool ParseStage::parse(const std::vector<std::uint8_t> &raw,
                       FrameBuffer &fb) const {
  if (raw.size() != expectBytes_)
    return false;

  const auto *p = reinterpret_cast<const std::int16_t *>(raw.data());
  const int numRx = cfg_.numRxAnt;
  const int numChirps = cfg_.numChirpsPerFrame;
  const int numSamp = cfg_.numAdcSamples;
  const std::size_t int16PerRx = static_cast<std::size_t>(numSamp) * 2;
  const std::size_t int16PerChirp = int16PerRx * numRx;

  const int outRx = allRx_ ? numRx : 1;
  fb.resize(FrameShape{static_cast<std::size_t>(numChirps),
                       static_cast<std::size_t>(outRx),
                       static_cast<std::size_t>(numSamp)});

  // 解交织单个 Rx 通道（与遗留 DataParser::parse_RxChannel 对拍）：
  // 对每个偶数采样点 s，base=s*2 处的 4 个 int16 为 I0 I1 Q0 Q1，
  // 即采样点 s = (I0,Q0)、采样点 s+1 = (I1,Q1)。
  auto fillRx = [&](std::size_t rxbase, std::size_t chirp,
                    std::size_t outRxIdx) {
    for (int s = 0; s < numSamp; s += 2) {
      const std::size_t base = rxbase + static_cast<std::size_t>(s) * 2;
      const float I0 = static_cast<float>(p[base + 0]);
      const float I1 = static_cast<float>(p[base + 1]);
      const float Q0 = static_cast<float>(p[base + 2]);
      const float Q1 = static_cast<float>(p[base + 3]);
      fb.at(chirp, outRxIdx, s) = {I0, Q0};
      fb.at(chirp, outRxIdx, s + 1) = {I1, Q1};
    }
  };

  for (int c = 0; c < numChirps; ++c) {
    const std::size_t chirpStart = static_cast<std::size_t>(c) * int16PerChirp;
    if (allRx_) {
      for (int r = 0; r < numRx; ++r)
        fillRx(chirpStart + static_cast<std::size_t>(r) * int16PerRx, c, r);
    } else {
      fillRx(chirpStart + static_cast<std::size_t>(cfg_.rxIdx) * int16PerRx, c,
             0);
    }
  }
  return true;
}

bool ParseStage::process(FrameContext &ctx) {
  if (!ctx.raw) {
    ctx.valid = false;
    ctx.parsed = nullptr;
    return true; // 标记无效后继续转发供指标统计（非静默丢弃）
  }

  std::shared_ptr<FrameBuffer> fb =
      pool_ ? pool_->acquire() : std::make_shared<FrameBuffer>();
  if (!parse(*ctx.raw, *fb)) {
    ctx.valid = false;
    ctx.parsed = nullptr;
    return true;
  }
  ctx.parsed = std::move(fb);
  return true;
}

} // namespace radar
