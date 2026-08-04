#pragma once
// ParseStage.h — Phase 3 中以流水线 stage 形式替代 DataParser。
//
// 将原始 DCA1000 int16 I/Q 字节解交织为连续的 FrameBuffer
// [chirp][rx][sample]。严格保持遗留 DataParser 的字节布局
// （每 Rx 的成对布局：I0 I1 Q0 Q1），使输出匹配旧的 bin->CSV
// 黄金基准。与遗留实现的差异：
//   * 写入单块连续内存（没有 vector<vector<vector<...>>>）
//   * 无内部 mutex（流水线中每帧单写者）
//   * 可选 BufferPool 复用帧缓冲（无逐帧 malloc）

#include <cstdint>
#include <memory>
#include <vector>

#include "core/BufferPool.h"
#include "core/FrameBuffer.h"
#include "core/Interfaces.h"
#include "core/RadarConfig.h"

namespace radar {

class ParseStage : public IStage {
public:
  // parseAllRx=false => 单 Rx（cfg.rxIdx），输出形状 {chirps, 1, samples}。
  // parseAllRx=true  => 全 Rx，输出形状 {chirps, numRxAnt, samples}。
  // `pool` 可选；为空时 process() 新建 FrameBuffer。
  ParseStage(const RadarConfig &cfg, bool parseAllRx = false,
             std::shared_ptr<BufferPool<FrameBuffer>> pool = nullptr);

  const char *name() const override { return "Parse"; }

  // 读 ctx.raw，填 ctx.parsed。尺寸不匹配时：ctx.valid=false 且
  // ctx.parsed=nullptr（标记而非静默转发残帧）。
  // 返回 true（帧保留给下游指标/sink），不做静默丢弃。
  bool process(FrameContext &ctx) override;

  // 直接解交织（单测也用）。尺寸不匹配返回 false。
  bool parse(const std::vector<std::uint8_t> &raw, FrameBuffer &fb) const;

  std::size_t expectedBytes() const noexcept { return expectBytes_; }

private:
  RadarConfig cfg_;
  bool allRx_;
  std::size_t expectBytes_; // 完整线上帧（含全 Rx），== cfg.bytesPerFrame
  std::shared_ptr<BufferPool<FrameBuffer>> pool_;
};

} // namespace radar
