#include "transport/Dca1000Control.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

#include <iostream>
#include <utility>

namespace radar {
namespace {

std::uint16_t le16(const std::uint8_t *p) {
  return std::uint16_t(p[0]) | (std::uint16_t(p[1]) << 8);
}

void put16(std::vector<std::uint8_t> &v, std::uint16_t x) {
  v.push_back(static_cast<std::uint8_t>(x));
  v.push_back(static_cast<std::uint8_t>(x >> 8));
}

sockaddr_in address(const std::string &ip, int port) {
  sockaddr_in a{};
  std::string err;
  if (!resolveIpv4(ip, port, a, err)) throw std::runtime_error(err);
  return a;
}

void writeAll(int fd, const std::string &s) {
  const char *p = s.data();
  std::size_t n = s.size();
  while (n) {
    const ssize_t k = ::write(fd, p, n);
    if (k < 0 && errno == EINTR) continue;
    if (k <= 0) throw std::runtime_error("serial write failed");
    p += k;
    n -= static_cast<std::size_t>(k);
  }
}

std::string printable(const std::string &s) {
  if (s.empty()) return "<no reply>";
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (c == '\r' || c == '\n') out += "\\n";
    else if (u < 0x20 || u == 0x7f) out += '.';
    else out += c;
  }
  return out;
}

// Discard anything already pending on the port. The CLI prints its prompt as a
// separate chunk after "Done", so without this the trailing prompt of one
// command is mis-attributed to the reply of the next one.
void drainInput(int fd, int quietMs) {
  for (;;) {
    pollfd p{fd, POLLIN, 0};
    if (::poll(&p, 1, quietMs) <= 0) return;
    char buf[256];
    if (::read(fd, buf, sizeof(buf)) <= 0) return;
  }
}

void serialCommand(int fd, const std::string &line, int timeoutMs = 3000) {
  drainInput(fd, 60);
  writeAll(fd, line + "\n");
  std::string reply;
  bool acknowledged = false;
  for (;;) {
    // Allow timeoutMs for the CLI to react; once it acknowledged, only wait for
    // the link to go quiet (the response may arrive in several USB chunks).
    pollfd p{fd, POLLIN, 0};
    if (::poll(&p, 1, acknowledged ? 150 : timeoutMs) <= 0) {
      if (acknowledged) break;
      throw std::runtime_error("serial timeout: " + line +
                               " (reply so far: " + printable(reply) + ")");
    }
    char buf[256];
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n <= 0) throw std::runtime_error("serial read failed: " + line);
    reply.append(buf, static_cast<std::size_t>(n));
    if (reply.find("Error") != std::string::npos ||
        reply.find("not recognized") != std::string::npos ||
        reply.find("Unknown") != std::string::npos ||
        reply.find("Invalid") != std::string::npos)
      throw std::runtime_error("radar rejected " + line + ": " +
                               printable(reply));
    // "Done" for accepted commands; "Ignored: ..." for benign no-ops such as
    // sensorStop on an already-stopped sensor (that one has no "Done").
    if (reply.find("Done") != std::string::npos ||
        reply.find("Ignored") != std::string::npos)
      acknowledged = true;
    // The xWR18xx CLI prompt (e.g. "mmwDemo:/>") terminates a response.
    if (reply.find(":/>") != std::string::npos) break;
    if (reply.size() > 8192) throw std::runtime_error("serial reply too long");
  }
  // Do not fire the next line before the CLI finished printing its prompt:
  // sending while it is busy makes the radar UART drop the command's leading
  // bytes (observed as 'eDataOutputMode' instead of 'dfeDataOutputMode').
  ::usleep(20000);
}

} // namespace

bool resolveIpv4(const std::string &ip, int port, sockaddr_in &out,
                 std::string &err) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<std::uint16_t>(port));
  if (::inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) {
    err = "invalid IPv4 address: " + ip;
    return false;
  }
  out = a;
  return true;
}

Dca1000Control::Dca1000Control(Dca1000LinkOptions opts) : o_(std::move(opts)) {}

Dca1000LinkOptions linkOptionsFrom(const CaptureConfig &cfg) {
  Dca1000LinkOptions link;
  link.bindIp = cfg.bindIp;
  link.dcaIp = cfg.dcaIp;
  link.configPort = cfg.configPort;
  link.lvdsLanes = cfg.lvdsLanes;
  link.packetDelayUs = cfg.packetDelayUs;
  link.serial = cfg.serial;
  return link;
}

Dca1000Control::~Dca1000Control() {
  shutdownQuietly();
  close();
}

void Dca1000Control::open() {
  ctrl_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (ctrl_ < 0) throw std::runtime_error("command socket failed");
  timeval timeout{2, 0};
  ::setsockopt(ctrl_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  const auto local = address(o_.bindIp, o_.configPort);
  if (::bind(ctrl_, reinterpret_cast<const sockaddr *>(&local), sizeof(local)))
    throw std::runtime_error("config port bind failed");
  dca_ = address(o_.dcaIp, o_.configPort);

  if (o_.serial.empty()) return; // 只控制 DCA1000（不碰雷达串口）

  // The XDS110 re-enumerates on reset/power-cycle, so the port can be
  // briefly absent right when the tool starts. Retry instead of failing.
  for (int attempt = 1; attempt <= o_.serialOpenAttempts; ++attempt) {
    serial_ = ::open(o_.serial.c_str(), O_RDWR | O_NOCTTY | O_SYNC);
    if (serial_ >= 0) break;
    if (attempt == o_.serialOpenAttempts)
      throw std::runtime_error("cannot open serial: " + o_.serial + ": " +
                               std::strerror(errno));
    std::cerr << "serial " << o_.serial << " not ready (" << std::strerror(errno)
              << "), retrying...\n";
    ::usleep(500000); // 0.5 s，共约 5 s
  }
  termios tty{};
  if (::tcgetattr(serial_, &tty)) throw std::runtime_error("tcgetattr failed");
  ::cfmakeraw(&tty);
  ::cfsetispeed(&tty, B115200);
  ::cfsetospeed(&tty, B115200);
  tty.c_cflag |= CLOCAL | CREAD;
  if (::tcsetattr(serial_, TCSANOW, &tty))
    throw std::runtime_error("tcsetattr failed");
  ::tcflush(serial_, TCIFLUSH);
}

void Dca1000Control::close() {
  if (ctrl_ >= 0) {
    ::close(ctrl_);
    ctrl_ = -1;
  }
  if (serial_ >= 0) {
    ::close(serial_);
    serial_ = -1;
  }
}

void Dca1000Control::sendCommand(std::uint16_t code,
                                 const std::vector<std::uint8_t> &payload) {
  std::vector<std::uint8_t> req;
  put16(req, 0xA55A);
  put16(req, code);
  put16(req, static_cast<std::uint16_t>(payload.size()));
  req.insert(req.end(), payload.begin(), payload.end());
  put16(req, 0xEEAA);
  if (::sendto(ctrl_, req.data(), req.size(), 0,
               reinterpret_cast<const sockaddr *>(&dca_),
               sizeof(dca_)) != static_cast<ssize_t>(req.size()))
    throw std::runtime_error("DCA command send failed");
  for (;;) {
    std::uint8_t resp[512];
    sockaddr_in from{};
    socklen_t fromLen = sizeof(from);
    const ssize_t n = ::recvfrom(ctrl_, resp, sizeof(resp), 0,
                                 reinterpret_cast<sockaddr *>(&from), &fromLen);
    if (n < 0) throw std::runtime_error("DCA command timed out or recv failed");
    // DCA1000 may reply from UDP 1024 even when commands target UDP 4096.
    // Match the device IP, command code and packet envelope instead.
    if (from.sin_addr.s_addr != dca_.sin_addr.s_addr || n != 8 ||
        le16(resp) != 0xA55A || le16(resp + 6) != 0xEEAA ||
        le16(resp + 2) != code)
      continue;
    if (le16(resp + 4) != 0)
      throw std::runtime_error("DCA rejected command " + std::to_string(code));
    return;
  }
}

void Dca1000Control::connect() { sendCommand(0x09); }

void Dca1000Control::configureFpga() {
  std::vector<std::uint8_t> fpga{
      1, static_cast<std::uint8_t>(o_.lvdsLanes == 4 ? 1 : 2), 1, 2, 3, 30};
  sendCommand(0x03, fpga); // raw, LVDS, Ethernet, 16-bit
}

void Dca1000Control::setPacketDelay() {
  std::vector<std::uint8_t> packet;
  put16(packet, 1472);
  put16(packet, static_cast<std::uint16_t>(o_.packetDelayUs));
  put16(packet, 0);
  sendCommand(0x0B, packet);
}

void Dca1000Control::startRecording() {
  sendCommand(0x05);
  recording_ = true;
}

void Dca1000Control::stopRecording() {
  sendCommand(0x06);
  recording_ = false;
}

void Dca1000Control::sendSerial(const std::string &line, int timeoutMs) {
  serialCommand(serial_, line, timeoutMs);
}

bool Dca1000Control::tryStopRadar(int attempts, int timeoutMs) {
  for (int attempt = 1; attempt <= attempts; ++attempt) {
    try {
      serialCommand(serial_, "sensorStop", timeoutMs);
      radarStarted_ = false;
      return true;
    } catch (const std::exception &e) {
      std::cerr << "sensorStop attempt " << attempt << "/" << attempts
                << " failed: " << e.what() << '\n';
    }
  }
  return false;
}

void Dca1000Control::stopRadarOrThrow(int attempts, int timeoutMs) {
  if (tryStopRadar(attempts, timeoutMs)) return;
  // A silent CLI is not a transient condition: after a capture the mmWave demo
  // CLI stops answering for good, so retrying the whole cfg sequence only
  // produces a confusing timeout on the first config command.
  throw std::runtime_error(
      "radar CLI is not responding to sensorStop. Per-frame UART reporting "
      "(cfg guiMonitor) starves the demo CLI task, and it does not recover on "
      "its own: power-cycle or reset the AWR1843BOARD and run again.");
}

bool Dca1000Control::stopRadarBestEffort(int attempts, int timeoutMs) {
  if (tryStopRadar(attempts, timeoutMs)) return true;
  radarStopTried_ = true;
  std::cerr << "warning: radar CLI did not acknowledge sensorStop; the "
               "captured data is complete. Check that the cfg keeps "
               "guiMonitor at all-zero (per-frame UART reporting starves the "
               "CLI task) and power-cycle/reset the AWR1843BOARD before the "
               "next run.\n";
  return false;
}

void Dca1000Control::sendCfgCommands(const std::vector<std::string> &commands) {
  for (const auto &line : commands) serialCommand(serial_, line);
}

void Dca1000Control::radarStart() {
  serialCommand(serial_, "sensorStart");
  radarStarted_ = true;
}

void Dca1000Control::shutdownQuietly() {
  // Same order as the normal shutdown path: quiet the DCA1000 before asking
  // the radar to stop.
  try {
    if (recording_) stopRecording();
  } catch (const std::exception &e) {
    std::cerr << "DCA stop failed: " << e.what() << '\n';
  }
  try {
    if (radarStarted_ && !radarStopTried_) serialCommand(serial_, "sensorStop");
  } catch (const std::exception &e) {
    std::cerr << "sensorStop (final attempt) failed: " << e.what() << '\n';
  }
}

} // namespace radar
