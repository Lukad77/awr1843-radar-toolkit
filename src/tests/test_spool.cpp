// test_spool.cpp — Phase 2 tests for the two-tier lossless FrameSpool:
// FIFO order + byte-identical round-trip across the RAM->disk boundary, that
// spilling actually happens, RAM reuse after drain, and clean threaded shutdown.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include "transport/FrameSpool.h"

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

static std::vector<std::uint8_t> mkframe(std::uint64_t i) {
    std::vector<std::uint8_t> f(8);
    std::memcpy(f.data(), &i, 8);
    return f;
}
static std::uint64_t decode(const std::vector<std::uint8_t>& f) {
    std::uint64_t i = 0;
    std::memcpy(&i, f.data(), 8);
    return i;
}

static void test_spill_order_lossless() {
    FrameSpool s(/*frameBytes=*/8, /*ramCapFrames=*/4, "radar_spool_testA.bin");
    const std::uint64_t M = 100;

    for (std::uint64_t i = 0; i < M; ++i) CHECK(s.push(mkframe(i)));
    CHECK(s.diskPeak() > 0);  // ramCap(4) << 100 => must have spilled to disk

    std::vector<std::uint8_t> f;
    bool ok = true;
    for (std::uint64_t i = 0; i < M; ++i) {
        if (!s.pop(f) || decode(f) != i) ok = false;  // FIFO + byte-identical
    }
    CHECK(ok);

    // After full drain, spilling resets and RAM tier is used again.
    CHECK(s.diskPending() == 0);
    for (std::uint64_t i = 0; i < 4; ++i) CHECK(s.push(mkframe(1000 + i)));
    CHECK(s.diskPending() == 0);  // fit in RAM, no spill
    for (std::uint64_t i = 0; i < 4; ++i) {
        CHECK(s.pop(f));
        CHECK(decode(f) == 1000 + i);
    }
}

static void test_threaded_close() {
    FrameSpool s(8, 4, "radar_spool_testB.bin");
    const std::uint64_t N = 5000;
    std::atomic<std::uint64_t> count{0};
    bool orderOk = true;

    std::thread cons([&] {
        std::vector<std::uint8_t> f;
        std::uint64_t prev = 0;
        bool first = true;
        while (s.pop(f)) {
            std::uint64_t d = decode(f);
            if (!first && d != prev + 1) orderOk = false;  // strict FIFO
            prev = d;
            first = false;
            count.fetch_add(1);
        }
    });

    for (std::uint64_t i = 0; i < N; ++i) CHECK(s.push(mkframe(i)));  // lossless, non-blocking
    s.close();  // must wake the blocked consumer and let it drain
    cons.join();

    CHECK(count.load() == N);
    CHECK(orderOk);
}

int main() {
    test_spill_order_lossless();
    test_threaded_close();

    std::cout << (g_total - g_fail) << "/" << g_total << " checks passed\n";
    if (g_fail) {
        std::cerr << g_fail << " CHECK(s) FAILED\n";
        return 1;
    }
    std::cout << "ALL PASS\n";
    return 0;
}
