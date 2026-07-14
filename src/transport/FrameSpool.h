#pragma once
// FrameSpool.h — two-tier lossless FIFO for fixed-size raw frames (Phase 2).
//
// Tier 1: bounded RAM ring (deque up to ramCapFrames).
// Tier 2: when RAM is full, spill to a disk file so the producer (the socket
//         drain thread) NEVER blocks and NEVER drops. This is the concrete
//         "record-then-process" safety net: capacity is bounded by disk, not RAM.
//
// FIFO order is preserved across the RAM->disk boundary: the reader drains RAM
// first (it holds the oldest frames), then disk; and once spilling starts, new
// frames keep going to disk until it fully drains (then RAM is used again).
//
// push() is non-blocking and lossless; it returns false only when closed or on
// a disk write error (the explicit overload condition to alarm on).

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace radar {

class FrameSpool {
public:
    FrameSpool(std::size_t frameBytes, std::size_t ramCapFrames, std::string spillPath);
    ~FrameSpool();

    FrameSpool(const FrameSpool&) = delete;
    FrameSpool& operator=(const FrameSpool&) = delete;

    // Non-blocking, lossless. frame.size() must equal frameBytes.
    // Returns false when closed or on disk write error (overload).
    bool push(std::vector<std::uint8_t> frame);

    // Blocking, FIFO. Returns false only when closed AND fully drained.
    bool pop(std::vector<std::uint8_t>& out);

    void close();

    // Metrics (each takes the lock).
    std::size_t ramDepth() const;
    std::size_t diskPending() const;
    std::size_t diskPeak() const;  // max disk backlog ever seen (>0 => spilled)

private:
    bool diskWrite(const std::vector<std::uint8_t>& f);  // caller holds lock
    bool diskRead(std::vector<std::uint8_t>& out);       // caller holds lock

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::size_t frameBytes_;
    std::size_t ramCap_;
    std::deque<std::vector<std::uint8_t>> ram_;
    bool spilling_ = false;
    bool closed_ = false;

    std::string path_;
    std::fstream file_;
    std::size_t writeOff_ = 0;  // in frames
    std::size_t readOff_ = 0;   // in frames
    std::size_t diskPeak_ = 0;
};

}  // namespace radar
