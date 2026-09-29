#include "transport/MmwaveCfg.h"

#include <fstream>
#include <sstream>
#include <string>
#include <utility>

namespace radar {
namespace {

int popcount(unsigned v) {
  int n = 0;
  while (v) {
    v &= v - 1u;
    ++n;
  }
  return n;
}

int toInt(const std::vector<std::string> &t, std::size_t i) {
  return static_cast<int>(std::stod(t[i]));
}

float toFloat(const std::vector<std::string> &t, std::size_t i) {
  return static_cast<float>(std::stod(t[i]));
}

std::vector<std::string> tokenize(const std::string &line) {
  std::istringstream ss(line);
  std::vector<std::string> t;
  for (std::string w; ss >> w;) t.push_back(w);
  return t;
}

} // namespace

bool loadMmwaveCfg(const std::string &path, MmwaveCfg &out, std::string &err) {
  std::ifstream in(path);
  if (!in) {
    err = "cannot open radar cfg: " + path;
    return false;
  }

  // 待填的 .cfg 主字段（-1 表示「未出现」）。
  int rxMask = -1, adcBits = -1, adcFmt = -1, lvdsHeader = -1, lvdsFmt = -1;
  int samples = -1, digOutRate = -1;
  int chirpStart = -1, chirpEnd = -1, loops = -1, numFrames = -1;
  float framePeriodicity = -1.f, triggerDelay = -1.f;
  int triggerSelect = -1;
  float startFreq = -1.f, idleTime = -1.f, rampEnd = -1.f, freqSlope = -1.f;
  std::vector<std::pair<int, int>> chirpTx; // {chirpStartIdx, txEnable mask}

  try {
    std::string line;
    while (std::getline(in, line)) {
      const auto cut = line.find_first_of("%#");
      if (cut != std::string::npos) line.resize(cut);
      const std::vector<std::string> t = tokenize(line);
      if (t.empty() || t[0] == "sensorStart" || t[0] == "sensorStop") continue;

      if (t[0] == "channelCfg" && t.size() >= 3) rxMask = toInt(t, 1);
      if (t[0] == "adcCfg" && t.size() >= 3) {
        adcBits = toInt(t, 1);
        adcFmt = toInt(t, 2);
      }
      // profileCfg: id startFreq idle adcStart rampEnd txPow txPhase
      //             freqSlope txStart numAdcSamples digOutRate hpf1 hpf2 gain
      if (t[0] == "profileCfg" && t.size() >= 12) {
        startFreq = toFloat(t, 2);
        idleTime = toFloat(t, 3);
        rampEnd = toFloat(t, 5);
        freqSlope = toFloat(t, 8);
        samples = toInt(t, 10);
        digOutRate = toInt(t, 11);
      }
      // frameCfg: chirpStart chirpEnd numLoops numFrames periodicity trigger delay
      if (t[0] == "frameCfg" && t.size() >= 8) {
        chirpStart = toInt(t, 1);
        chirpEnd = toInt(t, 2);
        loops = toInt(t, 3);
        numFrames = toInt(t, 4);
        framePeriodicity = toFloat(t, 5);
        triggerSelect = toInt(t, 6);
        triggerDelay = toFloat(t, 7);
      }
      // chirpCfg: startIdx endIdx profileId freqVar slopeVar idleVar adcStartVar txEnable
      if (t[0] == "chirpCfg" && t.size() >= 9)
        chirpTx.emplace_back(toInt(t, 1), toInt(t, 8));
      if (t[0] == "lvdsStreamCfg" && t.size() >= 4) {
        lvdsHeader = toInt(t, 2);
        lvdsFmt = toInt(t, 3);
      }
      out.commands.push_back(line);
    }
  } catch (const std::exception &e) {
    err = path + ": cannot parse numeric field (" + e.what() + ")";
    return false;
  }

  // 每个 chirp 的 txEnable 必须是单个 bit（=> 单 TX）。
  bool singleTxChirp = !chirpTx.empty();
  int txMask = 0;
  for (const auto &c : chirpTx) {
    if (c.second <= 0 || (c.second & (c.second - 1)) != 0) singleTxChirp = false;
    if (c.first == chirpStart) txMask = c.second;
  }
  if (txMask == 0) singleTxChirp = false;

  if (rxMask <= 0 || samples <= 0 || digOutRate <= 0 || chirpStart < 0 ||
      chirpEnd != chirpStart || loops <= 0 || adcBits != 2 || adcFmt != 1 ||
      lvdsHeader != 0 || lvdsFmt != 1 || !singleTxChirp || startFreq <= 0.f ||
      freqSlope == 0.f || framePeriodicity <= 0.f) {
    err = "cfg requires one TX chirp, complex 16-bit ADC, LVDS raw ADC without "
          "headers and a frame period > 0";
    return false;
  }

  out.radar = RadarConfig();
  out.radar.numRxAnt = popcount(static_cast<unsigned>(rxMask));
  out.radar.numTxAnt = popcount(static_cast<unsigned>(txMask));
  out.radar.isReal = false; // adcCfg 2 1 => 16 位复数
  out.radar.numAdcBits = 16;
  out.radar.numAdcSamples = samples;
  out.radar.rxIdx = 0;
  out.radar.numAngleBins = 64; // .cfg 不含该项，用默认零填充点数
  out.radar.chirpStartIdx = chirpStart;
  out.radar.chirpEndIdx = chirpEnd;
  out.radar.numLoops = loops;
  out.radar.numFrames = numFrames;
  out.radar.framePeriodicityMs = framePeriodicity;
  out.radar.triggerSelect = triggerSelect;
  out.radar.triggerDelay = triggerDelay;
  out.radar.startFreqGHz = startFreq;
  out.radar.idleTimeUs = idleTime;
  out.radar.rampEndTimeUs = rampEnd;
  out.radar.freqSlopeMHzPerUs = freqSlope;
  out.radar.digOutSampleRateKsps = digOutRate;
  out.radar.derive();

  std::string verr;
  if (!out.radar.validate(verr)) {
    err = path + ": derived radar config is invalid: " + verr;
    return false;
  }
  return true;
}

} // namespace radar
