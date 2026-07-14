// test_pipeline.cpp — Phase 3 tests: ParseStage deinterleave + Pipeline
// order-preserving/lossless behavior (no hardware; frames synthesized in RAM).

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

#include "core/Interfaces.h"
#include "core/RadarConfig.h"
#include "pipeline/ParseStage.h"
#include "pipeline/Pipeline.h"

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

// Small config: 2 chirps, 2 Rx, 4 samples => 2*2*4*4 = 64 bytes = 32 int16.
static RadarConfig makeCfg(int rxIdx) {
    RadarConfig c;
    c.numRxAnt = 2;
    c.numTxAnt = 1;
    c.numAdcSamples = 4;
    c.chirpStartIdx = 0;
    c.chirpEndIdx = 0;
    c.numLoops = 2;
    c.rxIdx = rxIdx;
    c.derive();
    return c;
}

// Raw frame where int16[k] = k (little-endian bytes).
static std::shared_ptr<std::vector<std::uint8_t>> makeRaw(int numInt16) {
    auto buf = std::make_shared<std::vector<std::uint8_t>>(numInt16 * 2);
    for (int k = 0; k < numInt16; ++k) {
        std::int16_t v = static_cast<std::int16_t>(k);
        std::memcpy(buf->data() + k * 2, &v, 2);
    }
    return buf;
}

// Reference deinterleave (mirrors legacy DataParser::parse_RxChannel) for verification.
static std::complex<float> expectedSample(int c, int r, int s, int numRx, int numSamp) {
    const int int16PerRx = numSamp * 2;
    const int int16PerChirp = int16PerRx * numRx;
    const int rxbase = c * int16PerChirp + r * int16PerRx;
    const int se = (s / 2) * 2;
    const int base = rxbase + se * 2;
    auto val = [](int k) { return static_cast<float>(static_cast<std::int16_t>(k)); };
    if (s % 2 == 0) return {val(base + 0), val(base + 2)};
    return {val(base + 1), val(base + 3)};
}

static void test_parse_single_rx() {
    RadarConfig cfg = makeCfg(/*rxIdx=*/1);
    ParseStage stage(cfg, /*allRx=*/false);
    CHECK(stage.expectedBytes() == 64u);

    auto raw = makeRaw(32);
    FrameBuffer fb;
    CHECK(stage.parse(*raw, fb));
    CHECK(fb.shape() == (FrameShape{2, 1, 4}));

    bool ok = true;
    for (int c = 0; c < 2; ++c)
        for (int s = 0; s < 4; ++s)
            if (fb.at(c, 0, s) != expectedSample(c, /*r=*/1, s, 2, 4)) ok = false;
    CHECK(ok);

    // Size mismatch is rejected (never silently accepted).
    std::vector<std::uint8_t> bad(10);
    FrameBuffer fb2;
    CHECK(!stage.parse(bad, fb2));
}

static void test_parse_all_rx() {
    RadarConfig cfg = makeCfg(/*rxIdx=*/0);
    ParseStage stage(cfg, /*allRx=*/true);

    auto raw = makeRaw(32);
    FrameBuffer fb;
    CHECK(stage.parse(*raw, fb));
    CHECK(fb.shape() == (FrameShape{2, 2, 4}));

    bool ok = true;
    for (int c = 0; c < 2; ++c)
        for (int r = 0; r < 2; ++r)
            for (int s = 0; s < 4; ++s)
                if (fb.at(c, r, s) != expectedSample(c, r, s, 2, 4)) ok = false;
    CHECK(ok);
}

// Sink that records order/validity of everything it receives (runs on worker thread;
// read from main only after Pipeline::stop() joins => safe happens-before).
class VerifySink : public IResultSink {
public:
    void consume(const FrameContext& ctx) override {
        seqs.push_back(ctx.frameSeq);
        if (!ctx.valid || !ctx.parsed) allValidParsed = false;
    }
    std::vector<std::uint64_t> seqs;
    bool allValidParsed = true;
};

static void test_pipeline_order_lossless() {
    RadarConfig cfg = makeCfg(/*rxIdx=*/0);
    auto sink = std::make_shared<VerifySink>();

    Pipeline p(/*inputCapacity=*/8);  // small ring => exercises backpressure
    p.addStage(std::make_shared<ParseStage>(cfg, /*allRx=*/false));
    p.addSink(sink);
    p.start();

    const std::uint64_t N = 5000;
    auto raw = makeRaw(32);
    for (std::uint64_t i = 0; i < N; ++i) {
        FrameContext ctx;
        ctx.frameSeq = i;
        ctx.raw = raw;
        CHECK(p.submit(std::move(ctx)));  // blocking => lossless
    }
    p.stop();

    CHECK(sink->seqs.size() == N);           // nothing lost
    CHECK(sink->allValidParsed);             // every frame parsed OK
    bool ordered = true;
    for (std::uint64_t i = 0; i < sink->seqs.size(); ++i)
        if (sink->seqs[i] != i) ordered = false;  // strict monotonic order preserved
    CHECK(ordered);
}

int main() {
    test_parse_single_rx();
    test_parse_all_rx();
    test_pipeline_order_lossless();

    std::cout << (g_total - g_fail) << "/" << g_total << " checks passed\n";
    if (g_fail) {
        std::cerr << g_fail << " CHECK(s) FAILED\n";
        return 1;
    }
    std::cout << "ALL PASS\n";
    return 0;
}
