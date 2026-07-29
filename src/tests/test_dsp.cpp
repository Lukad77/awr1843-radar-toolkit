// test_dsp.cpp — Phase 4 tests: radix-2 FFT correctness (naive-DFT对拍),
// RangeFFT / DopplerFFT peak placement, CA-CFAR detection + Pfa behavior,
// AngleFFT steering-vector recovery, slow-time phase unwrapping (with real
// +-pi wraps), and the full stage chain through the Pipeline under
// backpressure. No hardware; all signals synthesized.

#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

#include "core/BufferPool.h"
#include "core/FrameBuffer.h"
#include "core/Interfaces.h"
#include "core/RadarConfig.h"
#include "dsp/AngleFftStage.h"
#include "dsp/CfarStage.h"
#include "dsp/ClutterRemovalStage.h"
#include "dsp/Detection.h"
#include "dsp/DopplerFftStage.h"
#include "dsp/Fft.h"
#include "dsp/PhaseUnwrapStage.h"
#include "dsp/RangeFftStage.h"
#include "pipeline/ParseStage.h"
#include "pipeline/Pipeline.h"

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
using cfloat = std::complex<float>;

static constexpr double kPi = 3.14159265358979323846;

// Deterministic LCG (identical across platforms) for noise synthesis.
static std::uint32_t g_rng = 0x12345678u;
static double urand() { // uniform (0, 1)
  g_rng = g_rng * 1664525u + 1013904223u;
  return (static_cast<double>(g_rng >> 8) + 1.0) / 16777217.0;
}

// ---------------------------------------------------------------------------
// FFT infrastructure
// ---------------------------------------------------------------------------

static void test_fft_basics() {
  // Single tone e^{j*2pi*3n/16} => all energy in bin 3, |X[3]| == N.
  FftPlan p16(16);
  std::vector<cfloat> x(16);
  for (int n = 0; n < 16; ++n) {
    const double a = 2.0 * kPi * 3.0 * n / 16.0;
    x[n] = {static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))};
  }
  p16.forward(x.data());
  std::size_t peak = 0;
  for (std::size_t k = 1; k < 16; ++k)
    if (std::abs(x[k]) > std::abs(x[peak]))
      peak = k;
  CHECK(peak == 3);
  CHECK(std::abs(std::abs(x[3]) - 16.f) < 1e-3f);

  // Random input vs naive O(N^2) DFT.
  const std::size_t N = 64;
  FftPlan p64(N);
  std::vector<cfloat> in(N), fft(N), dft(N);
  for (std::size_t n = 0; n < N; ++n)
    in[n] = {static_cast<float>(urand() * 2 - 1),
             static_cast<float>(urand() * 2 - 1)};
  fft = in;
  p64.forward(fft.data());
  for (std::size_t k = 0; k < N; ++k) {
    std::complex<double> acc{0, 0};
    for (std::size_t n = 0; n < N; ++n) {
      const double a = -2.0 * kPi * static_cast<double>(k * n) / N;
      acc += std::complex<double>(in[n].real(), in[n].imag()) *
             std::complex<double>(std::cos(a), std::sin(a));
    }
    dft[k] = {static_cast<float>(acc.real()), static_cast<float>(acc.imag())};
  }
  float maxErr = 0.f, maxMag = 0.f;
  double eIn = 0.0, eOut = 0.0;
  for (std::size_t k = 0; k < N; ++k) {
    maxErr = std::max(maxErr, std::abs(fft[k] - dft[k]));
    maxMag = std::max(maxMag, std::abs(dft[k]));
    eIn += std::norm(in[k]);
    eOut += std::norm(fft[k]);
  }
  CHECK(maxErr / maxMag < 1e-3f);                     // matches naive DFT
  CHECK(std::abs(eOut - N * eIn) / (N * eIn) < 1e-3); // Parseval

  // fftshift swaps halves.
  std::vector<cfloat> s{{1, 0}, {2, 0}, {3, 0}, {4, 0}};
  fftshift(s.data(), 4);
  CHECK(s[0] == cfloat(3, 0) && s[1] == cfloat(4, 0) && s[2] == cfloat(1, 0) &&
        s[3] == cfloat(2, 0));

  // Hann window: zero endpoints, symmetric.
  auto w = makeHannWindow(8);
  CHECK(w.size() == 8 && std::abs(w[0]) < 1e-6f && std::abs(w[7]) < 1e-6f);
  bool sym = true;
  for (int i = 0; i < 8; ++i)
    if (std::abs(w[i] - w[7 - i]) > 1e-6f)
      sym = false;
  CHECK(sym);
}

// ---------------------------------------------------------------------------
// RangeFftStage
// ---------------------------------------------------------------------------

static void test_range_fft() {
  RadarConfig cfg;
  cfg.numRxAnt = 2;
  cfg.numTxAnt = 1;
  cfg.numAdcSamples = 16;
  cfg.chirpStartIdx = 0;
  cfg.chirpEndIdx = 0;
  cfg.numLoops = 2;
  cfg.derive(); // rangeBins = 16

  auto parsed = std::make_shared<FrameBuffer>(FrameShape{2, 2, 16});
  for (std::size_t c = 0; c < 2; ++c)
    for (std::size_t r = 0; r < 2; ++r)
      for (int s = 0; s < 16; ++s) {
        const double a = 2.0 * kPi * 5.0 * s / 16.0;
        parsed->at(c, r, s) = {static_cast<float>((r + 1) * std::cos(a)),
                               static_cast<float>((r + 1) * std::sin(a))};
      }

  RangeFftStage stage(cfg);
  FrameContext ctx;
  ctx.parsed = parsed;
  CHECK(stage.process(ctx));
  CHECK(ctx.valid && ctx.rangeCube != nullptr);
  CHECK(ctx.rangeCube->shape() == (FrameShape{2, 2, 16}));

  bool peaksOk = true;
  for (std::size_t c = 0; c < 2; ++c)
    for (std::size_t r = 0; r < 2; ++r) {
      std::size_t peak = 0;
      for (std::size_t b = 1; b < 16; ++b)
        if (std::abs(ctx.rangeCube->at(c, r, b)) >
            std::abs(ctx.rangeCube->at(c, r, peak)))
          peak = b;
      if (peak != 5)
        peaksOk = false;
    }
  CHECK(peaksOk);

  // Zero-pad path: 12 samples -> 16 range bins; tone at 3/12 lands on bin 4.
  RadarConfig cfg2 = cfg;
  cfg2.numAdcSamples = 12;
  cfg2.derive();
  CHECK(cfg2.numRangeBins == 16);
  auto parsed2 = std::make_shared<FrameBuffer>(FrameShape{1, 1, 12});
  for (int s = 0; s < 12; ++s) {
    const double a = 2.0 * kPi * 3.0 * s / 12.0;
    parsed2->at(0, 0, s) = {static_cast<float>(std::cos(a)),
                            static_cast<float>(std::sin(a))};
  }
  RangeFftStage stage2(cfg2);
  FrameContext ctx2;
  ctx2.parsed = parsed2;
  CHECK(stage2.process(ctx2) && ctx2.rangeCube != nullptr);
  std::size_t peak2 = 0;
  for (std::size_t b = 1; b < 16; ++b)
    if (std::abs(ctx2.rangeCube->at(0, 0, b)) >
        std::abs(ctx2.rangeCube->at(0, 0, peak2)))
      peak2 = b;
  CHECK(peak2 == 4);

  // Shape mismatch is flagged, never silently processed.
  FrameContext bad;
  bad.parsed = std::make_shared<FrameBuffer>(FrameShape{2, 2, 8});
  CHECK(stage.process(bad));
  CHECK(!bad.valid && bad.rangeCube == nullptr);
}

// ---------------------------------------------------------------------------
// DopplerFftStage
// ---------------------------------------------------------------------------

static void test_doppler_fft() {
  RadarConfig cfg;
  cfg.numRxAnt = 1;
  cfg.numTxAnt = 1;
  cfg.numAdcSamples = 8;
  cfg.chirpStartIdx = 0;
  cfg.chirpEndIdx = 0;
  cfg.numLoops = 8;
  cfg.derive(); // chirps = 8, rangeBins = 8, dopplerBins = 8

  // Target at range bin 2 rotating e^{j*2pi*2c/8} => unshifted doppler bin 2
  // => shifted index 6 (= 2 + C/2).
  auto rangeCube = std::make_shared<FrameBuffer>(FrameShape{8, 1, 8});
  for (std::size_t c = 0; c < 8; ++c) {
    const double a = 2.0 * kPi * 2.0 * c / 8.0;
    rangeCube->at(c, 0, 2) = {static_cast<float>(std::cos(a)),
                              static_cast<float>(std::sin(a))};
  }

  DopplerFftStage stage(cfg);
  FrameContext ctx;
  ctx.rangeCube = rangeCube;
  CHECK(stage.process(ctx));
  CHECK(ctx.valid && ctx.dopplerCube != nullptr && ctx.rdMap != nullptr);
  CHECK(ctx.dopplerCube->shape() == (FrameShape{8, 1, 8}));
  CHECK(ctx.rdMap->size() == 64u);

  std::size_t peakD = 0;
  for (std::size_t d = 1; d < 8; ++d)
    if (std::abs(ctx.dopplerCube->at(d, 0, 2)) >
        std::abs(ctx.dopplerCube->at(peakD, 0, 2)))
      peakD = d;
  CHECK(peakD == 6);

  // rdMap global max sits at (doppler 6, range 2).
  std::size_t argmax = 0;
  for (std::size_t i = 1; i < ctx.rdMap->size(); ++i)
    if ((*ctx.rdMap)[i] > (*ctx.rdMap)[argmax])
      argmax = i;
  CHECK(argmax == 6u * 8u + 2u);
}

// ---------------------------------------------------------------------------
// CfarStage
// ---------------------------------------------------------------------------

static void test_cfar() {
  RadarConfig cfg;
  cfg.numRxAnt = 1;
  cfg.numTxAnt = 1;
  cfg.numAdcSamples = 256;
  cfg.chirpStartIdx = 0;
  cfg.chirpEndIdx = 0;
  cfg.numLoops = 4;
  cfg.derive(); // dopplerBins = 4, rangeBins = 256

  const int D = cfg.numDopplerBins, B = cfg.numRangeBins;
  // Exponential noise (matches the CA-CFAR design assumption), mean 1.
  auto rd = std::make_shared<std::vector<float>>(D * B);
  for (auto &v : *rd)
    v = static_cast<float>(-std::log(1.0 - urand()));
  // Inject 20 dB targets: two interior + two at the row edges.
  (*rd)[1 * B + 50] = 100.f;
  (*rd)[2 * B + 200] = 100.f;
  (*rd)[3 * B + 0] = 100.f;
  (*rd)[3 * B + 255] = 100.f;

  CfarStage stage(cfg);
  FrameContext ctx;
  ctx.rdMap = rd;
  CHECK(stage.process(ctx));
  CHECK(ctx.valid && ctx.detections != nullptr);

  auto found = [&](int d, int b) {
    for (const auto &det : *ctx.detections)
      if (det.dopplerBin == d && det.rangeBin == b)
        return true;
    return false;
  };
  CHECK(found(1, 50));
  CHECK(found(2, 200));
  CHECK(found(3, 0)); // truncated window still detects at the edge
  CHECK(found(3, 255));
  // Pfa=1e-4 over 1024 cells => ~0.1 expected false alarms; allow slack.
  CHECK(ctx.detections->size() <= 4u + 5u);

  // SNR estimate close to the injected 20 dB.
  for (const auto &det : *ctx.detections)
    if (det.dopplerBin == 1 && det.rangeBin == 50)
      CHECK(std::abs(det.snrDb - 20.f) < 3.f);

  // Dimension drift is flagged.
  FrameContext bad;
  bad.rdMap = std::make_shared<std::vector<float>>(10);
  CHECK(stage.process(bad));
  CHECK(!bad.valid);
}

// ---------------------------------------------------------------------------
// AngleFftStage
// ---------------------------------------------------------------------------

static void test_angle_fft() {
  RadarConfig cfg;
  cfg.numRxAnt = 4;
  cfg.numTxAnt = 1;
  cfg.numAdcSamples = 8;
  cfg.chirpStartIdx = 0;
  cfg.chirpEndIdx = 0;
  cfg.numLoops = 4;
  cfg.numAngleBins = 64;
  cfg.derive();

  auto cube = std::make_shared<FrameBuffer>(FrameShape{4, 4, 8});
  auto steer = [&](int d, int b, double thetaDeg) {
    const double st = std::sin(thetaDeg * kPi / 180.0);
    for (std::size_t r = 0; r < 4; ++r) {
      const double a = kPi * st * static_cast<double>(r); // lambda/2 ULA
      cube->at(d, r, b) = {static_cast<float>(std::cos(a)),
                           static_cast<float>(std::sin(a))};
    }
  };
  steer(2, 3, 20.0);  // +20 deg target
  steer(1, 5, -35.0); // -35 deg target
  steer(3, 6, 0.0);   // boresight

  auto dets = std::make_shared<std::vector<Detection>>();
  dets->push_back({3, 2}); // rangeBin=3, dopplerBin=2
  dets->push_back({5, 1});
  dets->push_back({6, 3});

  AngleFftStage stage(cfg);
  FrameContext ctx;
  ctx.dopplerCube = cube;
  ctx.detections = dets;
  CHECK(stage.process(ctx));

  // 64-bin grid => asin(2k/64) steps, ~1 deg near boresight, wider at edges.
  CHECK(std::abs((*dets)[0].angleDeg - 20.f) < 2.5f);
  CHECK(std::abs((*dets)[1].angleDeg + 35.f) < 2.5f);
  CHECK(std::abs((*dets)[2].angleDeg) < 1.f);
}

// ---------------------------------------------------------------------------
// PhaseUnwrapStage
// ---------------------------------------------------------------------------

static void test_phase_unwrap() {
  RadarConfig cfg;
  cfg.numRxAnt = 1;
  cfg.numTxAnt = 1;
  cfg.numAdcSamples = 16;
  cfg.chirpStartIdx = 0;
  cfg.chirpEndIdx = 0;
  cfg.numLoops = 4;
  cfg.startFreqGHz = 77.f; // lambdaM ~ 3.896 mm for displacement conversion
  cfg.derive();
  CHECK(cfg.lambdaM > 0.f);

  // phi(t) = 3*sin(2pi t/40): amplitude 3 rad > pi, so atan2 wraps for sure;
  // inter-frame step max ~0.47 rad < pi, so unwrapping is well-posed.
  PhaseUnwrapParams params;
  params.targetRangeBin = 5;
  PhaseUnwrapStage stage(cfg, params);

  const int M = 60;
  std::vector<double> truth(M), out(M);
  float lastDisp = 0.f;
  for (int t = 0; t < M; ++t) {
    truth[t] = 3.0 * std::sin(2.0 * kPi * t / 40.0);
    auto cube = std::make_shared<FrameBuffer>(FrameShape{4, 1, 16});
    for (std::size_t c = 0; c < 4; ++c) {
      for (std::size_t b = 0; b < 16; ++b)
        cube->at(c, 0, b) = {0.01f, 0.f};
      cube->at(c, 0, 5) = {static_cast<float>(10.0 * std::cos(truth[t])),
                           static_cast<float>(10.0 * std::sin(truth[t]))};
    }
    FrameContext ctx;
    ctx.frameSeq = static_cast<std::uint64_t>(t);
    ctx.rangeCube = cube;
    CHECK(stage.process(ctx));
    CHECK(!std::isnan(ctx.unwrappedPhaseRad));
    out[t] = ctx.unwrappedPhaseRad;
    lastDisp = ctx.displacementMm;
  }
  CHECK(stage.targetBin() == 5);

  // Continuous track: relative phase matches the (wrapping-free) truth.
  double maxErr = 0.0;
  for (int t = 0; t < M; ++t)
    maxErr =
        std::max(maxErr, std::abs((out[t] - out[0]) - (truth[t] - truth[0])));
  CHECK(maxErr < 1e-3);

  // Displacement formula: delta_d = lambda * delta_phi / (4*pi).
  const double expectMm =
      (out[M - 1] - out[0]) * cfg.lambdaM / (4.0 * kPi) * 1000.0;
  CHECK(std::abs(lastDisp - expectMm) < 1e-3);

  // Auto bin selection locks onto the strongest bin.
  PhaseUnwrapStage autoStage(cfg, PhaseUnwrapParams{});
  auto cube = std::make_shared<FrameBuffer>(FrameShape{4, 1, 16});
  for (std::size_t c = 0; c < 4; ++c) {
    for (std::size_t b = 0; b < 16; ++b)
      cube->at(c, 0, b) = {0.01f, 0.f};
    cube->at(c, 0, 7) = {10.f, 0.f};
  }
  FrameContext ctx;
  ctx.rangeCube = cube;
  CHECK(autoStage.process(ctx));
  CHECK(autoStage.targetBin() == 7);
  CHECK(ctx.phaseTrackBin == 7);                      // quality field: bin
  CHECK(std::abs(ctx.phaseTrackAmp - 10.f) < 0.01f);  // quality field: raw amp
}

// Helper: frame with one complex value at `bin` (all chirps), tiny floor elsewhere.
static std::shared_ptr<FrameBuffer> binFrame(int nBins, int bin, std::complex<float> v,
                                             int bin2 = -1,
                                             std::complex<float> v2 = {0.f, 0.f}) {
  auto cube = std::make_shared<FrameBuffer>(
      FrameShape{4, 1, static_cast<std::size_t>(nBins)});
  for (std::size_t c = 0; c < 4; ++c) {
    for (int b = 0; b < nBins; ++b) cube->at(c, 0, b) = {0.01f, 0.f};
    cube->at(c, 0, bin) = v;
    if (bin2 >= 0) cube->at(c, 0, bin2) = v2;
  }
  return cube;
}

// Plateau mechanism reproduced, then removed by the Kasa DC compensation:
// v = S + a*e^{j*phi(t)} with |S| ~ 5a pins arg(v) near arg(S).
static void test_phase_dc_compensation() {
  RadarConfig cfg;
  cfg.numRxAnt = 1;
  cfg.numTxAnt = 1;
  cfg.numAdcSamples = 16;
  cfg.chirpStartIdx = 0;
  cfg.chirpEndIdx = 0;
  cfg.numLoops = 4;
  cfg.startFreqGHz = 77.f;
  cfg.derive();

  const std::complex<float> S{50.f, 30.f};  // |S| = 58.3 ~ 5.8a
  const float a = 10.f;
  const int M = 200;
  auto truth = [](int t) { return 0.8 * std::sin(2.0 * kPi * t / 40.0); };
  auto sample = [&](int t) {
    const double ph = truth(t);
    return S + std::complex<float>(static_cast<float>(a * std::cos(ph)),
                                   static_cast<float>(a * std::sin(ph)));
  };

  // A) compensation OFF => plateau: recovered p2p crushed to ~a/|S| scale.
  {
    PhaseUnwrapParams p;
    p.targetRangeBin = 5;
    p.dcCompensation = false;
    PhaseUnwrapStage stage(cfg, p);
    float lo = 1e9f, hi = -1e9f;
    for (int t = 0; t < M; ++t) {
      FrameContext ctx;
      ctx.rangeCube = binFrame(16, 5, sample(t));
      CHECK(stage.process(ctx));
      if (t >= 40) {  // steady state, full cycles
        lo = std::min(lo, ctx.unwrappedPhaseRad);
        hi = std::max(hi, ctx.unwrappedPhaseRad);
      }
    }
    const float truthP2p = 1.6f;
    CHECK((hi - lo) < 0.35f * truthP2p);  // plateau reproduced
  }

  // B) compensation ON => center removed, waveform recovered.
  {
    PhaseUnwrapParams p;
    p.targetRangeBin = 5;
    p.dcCompensation = true;
    p.dcWindowFrames = 64;
    PhaseUnwrapStage stage(cfg, p);
    std::vector<float> out(M);
    for (int t = 0; t < M; ++t) {
      FrameContext ctx;
      ctx.rangeCube = binFrame(16, 5, sample(t));
      CHECK(stage.process(ctx));
      out[t] = ctx.unwrappedPhaseRad;
    }
    // After warmup, relative phase must match the truth waveform.
    const int t0 = 120;
    double maxErr = 0.0;
    for (int t = t0; t < M; ++t)
      maxErr = std::max(maxErr,
                        std::abs((out[t] - out[t0]) - (truth(t) - truth(t0))));
    CHECK(maxErr < 0.05);  // < 5% of the 1.6 rad p2p
  }
}

// Bin switch: hysteresis-approved handover must not fabricate a phase step
// even when the new bin carries an arbitrary static phase offset.
static void test_phase_bridge_on_switch() {
  RadarConfig cfg;
  cfg.numRxAnt = 1;
  cfg.numTxAnt = 1;
  cfg.numAdcSamples = 16;
  cfg.chirpStartIdx = 0;
  cfg.chirpEndIdx = 0;
  cfg.numLoops = 4;
  cfg.derive();

  PhaseUnwrapParams p;  // auto + followPeak
  p.followPeak = true;
  p.switchHoldFrames = 3;
  p.switchRatio = 1.2f;
  p.dcCompensation = false;  // isolate the bridging behavior
  PhaseUnwrapStage stage(cfg, p);

  const int M = 40;
  std::vector<float> out(M);
  for (int t = 0; t < M; ++t) {
    const double th = 0.05 * t;   // common slow phase law
    const double th6 = th + 2.0;  // bin6 carries a +2 rad offset
    const float a5 = (t < 20) ? 10.f : 1.f;
    const float a6 = (t < 20) ? 1.f : 10.f;
    FrameContext ctx;
    ctx.rangeCube = binFrame(16, 5,
                             {static_cast<float>(a5 * std::cos(th)),
                              static_cast<float>(a5 * std::sin(th))},
                             6,
                             {static_cast<float>(a6 * std::cos(th6)),
                              static_cast<float>(a6 * std::sin(th6))});
    CHECK(stage.process(ctx));
    out[t] = ctx.unwrappedPhaseRad;
  }

  CHECK(stage.switchCount() == 1);
  CHECK(stage.targetBin() == 6);
  // No frame-to-frame step may approach the 2 rad offset: the bridge absorbs
  // it (the switch frame contributes ~0, all others ~0.05).
  bool noFakeJump = true;
  for (int t = 1; t < M; ++t)
    if (std::abs(out[t] - out[t - 1]) > 0.1f) noFakeJump = false;
  CHECK(noFakeJump);
}

// Hysteresis: alternating +-5% amplitudes must never trigger a switch.
static void test_phase_switch_hysteresis() {
  RadarConfig cfg;
  cfg.numRxAnt = 1;
  cfg.numTxAnt = 1;
  cfg.numAdcSamples = 16;
  cfg.chirpStartIdx = 0;
  cfg.chirpEndIdx = 0;
  cfg.numLoops = 4;
  cfg.derive();

  PhaseUnwrapStage stage(cfg, PhaseUnwrapParams{});  // hold=5, ratio=1.25
  for (int t = 0; t < 50; ++t) {
    const float a4 = (t % 2 == 0) ? 10.5f : 9.5f;
    const float a5 = (t % 2 == 0) ? 9.5f : 10.5f;
    FrameContext ctx;
    ctx.rangeCube = binFrame(16, 4, {a4, 0.f}, 5, {a5, 0.f});
    CHECK(stage.process(ctx));
  }
  CHECK(stage.switchCount() == 0);
  CHECK(stage.targetBin() == 4);  // locked on frame 0, never ping-pongs
}

// Degenerate circle fit (truly static target: zero-span window) must keep the
// signal uncompensated -- stable output, no NaN, never a bogus center.
static void test_phase_dc_degenerate() {
  RadarConfig cfg;
  cfg.numRxAnt = 1;
  cfg.numTxAnt = 1;
  cfg.numAdcSamples = 16;
  cfg.chirpStartIdx = 0;
  cfg.chirpEndIdx = 0;
  cfg.numLoops = 4;
  cfg.derive();

  PhaseUnwrapParams p;
  p.targetRangeBin = 5;
  p.dcCompensation = true;
  p.dcWindowFrames = 32;
  PhaseUnwrapStage stage(cfg, p);

  bool stable = true;
  for (int t = 0; t < 100; ++t) {  // window fills; fit must stay degenerate
    FrameContext ctx;
    ctx.rangeCube = binFrame(16, 5, {10.f, 0.f});
    CHECK(stage.process(ctx));
    if (std::isnan(ctx.unwrappedPhaseRad) ||
        std::abs(ctx.unwrappedPhaseRad) > 1e-5f)
      stable = false;
  }
  CHECK(stable);
}

// ---------------------------------------------------------------------------
// ClutterRemovalStage
// ---------------------------------------------------------------------------

static void test_clutter_removal() {
    ClutterRemovalStage stage(0.05f);

    // 10 frames, cube {8 chirps, 2 rx, 8 bins}:
    //   * static return at bin 2 (constant across chirps AND frames)
    //   * moving target at bin 5: e^{j*2pi*c/8}, full cycle => chirp-mean == 0
    const int M = 10;
    bool staticGone = true, movingKept = true;
    for (int t = 0; t < M; ++t) {
        auto cube = std::make_shared<FrameBuffer>(FrameShape{8, 2, 8});
        for (std::size_t c = 0; c < 8; ++c)
            for (std::size_t r = 0; r < 2; ++r) {
                cube->at(c, r, 2) = {30.f * (r + 1), 40.f * (r + 1)};
                const double a = 2.0 * kPi * c / 8.0;
                cube->at(c, r, 5) = {static_cast<float>(10.0 * std::cos(a)),
                                     static_cast<float>(10.0 * std::sin(a))};
            }
        FrameContext ctx;
        ctx.rangeCube = cube;
        CHECK(stage.process(ctx));
        for (std::size_t c = 0; c < 8; ++c)
            for (std::size_t r = 0; r < 2; ++r) {
                // Static clutter nulled from frame 0 (map seeded on first frame).
                if (std::abs(cube->at(c, r, 2)) > 1e-3f) staticGone = false;
                // Doppler-rotating target untouched (its chirp-mean is zero).
                if (std::abs(std::abs(cube->at(c, r, 5)) - 10.f) > 1e-3f)
                    movingKept = false;
            }
    }
    CHECK(staticGone);
    CHECK(movingKept);

    // Shape change resets the map cleanly (no crash, static removed again).
    auto cube2 = std::make_shared<FrameBuffer>(FrameShape{4, 1, 16});
    for (std::size_t c = 0; c < 4; ++c) cube2->at(c, 0, 7) = {5.f, -5.f};
    FrameContext ctx2;
    ctx2.rangeCube = cube2;
    CHECK(stage.process(ctx2));
    bool resetOk = true;
    for (std::size_t c = 0; c < 4; ++c)
        if (std::abs(cube2->at(c, 0, 7)) > 1e-3f) resetOk = false;
    CHECK(resetOk);
}

// ---------------------------------------------------------------------------
// Full chain through the Pipeline (backpressure + order + products)
// ---------------------------------------------------------------------------

// Wire-format inverse of ParseStage: per (chirp, rx) block of int16, even s:
// blk[2s]=I_s, blk[2s+1]=I_{s+1}, blk[2s+2]=Q_s, blk[2s+3]=Q_{s+1}.
static std::shared_ptr<std::vector<std::uint8_t>>
makeRawFrame(const RadarConfig &cfg, int targetBin) {
  const int C = cfg.numChirpsPerFrame, R = cfg.numRxAnt, S = cfg.numAdcSamples;
  auto raw = std::make_shared<std::vector<std::uint8_t>>(
      static_cast<std::size_t>(cfg.bytesPerFrame));
  auto *p = reinterpret_cast<std::int16_t *>(raw->data());
  const std::size_t i16PerRx = static_cast<std::size_t>(S) * 2;
  const std::size_t i16PerChirp = i16PerRx * R;

  for (int c = 0; c < C; ++c) {
    for (int r = 0; r < R; ++r) {
      std::int16_t *blk = p + c * i16PerChirp + r * i16PerRx;
      for (int s = 0; s < S; ++s) {
        const double a = 2.0 * kPi * targetBin * s / static_cast<double>(S);
        const int ni = static_cast<int>(urand() * 40) - 20; // noise floor
        const int nq = static_cast<int>(urand() * 40) - 20;
        const std::int16_t I =
            static_cast<std::int16_t>(std::lround(1000.0 * std::cos(a)) + ni);
        const std::int16_t Q =
            static_cast<std::int16_t>(std::lround(1000.0 * std::sin(a)) + nq);
        const int se = (s / 2) * 2;
        if (s % 2 == 0) {
          blk[2 * se + 0] = I;
          blk[2 * se + 2] = Q;
        } else {
          blk[2 * se + 1] = I;
          blk[2 * se + 3] = Q;
        }
      }
    }
  }
  return raw;
}

class DspVerifySink : public IResultSink {
public:
  explicit DspVerifySink(int targetBin) : targetBin_(targetBin) {}
  void consume(const FrameContext &ctx) override {
    seqs.push_back(ctx.frameSeq);
    if (!ctx.valid || !ctx.parsed || !ctx.rangeCube || !ctx.dopplerCube ||
        !ctx.rdMap || !ctx.detections)
      allProducts = false;
    else {
      bool hit = false;
      for (const auto &det : *ctx.detections)
        if (std::abs(det.rangeBin - targetBin_) <= 1)
          hit = true;
      if (!hit)
        ++missedTarget;
    }
    if (std::isnan(ctx.unwrappedPhaseRad))
      phaseAlways = false;
  }
  std::vector<std::uint64_t> seqs;
  bool allProducts = true;
  bool phaseAlways = true;
  int missedTarget = 0;
  int targetBin_;
};

static void test_full_chain_pipeline() {
  RadarConfig cfg;
  cfg.numRxAnt = 4;
  cfg.numTxAnt = 1;
  cfg.numAdcSamples = 64;
  cfg.chirpStartIdx = 0;
  cfg.chirpEndIdx = 0;
  cfg.numLoops = 16;
  cfg.rxIdx = 0;
  cfg.numAngleBins = 64;
  cfg.derive();
  std::string err;
  CHECK(cfg.validate(err));

  const int kTargetBin = 8;

  auto parsePool = BufferPool<FrameBuffer>::create(
      [] { return std::make_unique<FrameBuffer>(); }, {}, 6);
  auto rangePool = BufferPool<FrameBuffer>::create(
      [] { return std::make_unique<FrameBuffer>(); }, {}, 6);
  auto dopplerPool = BufferPool<FrameBuffer>::create(
      [] { return std::make_unique<FrameBuffer>(); }, {}, 6);

  PhaseUnwrapParams pup;
  pup.targetRangeBin = kTargetBin;

  auto sink = std::make_shared<DspVerifySink>(kTargetBin);
  Pipeline p(/*inputCapacity=*/4); // tiny ring => real backpressure
  p.addStage(std::make_shared<ParseStage>(cfg, /*allRx=*/true, parsePool));
  p.addStage(std::make_shared<RangeFftStage>(cfg, rangePool));
  p.addStage(std::make_shared<DopplerFftStage>(cfg, dopplerPool));
  p.addStage(std::make_shared<CfarStage>(cfg));
  p.addStage(std::make_shared<AngleFftStage>(cfg));
  p.addStage(std::make_shared<PhaseUnwrapStage>(cfg, pup));
  p.addSink(sink);
  p.start();

  const std::uint64_t N = 200;
  for (std::uint64_t i = 0; i < N; ++i) {
    FrameContext ctx;
    ctx.frameSeq = i;
    ctx.raw = makeRawFrame(cfg, kTargetBin);
    CHECK(p.submit(std::move(ctx))); // blocking => lossless
  }
  p.stop();

  CHECK(sink->seqs.size() == N); // nothing lost
  bool ordered = true;
  for (std::uint64_t i = 0; i < sink->seqs.size(); ++i)
    if (sink->seqs[i] != i)
      ordered = false;
  CHECK(ordered);                 // strict order preserved
  CHECK(sink->allProducts);       // every stage produced its artifact
  CHECK(sink->missedTarget == 0); // CFAR found the injected target every frame
  CHECK(sink->phaseAlways);       // phase track present on every frame
}

int main() {
  test_fft_basics();
  test_range_fft();
  test_doppler_fft();
  test_cfar();
  test_angle_fft();
  test_phase_unwrap();
  test_phase_dc_compensation();
  test_phase_bridge_on_switch();
  test_phase_switch_hysteresis();
  test_phase_dc_degenerate();
  test_clutter_removal();
  test_full_chain_pipeline();

  std::cout << (g_total - g_fail) << "/" << g_total << " checks passed\n";
  if (g_fail) {
    std::cerr << g_fail << " CHECK(s) FAILED\n";
    return 1;
  }
  std::cout << "ALL PASS\n";
  return 0;
}
