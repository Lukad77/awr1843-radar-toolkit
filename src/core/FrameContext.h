#pragma once
// FrameContext.h — the unit of work that flows through the pipeline.
//
// Carries a monotonic uint64 `frameSeq` (the basis for ordering checks and
// loss detection end-to-end) plus the raw and/or parsed payloads and timing
// metadata for latency metrics. Buffers are shared_ptr so they can be pooled
// (see BufferPool) and cheaply handed between stages without deep copies.

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/FrameBuffer.h"

namespace radar {

struct FrameContext {
    // Application-level monotonic frame counter. uint64 => no practical overflow.
    std::uint64_t frameSeq = 0;

    // First DCA1000 packet seqNum that composed this frame (wrap-aware, uint32).
    std::uint32_t wireSeqStart = 0;

    // false when a seq gap/reorder left this frame incomplete: it must be
    // flagged + counted, never silently forwarded as a partial frame.
    bool valid = true;

    // Raw bytes as received/reassembled (may be null after parsing).
    std::shared_ptr<std::vector<std::uint8_t>> raw;

    // Parsed complex tensor [chirp][rx][sample] (may be null before parsing).
    std::shared_ptr<FrameBuffer> parsed;

    // Timestamps for end-to-end latency measurement.
    std::chrono::steady_clock::time_point tCaptured{};
};

} // namespace radar
