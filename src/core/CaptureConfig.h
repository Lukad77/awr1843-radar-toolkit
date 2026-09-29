#pragma once
// CaptureConfig.h — radar_capture 采集入口的参数单一事实源（JSON + 命令行）。
//
// 对应 README 路线图里的「配置外置（JSON）」。原先入口只接受命令行参数，
// 参数一多就难以复用与存档；这里把它收敛成一个带默认值的结构体：
//
//   * 本文件中的默认值 = 默认配置（字段未指定时保持不变）；
//   * `loadCaptureConfigFile()` 把 JSON 文件里的 "capture" 节合并到默认值之上；
//   * `applyCliOverrides()` 再把命令行显式给出的字段覆盖上去。
//
// 优先级：命令行 > JSON 文件 > 默认值。类型不匹配、未知键、缺文件等都由
// 这里给出带字段名的明确报错（不依赖第三方 JSON 库，解析见 core/Json.h）。

#include <cstdint>
#include <map>
#include <string>

#include "core/Json.h"

namespace radar {

struct CaptureConfig {
  // ---- 输出 ----
  std::string output; // 原始 ADC 帧落盘路径（必填）

  // ---- 硬件模式（noControl == false）----
  std::string cfg;    // mmWave CLI 配置文件路径（必填）
  std::string serial; // 雷达 CLI 串口设备（必填）

  // ---- DCA1000 UDP 链路 ----
  std::string bindIp = "0.0.0.0";       // 本机绑定地址
  std::string dcaIp = "192.168.33.180"; // DCA1000 地址
  int dataPort = 4098;                  // 数据端口
  int configPort = 4096;                // 命令端口
  int packetDelayUs = 25;               // 发包间隔 5..500 us
  int lvdsLanes = 4;                    // LVDS 通道数 2 或 4
  int rcvbuf = 8 * 1024 * 1024;         // SO_RCVBUF 请求值（>= 65536）

  // ---- 被动模式（noControl == true）----
  std::uint64_t frameBytes = 0; // 每帧字节数；0 表示由 cfg 推导
  std::uint64_t maxFrames = 0;  // 采集上限；0 表示直到 Ctrl-C
  bool noControl = false;       // true => 不控制 DCA1000/雷达，只收包
};

// 把已解析 JSON 文档的 "capture" 节合并到 *this（未列出的键保持原值）。
// 未知键、类型不匹配、缺少 "capture" 节都会失败并把原因写入 `err`。
bool applyCaptureJson(const JsonValue &root, CaptureConfig &cfg, std::string &err);

// 读取并合并 JSON 配置文件（默认值 -> JSON）。
bool loadCaptureConfigFile(const std::string &path, CaptureConfig &cfg,
                           std::string &err);

// 解析后的命令行：JSON 路径 + 「规范字段名 -> 原始文本」的覆盖项。
struct CliOverrides {
  std::string jsonPath;                      // --json <path>，未给出时为空
  std::map<std::string, std::string> values; // 例：{"maxFrames", "1000"}

  bool empty() const { return values.empty(); }
};

// 解析 argv（拒绝未知选项、缺少取值），`--no-control` 记作 noControl=true。
bool parseCli(int argc, char **argv, CliOverrides &out, std::string &err);

// 把命令行覆盖项应用到 `cfg`（命令行优先于 JSON 与默认值）。
bool applyCliOverrides(const CliOverrides &cli, CaptureConfig &cfg,
                       std::string &err);

// 跨字段一致性校验（端口范围、缓冲区下限、模式必需项）。
bool validateCaptureConfig(const CaptureConfig &cfg, std::string &err);

} // namespace radar
