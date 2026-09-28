#ifndef __linux__
#error radar_capture is Linux-only
#endif

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sstream>
#include <vector>

#include "transport/Dca1000Reassembler.h"

namespace {
volatile std::sig_atomic_t stopping = 0;
void onSignal(int) { stopping = 1; }

struct Options {
  std::string bindIp = "0.0.0.0", dcaIp = "192.168.33.180";
  std::string serial, cfg, output;
  int dataPort = 4098, configPort = 4096, packetDelayUs = 25;
  int lvdsLanes = 4, rcvbuf = 8 * 1024 * 1024;
  std::size_t frameBytes = 0;
  std::uint64_t maxFrames = 0;
  bool noControl = false;
};

struct Fd {
  int value = -1;
  explicit Fd(int fd = -1) : value(fd) {}
  ~Fd() { if (value >= 0) ::close(value); }
  Fd(const Fd &) = delete;
  Fd &operator=(const Fd &) = delete;
};

struct CaptureStop {
  int ctrl = -1, serial = -1;
  sockaddr_in dca{};
  bool recording = false, radarStarted = false;
  ~CaptureStop();
};

std::uint16_t le16(const std::uint8_t *p) {
  return std::uint16_t(p[0]) | (std::uint16_t(p[1]) << 8);
}
void put16(std::vector<std::uint8_t> &v, std::uint16_t x) {
  v.push_back(static_cast<std::uint8_t>(x));
  v.push_back(static_cast<std::uint8_t>(x >> 8));
}

Options parseArgs(int argc, char **argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--no-control") { o.noControl = true; continue; }
    if (i + 1 == argc) throw std::runtime_error("missing value for " + key);
    const std::string val = argv[++i];
    if (key == "--output") o.output = val;
    else if (key == "--cfg") o.cfg = val;
    else if (key == "--serial") o.serial = val;
    else if (key == "--bind-ip") o.bindIp = val;
    else if (key == "--dca-ip") o.dcaIp = val;
    else if (key == "--data-port") o.dataPort = std::stoi(val);
    else if (key == "--config-port") o.configPort = std::stoi(val);
    else if (key == "--packet-delay-us") o.packetDelayUs = std::stoi(val);
    else if (key == "--lvds-lanes") o.lvdsLanes = std::stoi(val);
    else if (key == "--rcvbuf") o.rcvbuf = std::stoi(val);
    else if (key == "--frame-bytes") o.frameBytes = std::stoull(val);
    else if (key == "--max-frames") o.maxFrames = std::stoull(val);
    else throw std::runtime_error("unknown option " + key);
  }
  if (o.output.empty()) throw std::runtime_error("--output is required");
  if (!o.noControl && (o.cfg.empty() || o.serial.empty()))
    throw std::runtime_error("hardware mode requires --cfg and --serial");
  if (o.noControl && !o.frameBytes && o.cfg.empty())
    throw std::runtime_error("passive mode requires --frame-bytes or --cfg");
  if (o.dataPort < 1 || o.dataPort > 65535 || o.configPort < 1 || o.configPort > 65535 ||
      o.packetDelayUs < 5 || o.packetDelayUs > 500 ||
      (o.lvdsLanes != 2 && o.lvdsLanes != 4) || o.rcvbuf < 65536)
    throw std::runtime_error("invalid port, packet delay, LVDS lanes or receive buffer");
  return o;
}

// Only the supported single-TX, complex 16-bit raw ADC profile is accepted.
std::size_t cfgFrameBytes(const std::string &path, std::vector<std::string> &commands) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open radar cfg: " + path);
  int rxMask = -1, samples = -1, chirpStart = -1, chirpEnd = -1, loops = -1;
  int adcBits = -1, adcFmt = -1, lvdsFmt = -1, lvdsHeader = -1;
  std::vector<int> chirpTxMasks;
  std::string line;
  while (std::getline(in, line)) {
    const auto cut = line.find_first_of("%#");
    if (cut != std::string::npos) line.resize(cut);
    std::istringstream ss(line);
    std::vector<std::string> t;
    for (std::string word; ss >> word;) t.push_back(word);
    if (t.empty() || t[0] == "sensorStart" || t[0] == "sensorStop") continue;
    if (t[0] == "channelCfg" && t.size() >= 3) rxMask = std::stoi(t[1]);
    if (t[0] == "profileCfg" && t.size() >= 11) samples = std::stoi(t[10]);
    if (t[0] == "frameCfg" && t.size() >= 4) {
      chirpStart = std::stoi(t[1]); chirpEnd = std::stoi(t[2]); loops = std::stoi(t[3]);
    }
    if (t[0] == "adcCfg" && t.size() >= 3) {
      adcBits = std::stoi(t[1]); adcFmt = std::stoi(t[2]);
    }
    if (t[0] == "chirpCfg" && t.size() >= 9) chirpTxMasks.push_back(std::stoi(t[8]));
    if (t[0] == "lvdsStreamCfg" && t.size() >= 4) {
      lvdsHeader = std::stoi(t[2]); lvdsFmt = std::stoi(t[3]);
    }
    commands.push_back(line);
  }
  if (rxMask <= 0 || samples <= 0 || chirpStart < 0 || chirpEnd != chirpStart ||
      loops <= 0 || adcBits != 2 || adcFmt != 1 || lvdsHeader != 0 || lvdsFmt != 1 ||
      chirpTxMasks.empty() ||
      std::any_of(chirpTxMasks.begin(), chirpTxMasks.end(), [](int mask) {
        return mask <= 0 || (mask & (mask - 1)) != 0;
      }))
    throw std::runtime_error("cfg requires one TX chirp, complex 16-bit ADC and LVDS raw ADC without headers");
  const int rx = __builtin_popcount(static_cast<unsigned>(rxMask));
  return static_cast<std::size_t>(rx) * samples * loops * 4;
}

sockaddr_in address(const std::string &ip, int port) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<std::uint16_t>(port));
  if (::inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1)
    throw std::runtime_error("invalid IPv4 address: " + ip);
  return a;
}

void command(int fd, const sockaddr_in &dca, std::uint16_t code,
             const std::vector<std::uint8_t> &payload = {}) {
  std::vector<std::uint8_t> req;
  put16(req, 0xA55A); put16(req, code);
  put16(req, static_cast<std::uint16_t>(payload.size()));
  req.insert(req.end(), payload.begin(), payload.end());
  put16(req, 0xEEAA);
  if (::sendto(fd, req.data(), req.size(), 0, reinterpret_cast<const sockaddr *>(&dca),
               sizeof(dca)) != static_cast<ssize_t>(req.size()))
    throw std::runtime_error("DCA command send failed");
  for (;;) {
    std::uint8_t resp[512];
    sockaddr_in from{}; socklen_t fromLen = sizeof(from);
    const ssize_t n = ::recvfrom(fd, resp, sizeof(resp), 0,
                                 reinterpret_cast<sockaddr *>(&from), &fromLen);
    if (n < 0) throw std::runtime_error("DCA command timed out or recv failed");
    // DCA1000 may reply from UDP 1024 even when commands target UDP 4096.
    // Match the device IP, command code and packet envelope instead.
    if (from.sin_addr.s_addr != dca.sin_addr.s_addr ||
        n != 8 || le16(resp) != 0xA55A || le16(resp + 6) != 0xEEAA ||
        le16(resp + 2) != code) continue;
    if (le16(resp + 4) != 0)
      throw std::runtime_error("DCA rejected command " + std::to_string(code));
    return;
  }
}

void writeAll(int fd, const std::string &s) {
  const char *p = s.data(); std::size_t n = s.size();
  while (n) {
    const ssize_t k = ::write(fd, p, n);
    if (k < 0 && errno == EINTR) continue;
    if (k <= 0) throw std::runtime_error("serial write failed");
    p += k; n -= static_cast<std::size_t>(k);
  }
}

void serialCommand(int fd, const std::string &line) {
  writeAll(fd, line + "\n");
  std::string reply;
  for (;;) {
    pollfd p{fd, POLLIN, 0};
    if (::poll(&p, 1, 3000) <= 0) throw std::runtime_error("serial timeout: " + line);
    char buf[256]; const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n <= 0) throw std::runtime_error("serial read failed: " + line);
    reply.append(buf, static_cast<std::size_t>(n));
    if (reply.find("Error") != std::string::npos)
      throw std::runtime_error("radar rejected " + line + ": " + reply);
    if (reply.find("Done") != std::string::npos) return;
    if (reply.size() > 8192) throw std::runtime_error("serial reply too long");
  }
}

CaptureStop::~CaptureStop() {
  try {
    if (radarStarted) serialCommand(serial, "sensorStop");
  } catch (const std::exception &e) {
    std::cerr << "sensorStop failed: " << e.what() << '\n';
  }
  try {
    if (recording) command(ctrl, dca, 0x06);
  } catch (const std::exception &e) {
    std::cerr << "DCA stop failed: " << e.what() << '\n';
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    const Options o = parseArgs(argc, argv);
    std::vector<std::string> cfgCommands;
    const std::size_t cfgBytes = o.cfg.empty() ? 0 : cfgFrameBytes(o.cfg, cfgCommands);
    if (o.frameBytes && cfgBytes && o.frameBytes != cfgBytes)
      throw std::runtime_error("--frame-bytes disagrees with radar cfg");
    const std::size_t frameBytes = cfgBytes ? cfgBytes : o.frameBytes;
    std::ofstream raw(o.output, std::ios::binary | std::ios::trunc);
    std::ofstream index(o.output + ".frames.csv", std::ios::trunc);
    if (!raw || !index) throw std::runtime_error("cannot create output files");
    index << "file_frame,wire_frame\n";
    Fd data(::socket(AF_INET, SOCK_DGRAM, 0));
    if (data.value < 0) throw std::runtime_error("data socket failed");
    ::setsockopt(data.value, SOL_SOCKET, SO_RCVBUF, &o.rcvbuf, sizeof(o.rcvbuf));
    const auto bindAddr = address(o.bindIp, o.dataPort);
    if (::bind(data.value, reinterpret_cast<const sockaddr *>(&bindAddr), sizeof(bindAddr)))
      throw std::runtime_error("data port bind failed: " + std::string(std::strerror(errno)));
    int actualBuf = 0; socklen_t len = sizeof(actualBuf);
    ::getsockopt(data.value, SOL_SOCKET, SO_RCVBUF, &actualBuf, &len);
    std::cout << "frameBytes=" << frameBytes << " SO_RCVBUF=" << actualBuf << '\n';

    Fd ctrl, serial;
    sockaddr_in dca{};
    CaptureStop cleanup;
    if (!o.noControl) {
      ctrl.value = ::socket(AF_INET, SOCK_DGRAM, 0);
      if (ctrl.value < 0) throw std::runtime_error("command socket failed");
      timeval timeout{2, 0};
      ::setsockopt(ctrl.value, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      const auto local = address(o.bindIp, o.configPort);
      if (::bind(ctrl.value, reinterpret_cast<const sockaddr *>(&local), sizeof(local)))
        throw std::runtime_error("config port bind failed");
      dca = address(o.dcaIp, o.configPort);
      cleanup.ctrl = ctrl.value; cleanup.dca = dca;
      serial.value = ::open(o.serial.c_str(), O_RDWR | O_NOCTTY | O_SYNC);
      if (serial.value < 0) throw std::runtime_error("cannot open serial: " + o.serial);
      cleanup.serial = serial.value;
      termios tty{};
      if (::tcgetattr(serial.value, &tty)) throw std::runtime_error("tcgetattr failed");
      ::cfmakeraw(&tty);
      ::cfsetispeed(&tty, B115200); ::cfsetospeed(&tty, B115200);
      tty.c_cflag |= CLOCAL | CREAD;
      if (::tcsetattr(serial.value, TCSANOW, &tty)) throw std::runtime_error("tcsetattr failed");
      ::tcflush(serial.value, TCIFLUSH);
      command(ctrl.value, dca, 0x09); // connectivity
      std::vector<std::uint8_t> fpga{1, static_cast<std::uint8_t>(o.lvdsLanes == 4 ? 1 : 2),
                                     1, 2, 3, 30};
      command(ctrl.value, dca, 0x03, fpga); // raw, LVDS, Ethernet, 16-bit
      std::vector<std::uint8_t> packet;
      put16(packet, 1472); put16(packet, static_cast<std::uint16_t>(o.packetDelayUs));
      put16(packet, 0);
      command(ctrl.value, dca, 0x0B, packet);
      serialCommand(serial.value, "sensorStop");
      for (const auto &line : cfgCommands) serialCommand(serial.value, line);
      command(ctrl.value, dca, 0x05); cleanup.recording = true;
      serialCommand(serial.value, "sensorStart"); cleanup.radarStarted = true;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::uint64_t saved = 0;
    std::uint64_t lastReported = 0;
    radar::Dca1000Reassembler reassembler(frameBytes,
      [&](std::uint64_t wireFrame, const std::vector<std::uint8_t> &frame) {
        raw.write(reinterpret_cast<const char *>(frame.data()),
                  static_cast<std::streamsize>(frame.size()));
        index << saved << ',' << wireFrame << '\n';
        if (!raw || !index) throw std::runtime_error("output write failed");
        ++saved;
        return true;
      });
    std::uint8_t packet[65536];
    while (!stopping && (!o.maxFrames || saved < o.maxFrames)) {
      pollfd p{data.value, POLLIN, 0};
      const int ready = ::poll(&p, 1, 200);
      if (ready < 0 && errno == EINTR) continue;
      if (ready < 0) throw std::runtime_error("data poll failed");
      if (!ready) continue;
      sockaddr_in from{}; socklen_t fromLen = sizeof(from);
      const ssize_t n = ::recvfrom(data.value, packet, sizeof(packet), 0,
                                   reinterpret_cast<sockaddr *>(&from), &fromLen);
      if (n < 0 && errno == EINTR) continue;
      if (n < 0) throw std::runtime_error("data receive failed");
      if (!o.noControl && from.sin_addr.s_addr != dca.sin_addr.s_addr) continue;
      reassembler.consume(packet, static_cast<std::size_t>(n));
      if (saved && saved % 100 == 0 && saved != lastReported) {
        lastReported = saved;
        std::cout << "saved=" << saved << " missingPackets=" << reassembler.missingPackets()
                  << " discardedFrames=" << reassembler.discardedFrames() << '\r' << std::flush;
      }
    }
    if (cleanup.radarStarted) {
      serialCommand(serial.value, "sensorStop"); cleanup.radarStarted = false;
    }
    if (cleanup.recording) {
      command(ctrl.value, dca, 0x06); cleanup.recording = false;
    }
    raw.flush(); index.flush();
    std::ofstream stats(o.output + ".stats.txt", std::ios::trunc);
    stats << "savedFrames=" << saved << '\n'
          << "frameBytes=" << frameBytes << '\n'
          << "missingPackets=" << reassembler.missingPackets() << '\n'
          << "latePackets=" << reassembler.latePackets() << '\n'
          << "malformedPackets=" << reassembler.malformedPackets() << '\n'
          << "discardedFrames=" << reassembler.discardedFrames() << '\n';
    std::cout << "\nsaved=" << saved << " missingPackets=" << reassembler.missingPackets()
              << " latePackets=" << reassembler.latePackets()
              << " malformedPackets=" << reassembler.malformedPackets()
              << " discardedFrames=" << reassembler.discardedFrames() << '\n';
    return raw && index && stats && saved && !reassembler.missingPackets() &&
                   !reassembler.discardedFrames() && !reassembler.malformedPackets() ? 0 : 2;
  } catch (const std::exception &e) {
    std::cerr << "capture failed: " << e.what() << '\n';
    return 1;
  }
}
