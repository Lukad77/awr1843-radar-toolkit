// test_transport.cpp — 传输层无硬件单测。
//
// 覆盖：.cfg -> RadarConfig/命令序列 的解析与拒绝规则（MmwaveCfg）、
// 原始帧落盘 sink 的产物与统计（RawFileSink）。
// 临时文件写在当前工作目录（ctest 默认在 build/ 下运行）。

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "core/FrameContext.h"
#include "transport/MmwaveCfg.h"
#include "transport/RawFileSink.h"

using radar::FrameContext;
using radar::MmwaveCfg;
using radar::RawFileSink;

namespace {

int failures = 0;

void check(bool ok, const std::string &name) {
  if (!ok) {
    std::cerr << "FAIL " << name << '\n';
    ++failures;
  }
}

void checkEq(long long got, long long want, const std::string &name) {
  if (got != want) {
    std::cerr << "FAIL " << name << "\n  got:  " << got << "\n  want: " << want
              << '\n';
    ++failures;
  }
}

void checkNear(double got, double want, double tol, const std::string &name) {
  if (!(got > want - tol && got < want + tol)) {
    std::cerr << "FAIL " << name << "\n  got:  " << got << "\n  want: " << want
              << '\n';
    ++failures;
  }
}

bool contains(const std::string &haystack, const std::string &needle) {
  return haystack.find(needle) != std::string::npos;
}

bool writeFile(const std::string &path, const std::string &text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out << text;
  return static_cast<bool>(out);
}

std::string readFile(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

// 与 awr1843.cfg 同构的最小可用配置：单 TX、单 chirp、16 位复数、LVDS 无头。
const char *kGoodCfg =
    "% comment line\n"
    "sensorStop\n"
    "flushCfg\n"
    "dfeDataOutputMode 1\n"
    "channelCfg 15 1 0\n"
    "adcCfg 2 1\n"
    "adcbufCfg -1 0 1 1 1\n"
    "profileCfg 0 77 10 7 60 0 0 57.984 1 256 10000 0 0 30\n"
    "chirpCfg 0 0 0 0 0 0 0 1\n"
    "frameCfg 0 0 64 0 10 1 0\n"
    "guiMonitor -1 0 0 0 0 0 0\n"
    "lvdsStreamCfg -1 0 1 0\n"
    "sensorStart\n";

void testMmwaveCfg() {
  const std::string path = "radar_mmcfg_test.cfg";
  check(writeFile(path, kGoodCfg), "write good cfg");

  MmwaveCfg cfg;
  std::string err;
  check(radar::loadMmwaveCfg(path, cfg, err), "parse good cfg: " + err);
  checkEq(cfg.radar.numRxAnt, 4, "numRxAnt from channelCfg");
  checkEq(cfg.radar.numTxAnt, 1, "numTxAnt from chirpCfg txEnable");
  checkEq(cfg.radar.numAdcSamples, 256, "numAdcSamples from profileCfg");
  checkEq(cfg.radar.numLoops, 64, "numLoops from frameCfg");
  checkEq(cfg.radar.numChirpsPerFrame, 64, "derived numChirpsPerFrame");
  checkEq(cfg.radar.numRangeBins, 256, "derived numRangeBins");
  checkEq(cfg.radar.numDopplerBins, 64, "derived numDopplerBins");
  checkEq(cfg.radar.bytesPerFrame, 262144, "derived bytesPerFrame");
  checkEq(cfg.radar.numFrames, 0, "numFrames from frameCfg");
  checkEq(static_cast<long long>(cfg.radar.framePeriodicityMs), 10, "frame periodicity");
  checkEq(static_cast<long long>(cfg.radar.startFreqGHz), 77, "start frequency");
  // float 存储 57.984 的相对误差约 2e-8，容差取 1e-3 足够且不失判别力。
  checkNear(cfg.radar.freqSlopeMHzPerUs, 57.984, 1e-3, "freq slope");
  checkEq(cfg.radar.digOutSampleRateKsps, 10000, "dig out sample rate");
  check(!cfg.radar.isReal, "complex samples");
  // 分辨率派生值应当算出来（非 0），显示坐标轴依赖它
  check(cfg.radar.rangeIdxToMeters > 0.f, "rangeIdxToMeters derived");
  check(cfg.radar.lambdaM > 0.f, "lambdaM derived");

  // 命令序列：剔除注释与 sensorStart/sensorStop，其余按原序保留
  checkEq(static_cast<long long>(cfg.commands.size()), 10, "command count");
  check(cfg.commands[0] == "flushCfg", "first command is flushCfg");
  check(cfg.commands[1] == "dfeDataOutputMode 1", "second command");
  for (const std::string &c : cfg.commands) {
    check(!contains(c, "sensorStart"), "sensorStart not in commands");
    check(!contains(c, "sensorStop"), "sensorStop not in commands");
    check(!contains(c, "%"), "comments stripped");
  }

  // ---- 拒绝规则（都必须明确报错，不能按错参数静默解析）----
  struct Case {
    const char *name;
    const char *text;
  };
  const Case cases[] = {
      {"real ADC rejected", "channelCfg 15 1 0\nadcCfg 1 1\n"
                            "profileCfg 0 77 10 7 60 0 0 57.984 1 256 10000 0 0 30\n"
                            "chirpCfg 0 0 0 0 0 0 0 1\nframeCfg 0 0 64 0 10 1 0\n"
                            "lvdsStreamCfg -1 0 1 0\n"},
      {"LVDS header rejected", "channelCfg 15 1 0\nadcCfg 2 1\n"
                               "profileCfg 0 77 10 7 60 0 0 57.984 1 256 10000 0 0 30\n"
                               "chirpCfg 0 0 0 0 0 0 0 1\nframeCfg 0 0 64 0 10 1 0\n"
                               "lvdsStreamCfg -1 1 1 0\n"},
      {"two TX chirp rejected", "channelCfg 15 1 0\nadcCfg 2 1\n"
                                "profileCfg 0 77 10 7 60 0 0 57.984 1 256 10000 0 0 30\n"
                                "chirpCfg 0 0 0 0 0 0 0 3\nframeCfg 0 0 64 0 10 1 0\n"
                                "lvdsStreamCfg -1 0 1 0\n"},
      {"chirp span rejected", "channelCfg 15 1 0\nadcCfg 2 1\n"
                              "profileCfg 0 77 10 7 60 0 0 57.984 1 256 10000 0 0 30\n"
                              "chirpCfg 0 0 0 0 0 0 0 1\nframeCfg 0 1 64 0 10 1 0\n"
                              "lvdsStreamCfg -1 0 1 0\n"},
      {"zero frame period rejected", "channelCfg 15 1 0\nadcCfg 2 1\n"
                                     "profileCfg 0 77 10 7 60 0 0 57.984 1 256 10000 0 0 30\n"
                                     "chirpCfg 0 0 0 0 0 0 0 1\nframeCfg 0 0 64 0 0 1 0\n"
                                     "lvdsStreamCfg -1 0 1 0\n"},
      {"empty cfg rejected", "\n"},
  };
  for (const Case &c : cases) {
    const std::string p = std::string("radar_mmcfg_bad.cfg");
    check(writeFile(p, c.text), std::string("write cfg: ") + c.name);
    MmwaveCfg bad;
    std::string badErr;
    check(!radar::loadMmwaveCfg(p, bad, badErr), std::string("reject: ") + c.name);
    check(!badErr.empty(), std::string("error message: ") + c.name);
    std::remove(p.c_str());
  }

  MmwaveCfg missing;
  err.clear();
  check(!radar::loadMmwaveCfg("radar_mmcfg_absent.cfg", missing, err),
        "missing cfg rejected");
  check(contains(err, "cannot open"), "missing cfg message");

  std::remove(path.c_str());
}

void testRawFileSink() {
  const std::string out = "radar_rawsink_test.bin";
  std::remove(out.c_str());
  std::remove((out + ".frames.csv").c_str());
  std::remove((out + ".stats.txt").c_str());

  RawFileSink sink(out, /*frameBytes=*/8);
  check(sink.ok(), "sink opens output files: " + sink.lastError());

  auto frame = [](std::uint8_t fill) {
    return std::make_shared<std::vector<std::uint8_t>>(
        std::vector<std::uint8_t>(8, fill));
  };
  FrameContext a;
  a.frameSeq = 0;
  a.wireSeqStart = 7;
  a.raw = frame(0xAA);
  sink.consume(a);

  FrameContext b;
  b.frameSeq = 1;
  b.wireSeqStart = 9;
  b.raw = frame(0xBB);
  sink.consume(b);

  // 应被计为跳过：无效帧 / 尺寸不符
  FrameContext bad;
  bad.frameSeq = 2;
  bad.valid = false;
  bad.raw = frame(0xCC);
  sink.consume(bad);
  FrameContext shortFrame;
  shortFrame.frameSeq = 3;
  shortFrame.raw = std::make_shared<std::vector<std::uint8_t>>(4, 0xDD);
  sink.consume(shortFrame);

  sink.flush();

  checkEq(static_cast<long long>(sink.savedFrames()), 2, "savedFrames");
  checkEq(static_cast<long long>(sink.skippedFrames()), 1, "skippedFrames");
  checkEq(static_cast<long long>(sink.shortFrames()), 1, "shortFrames");

  const std::string data = readFile(out);
  checkEq(static_cast<long long>(data.size()), 16, "output byte count");
  check(data == std::string(8, '\xAA') + std::string(8, '\xBB'), "output bytes in order");

  const std::string index = readFile(out + ".frames.csv");
  check(contains(index, "file_frame,wire_frame"), "index header");
  check(contains(index, "0,7"), "index row 0");
  check(contains(index, "1,9"), "index row 1");

  const std::string stats = readFile(out + ".stats.txt");
  check(contains(stats, "savedFrames=2"), "stats savedFrames");
  check(contains(stats, "frameBytes=8"), "stats frameBytes");

  std::remove(out.c_str());
  std::remove((out + ".frames.csv").c_str());
  std::remove((out + ".stats.txt").c_str());
}

} // namespace

int main() {
  testMmwaveCfg();
  testRawFileSink();
  std::cout << (failures ? "transport tests failed\n" : "transport tests passed\n");
  return failures ? 1 : 0;
}
