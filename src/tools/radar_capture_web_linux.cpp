#ifndef __linux__
#error radar_capture_web is Linux-only
#endif
// radar_capture_web_linux.cpp — 真机采集 + DSP 流水线 + 落盘 + 网页实时显示。
//
// 与只落盘的 radar_capture 的区别（两者共用控制面与参数体系）：
//
//   控制面 Dca1000Control：0x09/0x03/0x0B -> sensorStop -> cfg -> 0x05 -> sensorStart
//   数据面 Dca1000UdpSource（IFrameSource）：
//        rx 线程 recvfrom -> 帧重组 -> FrameSpool（两级无损，RAM 满溢写磁盘）
//        -> next() 按 FIFO 交给 Pipeline
//   处理面 Pipeline：Parse -> RangeFFT -> PhaseUnwrap -> ClutterRemoval
//        -> DopplerFFT -> CA-CFAR -> AngleFFT（与 radar_web_demo 同序）
//   输出面扇出：RawFileSink（无损落盘，产物与 radar_capture 一致）
//              + WsFrameSink（网页实时显示；慢客户端丢帧，绝不反压 DSP）
//
// 于是：采集永不丢帧（背压由 FrameSpool 用磁盘容量吸收），网页显示按需丢帧，
// 两者互不影响。用法见 README 与 docs/JETSON_NANO_ACCEPTANCE.md 3.1 节。

#include <csignal>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/BufferPool.h"
#include "core/CaptureConfig.h"
#include "core/FrameBuffer.h"
#include "core/Interfaces.h"
#include "dsp/AngleFftStage.h"
#include "dsp/CfarStage.h"
#include "dsp/ClutterRemovalStage.h"
#include "dsp/DopplerFftStage.h"
#include "dsp/PhaseUnwrapStage.h"
#include "dsp/RangeFftStage.h"
#include "pipeline/ParseStage.h"
#include "pipeline/Pipeline.h"
#include "transport/Dca1000Control.h"
#include "transport/Dca1000UdpSource.h"
#include "transport/MmwaveCfg.h"
#include "transport/RawFileSink.h"
#include "web/WsFrameSink.h"

namespace {
volatile std::sig_atomic_t stopping = 0;
void onSignal(int) { stopping = 1; }

const char *baseName(const std::string &path) {
  const auto cut = path.find_last_of("/\\");
  return path.c_str() + (cut == std::string::npos ? 0 : cut + 1);
}
} // namespace

int main(int argc, char **argv) {
  try {
    radar::CliOverrides cli;
    radar::CaptureConfig cfg;
    std::string configErr;
    if (!radar::resolveCaptureConfig(argc, argv, cfg, cli, configErr))
      throw std::runtime_error(configErr);
    std::cout << "config: json=" << (cli.jsonPath.empty() ? "<none>" : cli.jsonPath)
              << " cliOverrides=" << cli.values.size() << '\n';

    // 流水线必须知道雷达剖面（FFT 点数/分辨率/坐标轴换算），因此本入口要求
    // 提供 .cfg —— 这也让被动模式（--no-control，无硬件回放验收）可以工作。
    if (cfg.cfg.empty())
      throw std::runtime_error(
          "radar cfg is required for the web pipeline "
          "(JSON capture.cfg or --cfg)");
    radar::MmwaveCfg mmcfg;
    std::string cfgErr;
    if (!radar::loadMmwaveCfg(cfg.cfg, mmcfg, cfgErr))
      throw std::runtime_error(cfgErr);
    const radar::RadarConfig &rcfg = mmcfg.radar;
    const std::size_t frameBytes = static_cast<std::size_t>(rcfg.bytesPerFrame);
    if (cfg.frameBytes && cfg.frameBytes != frameBytes)
      throw std::runtime_error("frameBytes disagrees with the radar cfg frame size");
    const float fps =
        rcfg.framePeriodicityMs > 0.f ? 1000.f / rcfg.framePeriodicityMs : 20.f;

    // ---- 数据源：真机 UDP -> 帧重组 -> 两级无损缓冲 ----
    radar::Dca1000UdpSource::Options srcOpts;
    srcOpts.bindIp = cfg.bindIp;
    srcOpts.dataPort = cfg.dataPort;
    srcOpts.frameBytes = frameBytes;
    srcOpts.dcaIp = cfg.noControl ? std::string() : cfg.dcaIp; // 被动模式不限来源
    srcOpts.rcvbuf = static_cast<std::size_t>(cfg.rcvbuf);
    srcOpts.ramCapFrames = static_cast<std::size_t>(cfg.spoolFrames);
    srcOpts.spillPath = cfg.output + ".spill"; // FrameSpool 析构时自动删除
    srcOpts.stopFlag = &stopping;
    radar::Dca1000UdpSource source(std::move(srcOpts));
    if (!source.open()) throw std::runtime_error(source.lastError());
    std::cout << "frameBytes=" << frameBytes << " dataPort=" << cfg.dataPort
              << " spoolFrames=" << cfg.spoolFrames << " fps=" << fps << '\n';

    // ---- 输出面 1：无损落盘（产物与 radar_capture 一致）----
    auto fileSink = std::make_shared<radar::RawFileSink>(cfg.output, frameBytes);
    if (!fileSink->ok()) throw std::runtime_error(fileSink->lastError());
    fileSink->setExtraStats([&source] {
      std::ostringstream o;
      o << "framesReceived=" << source.framesReceived() << '\n'
        << "missingPackets=" << source.missingPackets() << '\n'
        << "latePackets=" << source.latePackets() << '\n'
        << "malformedPackets=" << source.malformedPackets() << '\n'
        << "discardedFrames=" << source.discardedFrames() << '\n'
        << "spoolWriteFailures=" << source.droppedFrames() << '\n'
        << "spillPeakFrames=" << source.spillPeak() << '\n';
      return o.str();
    });

    // ---- 输出面 2：网页实时显示（慢客户端丢帧，绝不反压 DSP）----
    auto wsSink = std::make_shared<radar::WsFrameSink>(
        rcfg, cfg.webPort, fps, baseName(cfg.cfg));
    if (!wsSink->start())
      throw std::runtime_error("WebSocket 服务启动失败（端口 " +
                               std::to_string(cfg.webPort) + " 被占用？）");

    // ---- 处理面：与 radar_web_demo 相同的算子顺序 ----
    auto mkPool = [] {
      return radar::BufferPool<radar::FrameBuffer>::create(
          [] { return std::make_unique<radar::FrameBuffer>(); }, {}, 4);
    };
    radar::Pipeline pipeline(/*inputCapacity=*/8);
    pipeline.addStage(
        std::make_shared<radar::ParseStage>(rcfg, /*allRx=*/true, mkPool()));
    pipeline.addStage(std::make_shared<radar::RangeFftStage>(rcfg, mkPool()));
    pipeline.addStage(
        std::make_shared<radar::PhaseUnwrapStage>(rcfg, radar::PhaseUnwrapParams{}));
    pipeline.addStage(std::make_shared<radar::ClutterRemovalStage>(/*alpha=*/0.02f));
    pipeline.addStage(std::make_shared<radar::DopplerFftStage>(rcfg, mkPool()));
    pipeline.addStage(std::make_shared<radar::CfarStage>(rcfg));
    pipeline.addStage(std::make_shared<radar::AngleFftStage>(rcfg));
    pipeline.addSink(fileSink);
    pipeline.addSink(wsSink);
    pipeline.start();

    // ---- 控制面：与 radar_capture 同一份时序（数据 socket 已就绪）----
    std::unique_ptr<radar::Dca1000Control> ctrl;
    if (!cfg.noControl) {
      ctrl = std::make_unique<radar::Dca1000Control>(radar::linkOptionsFrom(cfg));
      ctrl->open();
      ctrl->connect();        // 0x09
      ctrl->configureFpga();  // 0x03
      ctrl->setPacketDelay(); // 0x0B
      ctrl->stopRadarOrThrow();
      ctrl->sendCfgCommands(mmcfg.commands);
      ctrl->startRecording(); // 0x05
      ctrl->radarStart();     // sensorStart
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::cout << "WebSocket 服务: ws://localhost:" << cfg.webPort
              << "（浏览器打开 web/index.html 查看实时图表；Ctrl-C 停止）\n";

    // 循环终止条件：达到 maxFrames，或数据源在停止后排空（Ctrl-C => 源线程
    // 退出 -> 关闭 spool -> next() 把已收帧全部排空后才返回 false，故不丢帧）。
    std::uint64_t submitted = 0;
    for (;;) {
      if (cfg.maxFrames && submitted >= cfg.maxFrames) break;
      radar::FrameContext ctx;
      if (!source.next(ctx)) break;
      if (!pipeline.submit(std::move(ctx))) break;
      ++submitted;
      if (submitted % 200 == 0)
        std::cout << "已提交 " << submitted
                  << " 帧 | 客户端 " << wsSink->clientCount()
                  << " | 已发送 " << wsSink->framesSent()
                  << " | 发送丢帧 " << wsSink->framesDropped()
                  << " | 缓冲 " << source.ramDepth() << '\r' << std::flush;
    }

    // ---- 收尾：先停数据面（并 join，之后统计才可安全读取）----
    source.close();
    if (ctrl) {
      if (ctrl->recording()) {
        try {
          ctrl->stopRecording();
        } catch (const std::exception &e) {
          std::cerr << "DCA stop failed: " << e.what() << '\n';
        }
      }
      if (ctrl->radarStarted()) ctrl->stopRadarBestEffort();
    }
    pipeline.stop(); // 排空流水线、flush 各 sink（含 stats.txt）
    wsSink->stop();

    std::cout << "\n提交 " << submitted << " 帧，落盘 " << fileSink->savedFrames()
              << " 帧（跳过 " << fileSink->skippedFrames() << "，尺寸不符 "
              << fileSink->shortFrames() << "），网页发送 " << wsSink->framesSent()
              << " 帧（丢 " << wsSink->framesDropped() << "），缺包 "
              << source.missingPackets() << "，丢弃帧 " << source.discardedFrames()
              << '\n';
    if (source.droppedFrames() > 0)
      std::cerr << "warning: " << source.droppedFrames()
                << " 帧因溢写磁盘写入失败而丢失（检查磁盘空间/写入速度）\n";

    return fileSink->ok() && submitted && source.missingPackets() == 0 &&
                   source.discardedFrames() == 0 && source.malformedPackets() == 0
               ? 0
               : 2;
  } catch (const std::exception &e) {
    std::cerr << "capture failed: " << e.what() << '\n';
    return 1;
  }
}
