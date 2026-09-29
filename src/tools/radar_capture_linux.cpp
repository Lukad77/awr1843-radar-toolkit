#ifndef __linux__
#error radar_capture is Linux-only
#endif
// radar_capture_linux.cpp — DCA1000 真机采集（Linux/Jetson），只落盘。
//
// 数据面：UDP 收包 -> Dca1000Reassembler（48 位 byte count 重组）-> 原始帧
//         直接写文件（零中间队列，最省 CPU/内存，适合只取数据的场景）。
// 控制面：Dca1000Control（0x09/0x03/0x0B -> sensorStop -> cfg -> 0x05 -> sensorStart）。
// 参数：CaptureConfig（命令行 > JSON > 默认值，见 core/CaptureConfig.h）。
//
// 需要「采集 + DSP 流水线 + 网页实时显示」时用 radar_capture_web（同源的
// IFrameSource 版本），本工具刻意不引入 DSP/Web 依赖。

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/CaptureConfig.h"
#include "transport/Dca1000Control.h"
#include "transport/Dca1000Reassembler.h"
#include "transport/MmwaveCfg.h"

namespace {
volatile std::sig_atomic_t stopping = 0;
void onSignal(int) { stopping = 1; }

struct Fd {
  int value = -1;
  explicit Fd(int fd = -1) : value(fd) {}
  ~Fd() { if (value >= 0) ::close(value); }
  Fd(const Fd &) = delete;
  Fd &operator=(const Fd &) = delete;
};

} // namespace

int main(int argc, char **argv) {
  try {
    radar::CliOverrides cli;
    radar::CaptureConfig cfg;
    std::string configErr;
    if (!radar::resolveCaptureConfig(argc, argv, cfg, cli, configErr))
      throw std::runtime_error(configErr);
    // 把参数来源显式打出来，便于确认「命令行 > JSON > 默认值」的实际生效情况。
    std::cout << "config: json=" << (cli.jsonPath.empty() ? "<none>" : cli.jsonPath)
              << " cliOverrides=" << cli.values.size() << '\n';

    radar::MmwaveCfg mmcfg;
    if (!cfg.cfg.empty()) {
      std::string cfgErr;
      if (!radar::loadMmwaveCfg(cfg.cfg, mmcfg, cfgErr))
        throw std::runtime_error(cfgErr);
    }
    const std::size_t cfgBytes = static_cast<std::size_t>(mmcfg.radar.bytesPerFrame);
    if (cfg.frameBytes && cfgBytes && cfg.frameBytes != cfgBytes)
      throw std::runtime_error("frameBytes disagrees with the radar cfg frame size");
    const std::size_t frameBytes = cfgBytes ? cfgBytes : cfg.frameBytes;

    std::ofstream raw(cfg.output, std::ios::binary | std::ios::trunc);
    std::ofstream index(cfg.output + ".frames.csv", std::ios::trunc);
    if (!raw || !index) throw std::runtime_error("cannot create output files");
    index << "file_frame,wire_frame\n";

    Fd data(::socket(AF_INET, SOCK_DGRAM, 0));
    if (data.value < 0) throw std::runtime_error("data socket failed");
    ::setsockopt(data.value, SOL_SOCKET, SO_RCVBUF, &cfg.rcvbuf, sizeof(cfg.rcvbuf));
    sockaddr_in bindAddr{};
    std::string addrErr;
    if (!radar::resolveIpv4(cfg.bindIp, cfg.dataPort, bindAddr, addrErr))
      throw std::runtime_error(addrErr);
    if (::bind(data.value, reinterpret_cast<const sockaddr *>(&bindAddr),
               sizeof(bindAddr)))
      throw std::runtime_error("data port bind failed: " +
                               std::string(std::strerror(errno)));
    int actualBuf = 0;
    socklen_t len = sizeof(actualBuf);
    ::getsockopt(data.value, SOL_SOCKET, SO_RCVBUF, &actualBuf, &len);
    std::cout << "frameBytes=" << frameBytes << " SO_RCVBUF=" << actualBuf << '\n';

    std::unique_ptr<radar::Dca1000Control> ctrl;
    if (!cfg.noControl) {
      ctrl = std::make_unique<radar::Dca1000Control>(radar::linkOptionsFrom(cfg));
      ctrl->open();
      ctrl->connect();        // 0x09 连通性
      ctrl->configureFpga();  // 0x03 原始 ADC / LVDS / 以太网 / 16 bit
      ctrl->setPacketDelay(); // 0x0B 每包延迟
      ctrl->stopRadarOrThrow();
      ctrl->sendCfgCommands(mmcfg.commands);
      ctrl->startRecording(); // 0x05
      ctrl->radarStart();     // sensorStart
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::uint64_t saved = 0;
    std::uint64_t lastReported = 0;
    radar::Dca1000Reassembler reassembler(
        frameBytes, [&](std::uint64_t wireFrame,
                        const std::vector<std::uint8_t> &frame) {
          raw.write(reinterpret_cast<const char *>(frame.data()),
                    static_cast<std::streamsize>(frame.size()));
          index << saved << ',' << wireFrame << '\n';
          if (!raw || !index) throw std::runtime_error("output write failed");
          ++saved;
          return true;
        });

    std::uint8_t packet[65536];
    while (!stopping && (!cfg.maxFrames || saved < cfg.maxFrames)) {
      pollfd p{data.value, POLLIN, 0};
      const int ready = ::poll(&p, 1, 200);
      if (ready < 0 && errno == EINTR) continue;
      if (ready < 0) throw std::runtime_error("data poll failed");
      if (!ready) continue;
      sockaddr_in from{};
      socklen_t fromLen = sizeof(from);
      const ssize_t n = ::recvfrom(data.value, packet, sizeof(packet), 0,
                                   reinterpret_cast<sockaddr *>(&from), &fromLen);
      if (n < 0 && errno == EINTR) continue;
      if (n < 0) throw std::runtime_error("data receive failed");
      if (ctrl && from.sin_addr.s_addr != ctrl->dcaAddress().sin_addr.s_addr)
        continue;
      reassembler.consume(packet, static_cast<std::size_t>(n));
      if (saved && saved % 100 == 0 && saved != lastReported) {
        lastReported = saved;
        std::cout << "saved=" << saved
                  << " missingPackets=" << reassembler.missingPackets()
                  << " discardedFrames=" << reassembler.discardedFrames() << '\r'
                  << std::flush;
      }
    }

    // Shutdown is best-effort: the capture loop already wrote complete frames,
    // so a stop command that is ignored by the CLI must not discard the run
    // (it used to abort before the stats file was written).
    // The DCA1000 is stopped first so the data path is quiet before the radar
    // is asked to stop.
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

    raw.flush();
    index.flush();
    std::ofstream stats(cfg.output + ".stats.txt", std::ios::trunc);
    stats << "savedFrames=" << saved << '\n'
          << "frameBytes=" << frameBytes << '\n'
          << "missingPackets=" << reassembler.missingPackets() << '\n'
          << "latePackets=" << reassembler.latePackets() << '\n'
          << "malformedPackets=" << reassembler.malformedPackets() << '\n'
          << "discardedFrames=" << reassembler.discardedFrames() << '\n';
    std::cout << "\nsaved=" << saved
              << " missingPackets=" << reassembler.missingPackets()
              << " latePackets=" << reassembler.latePackets()
              << " malformedPackets=" << reassembler.malformedPackets()
              << " discardedFrames=" << reassembler.discardedFrames() << '\n';
    return raw && index && stats && saved && !reassembler.missingPackets() &&
                   !reassembler.discardedFrames() &&
                   !reassembler.malformedPackets()
               ? 0
               : 2;
  } catch (const std::exception &e) {
    std::cerr << "capture failed: " << e.what() << '\n';
    return 1;
  }
}
