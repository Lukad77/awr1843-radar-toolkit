#pragma once
// MmwaveCfg.h — 把 TI mmWave CLI 的 .cfg 解析成「雷达参数 + 可下发命令序列」。
//
// 采集链路需要两样东西，过去散在 radar_capture 的私有函数 cfgFrameBytes() 里：
//   1) RadarConfig（采样/chirp/Rx 维度、帧周期、分辨率派生值）——帧重组、
//      ParseStage 与整条 DSP 流水线都依赖它，是单一事实源；
//   2) 逐条 CLI 命令，供串口下发（sensorStart/sensorStop 由调用方按硬件时序
//      自己发，这里剔除）。
//
// 只接受当前采集固件支持的那一档配置：单 TX、单 chirp、16 位复数 ADC、
// lvdsStreamCfg 不带头；其余配置**明确报错**而不是按错参数静默解析
// （参数错了会一路污染帧尺寸、FFT 维度与显示坐标轴）。

#include <string>
#include <vector>

#include "core/RadarConfig.h"

namespace radar {

struct MmwaveCfg {
  RadarConfig radar;                 // 已 derive() 且 validate()
  std::vector<std::string> commands; // 逐条 CLI 命令（去注释、无 sensorStart/Stop）
};

// 解析 `path`；失败时把原因写入 `err`。
bool loadMmwaveCfg(const std::string &path, MmwaveCfg &out, std::string &err);

} // namespace radar
