#pragma once
// RadarConfig.h — 采集 + 处理参数的单一事实源。
//
// 收敛了过去三处漂移的表示（RadarParams、AWR1843Controller::ConfigParams
// 以及松散的 frameCfg 字段）。主字段由 .cfg/用户设置；derive() 一次性
// 计算全部派生值（bytesPerFrame、numRangeBins、分辨率等），validate()
// 在不一致性破坏帧重组/解析之前就把它们拦下。

#include <string>

namespace radar {

struct RadarConfig {
  // ---- 主字段：数据格式 ----
  int numAdcBits = 16;
  bool isReal = false; // false => 复数 I/Q（4 字节/采样点）
  int numRxAnt = 4;
  int numTxAnt = 1;
  int numAdcSamples = 256;
  int rxIdx = 0;         // 单 Rx 解析时选用的天线（0 基）
  int numAngleBins = 64; // 角度 FFT 点数（零填充虚拟阵列，2 的幂）

  // ---- 主字段：frameCfg ----
  int chirpStartIdx = 0;
  int chirpEndIdx = 0;
  int numLoops = 0;
  int numFrames = 0;
  float framePeriodicityMs = 0.f;
  int triggerSelect = 0;
  float triggerDelay = 0.f;

  // ---- 主字段：profileCfg ----
  float startFreqGHz = 0.f;
  float idleTimeUs = 0.f;
  float rampEndTimeUs = 0.f;
  float freqSlopeMHzPerUs = 0.f;
  int digOutSampleRateKsps = 0;

  // ---- 派生字段（由 derive() 填充）----
  int numChirpsPerFrame = 0;
  int numDopplerBins = 0;
  int numRangeBins = 0;
  int bytesPerSample = 4; // 4（复数 int16）或 2（实数 int16）
  long bytesPerFrame = 0; // chirps * rx * samples * bytesPerSample
  float rangeResolutionMeters = 0.f;
  float rangeIdxToMeters = 0.f;
  float dopplerResolutionMps = 0.f;
  float maxRange = 0.f;
  float maxVelocity = 0.f;
  int numVirtualAnt = 0; // numTxAnt * numRxAnt（角度 FFT 孔径）
  float lambdaM = 0.f;   // 载波波长 c/startFreq（角度换算 + 相位→位移都要用）

  // 由主字段计算全部派生字段。幂等。
  void derive();

  // 一致则返回 true；否则把原因写入 `err`。
  bool validate(std::string &err) const;
};

} // namespace radar
