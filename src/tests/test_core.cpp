// test_core.cpp — dependency-free unit tests for the Phase 0-2 foundation.
//
// Covers the correctness-critical behaviors the plan mandates:
//   * wrap-safe uint32 sequence arithmetic (SeqNum)
//   * bounded blocking SPSC ring: full => non-blocking push fails (no drop),
//     blocking push/pop is strictly FIFO and lossless across threads
//   * FrameBuffer contiguous indexing
//   * BufferPool recycling (no per-frame allocation)
//   * RadarConfig derivation + validation (single source of truth)
//
// Returns non-zero on any failure so CTest reports pass/fail.

#include <atomic>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

#include "core/BufferPool.h"
#include "core/FrameBuffer.h"
#include "core/Interfaces.h"
#include "core/RadarConfig.h"
#include "core/SeqNum.h"
#include "core/SpscRing.h"

static int g_fail = 0;
static int g_total = 0;
#define CHECK(cond)                                                                  \
    do {                                                                             \
        ++g_total;                                                                   \
        if (!(cond)) {                                                               \
            ++g_fail;                                                                \
            std::cerr << "FAIL: " << #cond << " @ " << __FILE__ << ":" << __LINE__   \
                      << "\n";                                                       \
        }                                                                            \
    } while (0)

using namespace radar;

static void test_seqnum() {
    CHECK(seq_lt(0xFFFFFFFFu, 0u));   // wrap: max is "just before" 0
    CHECK(seq_gt(0u, 0xFFFFFFFFu));
    CHECK(seq_diff(5u, 2u) == 3);
    CHECK(seq_diff(1u, 0xFFFFFFFFu) == 2);  // wrap distance across boundary
    CHECK(seq_ge(10u, 10u));
    CHECK(seq_le(10u, 10u));
    CHECK(seq_gap(100u, 103u) == 3);        // 3 ahead => gap
    CHECK(seq_gap(100u, 99u) == -1);        // stale/duplicate
}

static void test_spscring_basic() {
    SpscRing<int> q(4);
    CHECK(q.capacity() == 4);
    CHECK(q.try_push(1));
    CHECK(q.try_push(2));
    CHECK(q.try_push(3));
    CHECK(q.try_push(4));
    CHECK(!q.try_push(5));  // full => non-blocking push fails (no drop, no block)
    int v = 0;
    CHECK(q.try_pop(v) && v == 1);
    CHECK(q.try_pop(v) && v == 2);
    CHECK(q.size() == 2);
}

static void test_spscring_threaded() {
    SpscRing<std::uint64_t> q(64);
    const std::uint64_t N = 100000;
    std::uint64_t sum = 0;
    bool orderOk = true;

    std::thread cons([&] {
        std::uint64_t x = 0, prev = 0;
        bool first = true;
        while (q.pop(x)) {
            if (!first && x != prev + 1) orderOk = false;  // strict FIFO ordering
            prev = x;
            first = false;
            sum += x;
        }
    });

    for (std::uint64_t i = 1; i <= N; ++i) q.push(i);  // blocking => lossless backpressure
    q.close();
    cons.join();

    CHECK(orderOk);
    CHECK(sum == N * (N + 1) / 2);  // nothing lost, nothing duplicated
}

static void test_framebuffer() {
    FrameShape s{2, 4, 256};
    CHECK(s.total() == 2u * 4u * 256u);
    FrameBuffer fb(s);
    CHECK(fb.size() == s.total());
    CHECK(fb.index(0, 0, 0) == 0u);
    CHECK(fb.index(1, 3, 255) == (1u * 4u + 3u) * 256u + 255u);
    fb.at(1, 2, 3) = {1.5f, -2.5f};
    CHECK(fb.at(1, 2, 3).real() == 1.5f);
    CHECK(fb.at(1, 2, 3).imag() == -2.5f);
}

static void test_bufferpool() {
    auto pool = BufferPool<FrameBuffer>::create(
        [] { return std::make_unique<FrameBuffer>(FrameShape{1, 1, 256}); },
        [](FrameBuffer& b) {
            if (b.size()) b.data()[0] = {};
        },
        2);
    CHECK(pool->free_count() == 2);

    FrameBuffer* addr1 = nullptr;
    {
        auto b = pool->acquire();
        addr1 = b.get();
        CHECK(pool->free_count() == 1);
    }  // returned to pool here
    CHECK(pool->free_count() == 2);

    auto b2 = pool->acquire();
    CHECK(b2.get() == addr1);  // recycled (no new allocation)
}

static void test_radarconfig() {
    RadarConfig c;
    c.numRxAnt = 4;
    c.numTxAnt = 1;
    c.numAdcSamples = 256;
    c.chirpStartIdx = 0;
    c.chirpEndIdx = 0;
    c.numLoops = 64;
    c.startFreqGHz = 77.f;
    c.idleTimeUs = 100.f;
    c.rampEndTimeUs = 60.f;
    c.freqSlopeMHzPerUs = 60.f;
    c.digOutSampleRateKsps = 10000;
    c.derive();

    CHECK(c.numChirpsPerFrame == 64);
    CHECK(c.numDopplerBins == 64);
    CHECK(c.numRangeBins == 256);  // already a power of two
    CHECK(c.bytesPerSample == 4);
    CHECK(c.bytesPerFrame == static_cast<long>(64) * 4 * 256 * 4);  // 262144
    CHECK(c.rangeResolutionMeters > 0.f);
    CHECK(c.maxRange > 0.f);

    std::string err;
    CHECK(c.validate(err));

    // Non-power-of-two ADC samples round up for the range FFT size.
    RadarConfig d = c;
    d.numAdcSamples = 200;
    d.derive();
    CHECK(d.numRangeBins == 256);

    // Invalid config is rejected.
    RadarConfig bad;
    bad.numAdcSamples = 255;  // odd
    bad.derive();
    std::string e2;
    CHECK(!bad.validate(e2));
    CHECK(!e2.empty());
}

int main() {
    test_seqnum();
    test_spscring_basic();
    test_spscring_threaded();
    test_framebuffer();
    test_bufferpool();
    test_radarconfig();

    std::cout << (g_total - g_fail) << "/" << g_total << " checks passed\n";
    if (g_fail) {
        std::cerr << g_fail << " CHECK(s) FAILED\n";
        return 1;
    }
    std::cout << "ALL PASS\n";
    return 0;
}
