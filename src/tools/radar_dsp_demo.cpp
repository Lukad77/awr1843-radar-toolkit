// radar_dsp_demo.cpp — end-to-end DSP chain over a recorded ADC bin file
// (no hardware): Parse -> RangeFFT -> PhaseUnwrap -> ClutterRemoval ->
// DopplerFFT -> CA-CFAR -> AngleFFT, with per-stage timing and a
// respiration-phase CSV.
//
// PhaseUnwrap runs BEFORE ClutterRemoval on purpose: the vital-signs phase
// must come from the unfiltered rangeCube (see ClutterRemovalStage.h), while
// Doppler/CFAR consume the clutter-suppressed cube.
//
// Stages are invoked sequentially on this thread (same code path the
// Pipeline worker runs) so each stage can be timed individually.
//
// Usage:
//   radar_dsp_demo <adc_raw.bin> [maxFrames] [phase.csv]
//     maxFrames: 0 = whole file (default)
//     phase.csv: default "radar_phase.csv"
//
// Dataset default: 4 Rx x 64 chirps x 256 samples complex int16
// (262144 B/frame), AWR1843 77 GHz profile.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
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
#include "dsp/PhaseCsvSink.h"
#include "dsp/PhaseUnwrapStage.h"
#include "dsp/RangeFftStage.h"
#include "pipeline/ParseStage.h"

using namespace radar;
using Clock = std::chrono::steady_clock;

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "用法: " << argv[0] << " <adc_raw.bin> [maxFrames] [phase.csv]"
              << std::endl;
    return 1;
  }
  const std::string binPath = argv[1];
  const long maxFrames = (argc >= 3) ? std::stol(argv[2]) : 0;
  const std::string csvPath = (argc >= 4) ? argv[3] : "radar_phase.csv";

  // ---- 配置（与数据集/awr1843.cfg 对齐的 AWR1843 77GHz 剖面）----
  RadarConfig cfg;
  cfg.numRxAnt = 4;
  cfg.numTxAnt = 1;
  cfg.numAdcSamples = 256;
  cfg.rxIdx = 0;
  cfg.chirpStartIdx = 0;
  cfg.chirpEndIdx = 0;
  cfg.numLoops = 64;
  cfg.numFrames = 2000;
  cfg.startFreqGHz = 77.f;
  cfg.idleTimeUs = 100.f;
  cfg.rampEndTimeUs = 60.f;
  cfg.freqSlopeMHzPerUs = 60.f;
  cfg.digOutSampleRateKsps = 10000;
  cfg.numAngleBins = 64;
  cfg.derive();
  std::string err;
  if (!cfg.validate(err)) {
    std::cerr << "配置非法: " << err << std::endl;
    return 1;
  }

  std::ifstream in(binPath, std::ios::binary);
  if (!in) {
    std::cerr << "无法打开文件: " << binPath << std::endl;
    return 1;
  }

  std::cout << "帧大小 " << cfg.bytesPerFrame << " B, 距离分辨率 "
            << cfg.rangeResolutionMeters << " m, 速度分辨率 "
            << cfg.dopplerResolutionMps << " m/s, λ=" << cfg.lambdaM * 1000
            << " mm" << std::endl;

  // ---- 组装算子链（与 Pipeline.addStage 顺序一致）+ 缓冲池 ----
  auto mkPool = [] {
    return BufferPool<FrameBuffer>::create(
        [] { return std::make_unique<FrameBuffer>(); }, {}, 4);
  };
  auto phaseStage =
      std::make_shared<PhaseUnwrapStage>(cfg, PhaseUnwrapParams{});
  std::vector<std::shared_ptr<IStage>> stages = {
      std::make_shared<ParseStage>(cfg, /*allRx=*/true, mkPool()),
      std::make_shared<RangeFftStage>(cfg, mkPool()),
      phaseStage,  // 生命体征相位取自未滤波 rangeCube（见 ClutterRemovalStage.h）
      std::make_shared<ClutterRemovalStage>(/*alpha=*/0.02f),
      std::make_shared<DopplerFftStage>(cfg, mkPool()),
      std::make_shared<CfarStage>(cfg),
      std::make_shared<AngleFftStage>(cfg),
  };
  std::vector<double> stageMs(stages.size(), 0.0);
  PhaseCsvSink phaseSink(csvPath);

  // ---- 逐帧处理 ----
  std::uint64_t frames = 0, invalid = 0, totalDets = 0;
  Detection strongest; // 全程最强目标
  float strongestSnr = -1.f;
  auto raw = std::make_shared<std::vector<std::uint8_t>>(
      static_cast<std::size_t>(cfg.bytesPerFrame));

  while (in.read(reinterpret_cast<char *>(raw->data()),
                 static_cast<std::streamsize>(raw->size()))) {
    FrameContext ctx;
    ctx.frameSeq = frames;
    ctx.raw = raw;

    for (std::size_t i = 0; i < stages.size(); ++i) {
      const auto t0 = Clock::now();
      stages[i]->process(ctx);
      stageMs[i] +=
          std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    }
    phaseSink.consume(ctx);

    if (!ctx.valid)
      ++invalid;
    if (ctx.detections) {
      totalDets += ctx.detections->size();
      for (const auto &d : *ctx.detections)
        if (d.snrDb > strongestSnr) {
          strongestSnr = d.snrDb;
          strongest = d;
        }
    }
    ++frames;
    if (frames % 200 == 0) {
      std::cout << "frame " << frames
                << ": dets=" << (ctx.detections ? ctx.detections->size() : 0)
                << ", phase=" << ctx.unwrappedPhaseRad
                << " rad, disp=" << ctx.displacementMm << " mm" << std::endl;
    }
    if (maxFrames > 0 && frames >= static_cast<std::uint64_t>(maxFrames))
      break;
  }
  phaseSink.flush();

  // ---- 汇总 ----
  std::cout << "------------------------------------------" << std::endl;
  std::cout << "处理帧数: " << frames << " (invalid " << invalid << ")"
            << std::endl;
  if (frames == 0)
    return 1;
  double totalMs = 0.0;
  const char *names[] = {"Parse",          "RangeFFT", "PhaseUnwrap",
                       "ClutterRemoval", "DopplerFFT", "CA-CFAR",
                       "AngleFFT"};
  for (std::size_t i = 0; i < stages.size(); ++i) {
    std::cout << "  " << names[i] << ": " << stageMs[i] / frames << " ms/帧"
              << std::endl;
    totalMs += stageMs[i];
  }
  std::cout << "  DSP 合计: " << totalMs / frames << " ms/帧 (吞吐上限 "
            << 1000.0 / (totalMs / frames) << " fps)" << std::endl;
  std::cout << "平均检测数/帧: " << static_cast<double>(totalDets) / frames
            << std::endl;
  std::cout << "最强目标: range=" << strongest.rangeM
            << " m, v=" << strongest.velocityMps
            << " m/s, angle=" << strongest.angleDeg
            << " deg, SNR=" << strongestSnr << " dB (rangeBin "
            << strongest.rangeBin << ")" << std::endl;
  std::cout << "呼吸相位跟踪 bin: " << phaseStage->targetBin() << " (≈"
            << phaseStage->targetBin() * cfg.rangeIdxToMeters << " m)"
            << std::endl;
  std::cout << "相位波形 CSV: " << csvPath << std::endl;
  return invalid == 0 ? 0 : 1;
}
