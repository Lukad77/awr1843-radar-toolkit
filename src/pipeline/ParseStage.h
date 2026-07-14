#pragma once
// ParseStage.h — Phase 3 replacement for DataParser as a pipeline stage.
//
// Deinterleaves raw DCA1000 int16 I/Q bytes into a contiguous FrameBuffer
// [chirp][rx][sample]. Preserves the exact byte layout of the legacy
// DataParser (pair layout per Rx: I0 I1 Q0 Q1) so outputs match the old
// bin->CSV golden. Differences vs legacy:
//   * writes into one flat allocation (no vector<vector<vector<...>>>)
//   * no internal mutex (single-writer per frame in the pipeline)
//   * optional BufferPool to recycle frame buffers (no per-frame malloc)

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
    // parseAllRx=false => single-Rx (cfg.rxIdx) into shape {chirps, 1, samples}.
    // parseAllRx=true  => all Rx into shape {chirps, numRxAnt, samples}.
    // `pool` is optional; when null, process() allocates a fresh FrameBuffer.
    ParseStage(const RadarConfig& cfg, bool parseAllRx = false,
               std::shared_ptr<BufferPool<FrameBuffer>> pool = nullptr);

    const char* name() const override { return "Parse"; }

    // Reads ctx.raw, fills ctx.parsed. On size mismatch: ctx.valid=false and
    // ctx.parsed=nullptr (flagged, never silently forwarded as a partial frame).
    // Returns true (frame kept for downstream metrics/sinks), not a silent drop.
    bool process(FrameContext& ctx) override;

    // Direct deinterleave (also used by unit tests). Returns false on size mismatch.
    bool parse(const std::vector<std::uint8_t>& raw, FrameBuffer& fb) const;

    std::size_t expectedBytes() const noexcept { return expectBytes_; }

private:
    RadarConfig cfg_;
    bool allRx_;
    std::size_t expectBytes_;  // full wire frame (all Rx), == cfg.bytesPerFrame
    std::shared_ptr<BufferPool<FrameBuffer>> pool_;
};

}  // namespace radar
