// radar_web_demo.cpp — 离线回放 + WebSocket 实时推送：浏览器打开
// web/index.html 即可实时查看原始波形 / 距离谱 / 相位位移 / 呼吸波形。
//
// 与 radar_dsp_demo 不同，本工具走真实 Pipeline（addStage×7 +
// addSink(WsFrameSink) + start/submit/stop）：submit 满则阻塞（背压），
// WsFrameSink 内部对慢客户端丢帧，DSP 节奏不受网络影响。
//
// 帧节奏：按 cfg.framePeriodicityMs（数据集为 50ms = 20fps）用
// sleep_until 定时提交，模拟实盘到帧节拍。
//
// 用法：
//   radar_web_demo <adc_raw.bin> [port] [--loop]
//     port  : WebSocket 监听端口，默认 8765
//     --loop: 文件读尽后回卷到起点继续（frameSeq 持续递增；
//             PhaseUnwrapStage 有状态，回卷处相位有一次 <=pi 的
//             桥接跳变，属预期显示行为）
//
// 每帧提交一个独立的 raw 缓冲（经 BufferPool 复用）：Pipeline 是
// 异步消费，绝不能像 radar_dsp_demo 那样复用单一缓冲边读边交。

#include <chrono>
#include <csignal>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/BufferPool.h"
#include "core/FrameBuffer.h"
#include "core/Interfaces.h"
#include "core/RadarConfig.h"
#include "dsp/AngleFftStage.h"
#include "dsp/CfarStage.h"
#include "dsp/ClutterRemovalStage.h"
#include "dsp/DopplerFftStage.h"
#include "dsp/PhaseUnwrapStage.h"
#include "dsp/RangeFftStage.h"
#include "pipeline/ParseStage.h"
#include "pipeline/Pipeline.h"
#include "web/WsFrameSink.h"

using namespace radar;
using Clock = std::chrono::steady_clock;

namespace {
volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }
} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "用法: " << argv[0] << " <adc_raw.bin> [port] [--loop]"
              << std::endl;
    return 1;
  }
  const std::string binPath = argv[1];
  int port = 8765;
  bool loop = false;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--loop")
      loop = true;
    else
      port = std::stoi(a);
  }

  // ---- 配置（与 radar_dsp_demo 相同的数据集剖面）----
  RadarConfig cfg;
  cfg.numRxAnt = 4;
  cfg.numTxAnt = 1;
  cfg.numAdcSamples = 256;
  cfg.rxIdx = 0;
  cfg.chirpStartIdx = 0;
  cfg.chirpEndIdx = 0;
  cfg.numLoops = 64;
  cfg.numFrames = 2000;
  cfg.framePeriodicityMs = 50.f; // 数据集帧率 20 fps
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
  const float fps = 1000.f / cfg.framePeriodicityMs;

  std::ifstream in(binPath, std::ios::binary);
  if (!in) {
    std::cerr << "无法打开文件: " << binPath << std::endl;
    return 1;
  }

  // ---- 组装 Pipeline（顺序与 radar_dsp_demo 一致）----
  auto mkPool = [] {
    return BufferPool<FrameBuffer>::create(
        [] { return std::make_unique<FrameBuffer>(); }, {}, 4);
  };
  Pipeline pipeline(/*inputCapacity=*/8);
  pipeline.addStage(std::make_shared<ParseStage>(cfg, /*allRx=*/true, mkPool()));
  pipeline.addStage(std::make_shared<RangeFftStage>(cfg, mkPool()));
  pipeline.addStage(
      std::make_shared<PhaseUnwrapStage>(cfg, PhaseUnwrapParams{}));
  pipeline.addStage(std::make_shared<ClutterRemovalStage>(/*alpha=*/0.02f));
  pipeline.addStage(std::make_shared<DopplerFftStage>(cfg, mkPool()));
  pipeline.addStage(std::make_shared<CfarStage>(cfg));
  pipeline.addStage(std::make_shared<AngleFftStage>(cfg));

  const std::string srcName =
      binPath.substr(binPath.find_last_of("/\\") + 1);
  auto sink = std::make_shared<WsFrameSink>(cfg, port, fps, srcName);
  if (!sink->start()) {
    std::cerr << "WebSocket 服务启动失败（端口 " << port << " 被占用？）"
              << std::endl;
    return 1;
  }
  pipeline.addSink(sink);
  pipeline.start();

  std::signal(SIGINT, onSignal);
  std::cout << "WebSocket 服务: ws://localhost:" << port << " (帧率 " << fps
            << " fps" << (loop ? ", 循环回放" : "") << ")\n"
            << "浏览器打开 web/index.html 查看实时图表；Ctrl-C 退出。"
            << std::endl;

  // ---- 定时回放：raw 缓冲经池复用，逐帧独立提交 ----
  auto rawPool = BufferPool<std::vector<std::uint8_t>>::create(
      [&] {
        return std::make_unique<std::vector<std::uint8_t>>(
            static_cast<std::size_t>(cfg.bytesPerFrame));
      },
      {}, 8);

  const auto period = std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<double, std::milli>(cfg.framePeriodicityMs));
  // 递增式 deadline（而非 t0 + period*frames）：避免 uint64 帧计数把
  // duration 的 rep 提升成无符号后，sleep_until 内部 deadline-now 在
  // "已迟到"时下溢成天文数字（实测表现为首帧即永睡）。
  auto deadline = Clock::now();
  std::uint64_t frames = 0;

  while (!g_stop) {
    auto raw = rawPool->acquire();
    raw->resize(static_cast<std::size_t>(cfg.bytesPerFrame));
    if (!in.read(reinterpret_cast<char *>(raw->data()),
                 static_cast<std::streamsize>(raw->size()))) {
      if (!loop)
        break;
      in.clear();
      in.seekg(0); // 回卷继续（相位桥接跳变见文件头说明）
      continue;
    }

    std::this_thread::sleep_until(deadline);
    deadline += period;
    FrameContext ctx;
    ctx.frameSeq = frames;
    ctx.raw = raw;
    ctx.tCaptured = Clock::now();
    if (!pipeline.submit(std::move(ctx)))
      break; // pipeline 已停止
    ++frames;

    if (frames % 200 == 0)
      std::cout << "已提交 " << frames << " 帧 | 客户端 "
                << sink->clientCount() << " | 已发送 " << sink->framesSent()
                << " | 发送队列丢帧 " << sink->framesDropped() << std::endl;
  }

  pipeline.stop();
  sink->stop();
  std::cout << "结束：提交 " << frames << " 帧，发送 " << sink->framesSent()
            << " 帧，丢弃 " << sink->framesDropped() << " 帧。" << std::endl;
  return 0;
}
