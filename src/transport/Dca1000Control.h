#pragma once
// Dca1000Control.h — DCA1000 命令链 + 雷达串口 CLI 的控制面（Linux / Windows）。
//
// 这段时序原本内联在 radar_capture 的匿名命名空间里。为了让「纯采集落盘」
// （radar_capture）与「采集 + 流水线 + 实时显示」（radar_capture_web）共用
// 同一份硬件时序，抽成独立模块；**命令内容、下发顺序与报错消息与原来逐字
// 一致**，排障步骤见 docs/JETSON_NANO_ACCEPTANCE.md 第 3、6 节。
//
// 关键时序（踩过的坑都在这里）：
//   启动：0x09 连通性 -> 0x03 FPGA（原始 ADC/LVDS/以太网/16bit）-> 0x0B 发包
//         间隔 -> sensorStop（重试；无响应即可判定 CLI 已卡死）
//         -> 逐条下发 .cfg 命令 -> 0x05 开始记录 -> sensorStart
//   收尾：先 0x06 停记录，再 sensorStop（顺序原因见验收文档 6.1）

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <netinet/in.h>
#endif

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/CaptureConfig.h"

namespace radar {

// 把 "IP:port" 解析为 sockaddr_in（命令 socket 与数据 socket 共用）。
bool resolveIpv4(const std::string &ip, int port, sockaddr_in &out,
                 std::string &err);

struct Dca1000LinkOptions {
  // Optional diagnostic events around Windows serial writes and accepted replies.
  std::function<void(const std::string &)> trace;
  std::string bindIp = "0.0.0.0";       // 本机绑定地址（命令口）
  std::string dcaIp = "192.168.33.180"; // DCA1000 地址
  int configPort = 4096;                // DCA1000 命令端口
  int lvdsLanes = 4;                    // LVDS 通道数（2 或 4）
  int packetDelayUs = 25;               // 0x0B 每包延迟
  std::string serial;                   // 雷达 CLI 串口；空 => 不控制雷达
  int serialOpenAttempts = 10;          // 打开串口重试次数（每次间隔 0.5 s）
};

// 由采集配置生成链路参数，供两个采集入口共用（避免时序参数两处维护）。
Dca1000LinkOptions linkOptionsFrom(const CaptureConfig &cfg);

class Dca1000Control {
public:
  explicit Dca1000Control(Dca1000LinkOptions opts);
  ~Dca1000Control();

  Dca1000Control(const Dca1000Control &) = delete;
  Dca1000Control &operator=(const Dca1000Control &) = delete;

  // Windows 调用方须先 WSAStartup；建命令 socket 并 bind；（配置了 serial 时）打开并配置串口。失败抛异常。
  void open();
  // 关闭 fd，幂等。不发送任何停止命令。
  void close();

  // ---- DCA1000 命令链 ----
  void connect();        // 0x09 连通性
  void configureFpga();  // 0x03 原始 ADC / LVDS / 以太网 / 16 bit
  void setPacketDelay(); // 0x0B 每包延迟
  void startRecording(); // 0x05
  void stopRecording();  // 0x06

  // ---- 雷达串口 CLI ----
  bool hasSerial() const {
#ifdef _WIN32
    return serial_ != INVALID_HANDLE_VALUE;
#else
    return serial_ >= 0;
#endif
  }
  void sendSerial(const std::string &line, int timeoutMs = 3000);
  // 启动前：确保传感器处于停止态；重试 attempts 次仍无响应则抛异常。
  void stopRadarOrThrow(int attempts = 3, int timeoutMs = 5000);
  // 收尾：尽力停止；失败只告警并返回 false（数据已完整落盘，不应据此判失败）。
  bool stopRadarBestEffort(int attempts = 3, int timeoutMs = 8000);
  void sendCfgCommands(const std::vector<std::string> &commands);
  void radarStart();

  // 析构/显式收尾：先停 DCA1000 记录，再尽力停雷达。
  void shutdownQuietly();

  // 已解析的 DCA1000 地址（数据路径按来源地址过滤时使用）。
  const sockaddr_in &dcaAddress() const { return dca_; }
  bool recording() const { return recording_; }
  bool radarStarted() const { return radarStarted_; }

private:
  void sendCommand(std::uint16_t code,
                   const std::vector<std::uint8_t> &payload = {});
  bool tryStopRadar(int attempts, int timeoutMs);

  Dca1000LinkOptions o_;
#ifdef _WIN32
  SOCKET ctrl_ = INVALID_SOCKET;
  HANDLE serial_ = INVALID_HANDLE_VALUE;
#else
  int ctrl_ = -1;
  int serial_ = -1;
#endif
  sockaddr_in dca_{};
  bool recording_ = false;
  bool radarStarted_ = false;
  // 正常收尾路径已经重试过 sensorStop 后置位：析构不再重复同一次失败尝试，
  // 避免在结果汇总之后再打一条令人误解的报错。
  bool radarStopTried_ = false;
};

} // namespace radar
