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

  // 解交织单个 Rx 通道。线上每 4 个 int16 为一组 [x0 x1 y0 y1]，
  // 覆盖两个连续采样点：s = (y0, x0)、s+1 = (y1, x1) —— 前两个是
  // 虚部、后两个是实部。遗留 DataParser 按 (x, y) = (I, Q) 组复数，
  // 会把目标镜像到负频率 bin（实测本数据集正/镜像 bin 幅度比
  // 1:54，正频率一侧只剩泄漏裙边）；MATLAB 参考实现读入后同样
  // 显式做了 imag + 1i*real 交换（respiratory_main_F5_only.m）。
  auto fillRx = [&](std::size_t rxbase, std::size_t chirp,
                    std::size_t outRxIdx) {
    for (int s = 0; s < numSamp; s += 2) {
      const std::size_t base = rxbase + static_cast<std::size_t>(s) * 2;
      const float x0 = static_cast<float>(p[base + 0]);
      const float x1 = static_cast<float>(p[base + 1]);
      const float y0 = static_cast<float>(p[base + 2]);
      const float y1 = static_cast<float>(p[base + 3]);
      fb.at(chirp, outRxIdx, s) = {y0, x0};
      fb.at(chirp, outRxIdx, s + 1) = {y1, x1};
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
