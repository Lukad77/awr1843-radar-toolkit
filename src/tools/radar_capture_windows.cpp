// Windows native raw capture. Shared cfg parser/control/reassembler with Linux.
#include "transport/Dca1000Control.h"
#include "transport/Dca1000Reassembler.h"
#include "transport/MmwaveCfg.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void onSignal(int) { interrupted = 1; }
std::uint64_t clockNs() {
  LARGE_INTEGER t, f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f);
  return (t.QuadPart / f.QuadPart) * 1000000000ULL +
         (t.QuadPart % f.QuadPart) * 1000000000ULL / f.QuadPart;
}
bool stopRequested() {
  if (interrupted) return true;
  const HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
  if (GetFileType(h) != FILE_TYPE_PIPE) return false;
  DWORD n = 0;
  // Parent exit closes the pipe: stop hardware rather than orphan the capture.
  if (!PeekNamedPipe(h, nullptr, 0, nullptr, &n, nullptr)) return true;
  if (!n) return false;
  char buf[64]; DWORD read = 0;
  ReadFile(h, buf, sizeof(buf), &read, nullptr);
  return read > 0;
}
struct Network {
  Network() { WSADATA d{}; if (WSAStartup(MAKEWORD(2,2), &d)) throw std::runtime_error("WSAStartup failed"); }
  ~Network() { WSACleanup(); }
};
struct Socket {
  SOCKET value = INVALID_SOCKET;
  ~Socket() { if (value != INVALID_SOCKET) closesocket(value); }
};
}
int main(int argc, char **argv) {
  std::cout << std::unitbuf;
  std::signal(SIGINT, onSignal); std::signal(SIGTERM, onSignal);
  try {
    if (argc == 3 && std::string(argv[1]) == "--validate-cfg") {
      radar::MmwaveCfg check; std::string reason;
      if (!radar::loadMmwaveCfg(argv[2], check, reason)) throw std::runtime_error(reason);
      std::cout << "frameBytes=" << check.radar.bytesPerFrame << '\n';
      return 0;
    }
    Network network;
    radar::CaptureConfig cfg; radar::CliOverrides cli; std::string error;
    if (!radar::resolveCaptureConfig(argc, argv, cfg, cli, error)) throw std::runtime_error(error);
    radar::MmwaveCfg mmcfg;
    if (!cfg.cfg.empty() && !radar::loadMmwaveCfg(cfg.cfg, mmcfg, error)) throw std::runtime_error(error);
    const auto fromCfg = mmcfg.radar.bytesPerFrame;
    if (cfg.frameBytes && fromCfg && cfg.frameBytes != fromCfg) throw std::runtime_error("frameBytes disagrees with cfg");
    const std::size_t frameBytes = fromCfg ? fromCfg : cfg.frameBytes;
    if (!frameBytes || frameBytes > 256u * 1024u * 1024u) throw std::runtime_error("invalid frame size (limit 256 MiB)");
    const std::uint64_t maxFrames = cfg.maxFrames ? cfg.maxFrames : (cfg.noControl ? 0 : mmcfg.radar.numFrames);
    const auto path = std::filesystem::u8path(cfg.output);
    if (std::filesystem::exists(path) || std::filesystem::exists(std::filesystem::u8path(cfg.output + ".frames.csv")))
      throw std::runtime_error("output already exists; select a new session");
    std::ofstream raw(path, std::ios::binary);
    std::ofstream index(std::filesystem::u8path(cfg.output + ".frames.csv"));
    if (!raw || !index) throw std::runtime_error("cannot create output files");
    index << "file_frame,wire_frame,host_rx_monotonic_ns\n";
    Socket data; data.value = socket(AF_INET, SOCK_DGRAM, 0);
    if (data.value == INVALID_SOCKET) throw std::runtime_error("data socket failed");
    setsockopt(data.value, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&cfg.rcvbuf), sizeof(cfg.rcvbuf));
    sockaddr_in local{}, source{};
    if (!radar::resolveIpv4(cfg.bindIp, cfg.dataPort, local, error) ||
        !radar::resolveIpv4(cfg.dcaIp, cfg.dataPort, source, error)) throw std::runtime_error(error);
    if (bind(data.value, reinterpret_cast<sockaddr *>(&local), sizeof(local)))
      throw std::runtime_error("cannot bind data socket: " + cfg.bindIp + " (WSA " + std::to_string(WSAGetLastError()) + ")");
    std::atomic<bool> closing{false}, failed{false}, accepting{cfg.noControl};
    std::atomic<std::uint64_t> saved{0}, lastFrameNs{clockNs()};
    std::uint64_t packetNs = 0;
    std::string rxError;
    radar::Dca1000Reassembler reassembler(frameBytes, [&](std::uint64_t wire, const auto &frame) {
      if (maxFrames && saved.load() >= maxFrames) return false;
      raw.write(reinterpret_cast<const char *>(frame.data()), static_cast<std::streamsize>(frame.size()));
      index << saved.load() << ',' << wire << ',' << packetNs << '\n';
      if (!raw || !index) throw std::runtime_error("output write failed (disk full?)");
      ++saved; lastFrameNs = packetNs;
      return !maxFrames || saved.load() < maxFrames;
    });
    // Receive while commands are being acknowledged, including stop/drain.
    std::thread receiver([&] {
      try {
        std::uint8_t packet[65536];
        while (!closing) {
          fd_set ready; FD_ZERO(&ready); FD_SET(data.value, &ready);
          timeval wait{0, 100000};
          const int result = select(0, &ready, nullptr, nullptr, &wait);
          if (result < 0) throw std::runtime_error("data select failed");
          if (!result) continue;
          sockaddr_in from{}; int len = sizeof(from);
          const int n = recvfrom(data.value, reinterpret_cast<char *>(packet), sizeof(packet), 0, reinterpret_cast<sockaddr *>(&from), &len);
          packetNs = clockNs();
          if (n < 0) throw std::runtime_error("data receive failed");
          if (!accepting) continue;
          if (!cfg.noControl && from.sin_addr.s_addr != source.sin_addr.s_addr) continue;
          if (!reassembler.consume(packet, static_cast<std::size_t>(n))) break;
        }
      } catch (const std::exception &e) { rxError = e.what(); failed = true; }
    });
    std::unique_ptr<radar::Dca1000Control> control;
    bool ok = true;
    try {
      if (!cfg.noControl) {
        control = std::make_unique<radar::Dca1000Control>(radar::linkOptionsFrom(cfg));
        control->open(); control->connect(); control->configureFpga(); control->setPacketDelay();
        control->stopRadarOrThrow();
        if (stopRequested()) throw std::runtime_error("cancelled during configuration");
        control->sendCfgCommands(mmcfg.commands);
        if (stopRequested()) throw std::runtime_error("cancelled during configuration");
        std::cout << "ARMED " << clockNs() << '\n';
        accepting = true;
        control->startRecording(); control->radarStart();
      }
      lastFrameNs = clockNs();
      std::cout << "READY " << clockNs() << '\n';
      auto report = std::chrono::steady_clock::now();
      while (!stopRequested() && !failed && (!maxFrames || saved.load() < maxFrames)) {
        if (clockNs() - lastFrameNs.load() > 15000000000ULL) throw std::runtime_error("no complete radar frame for 15 seconds; check DCA1000 network/cfg");
        if (std::chrono::steady_clock::now() - report > std::chrono::milliseconds(500)) {
          std::cout << "PROGRESS " << saved.load() << ' ' << saved.load()*frameBytes << '\n';
          report = std::chrono::steady_clock::now();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    } catch (const std::exception &e) { std::cerr << "capture failed: " << e.what() << '\n'; ok = false; }
    if (control) {
      try { if (control->recording()) control->stopRecording(); }
      catch (const std::exception &e) { std::cerr << "stop failed: " << e.what() << '\n'; ok = false; }
      if (control->radarStarted() && !control->stopRadarBestEffort()) ok = false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    closing = true; receiver.join();
    if (failed) { std::cerr << rxError << '\n'; ok = false; }
    raw.flush(); index.flush();
    std::ofstream stats(std::filesystem::u8path(cfg.output + ".stats.txt"));
    stats << "savedFrames=" << saved.load() << "\nframeBytes=" << frameBytes
          << "\nmissingPackets=" << reassembler.missingPackets()
          << "\nlatePackets=" << reassembler.latePackets()
          << "\nmalformedPackets=" << reassembler.malformedPackets()
          << "\ndiscardedFrames=" << reassembler.discardedFrames() << '\n';
    stats.flush();
    std::cout << "PROGRESS " << saved.load() << ' ' << saved.load()*frameBytes << '\n';
    // A clean stop stores complete frames only; an in-flight trailing partial frame is omitted.
    return ok && raw && index && stats && saved && !reassembler.missingPackets() &&
           !reassembler.discardedFrames() && !reassembler.malformedPackets() ? 0 : 2;
  } catch (const std::exception &e) { std::cerr << "capture failed: " << e.what() << '\n'; return 1; }
}
