// test_config.cpp — 采集入口配置单测（无硬件）。
//
// 覆盖：JSON 解析（嵌套/转义/Unicode/语法错误定位）、默认值 -> JSON -> 命令行
// 三级合并与优先级、未知键/类型不匹配/文件缺失的报错、跨字段校验。
// 临时配置文件写在当前工作目录（ctest 默认在 build/ 下运行）。

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "core/CaptureConfig.h"
#include "core/Json.h"

using radar::CaptureConfig;
using radar::CliOverrides;
using radar::JsonValue;

namespace {

int failures = 0;

void check(bool ok, const std::string &name) {
  if (!ok) {
    std::cerr << "FAIL " << name << '\n';
    ++failures;
  }
}

void checkEq(const std::string &got, const std::string &want,
             const std::string &name) {
  if (got != want) {
    std::cerr << "FAIL " << name << "\n  got:  \"" << got << "\"\n  want: \""
              << want << "\"\n";
    ++failures;
  }
}

void checkEqU(std::uint64_t got, std::uint64_t want, const std::string &name) {
  if (got != want) {
    std::cerr << "FAIL " << name << "\n  got:  " << got << "\n  want: " << want
              << '\n';
    ++failures;
  }
}

// 期望操作失败，并返回错误信息供进一步断言。
std::string expectError(bool ok, const std::string &err, const std::string &name) {
  check(!ok, name + ": expected failure");
  check(!err.empty(), name + ": expected an error message");
  return err;
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

// 以可变 argv 形式解释命令行，贴近 main() 的真实调用。
bool runCli(const std::vector<std::string> &args, CliOverrides &cli,
            std::string &err) {
  std::vector<char *> argv;
  for (const std::string &arg : args)
    argv.push_back(const_cast<char *>(arg.c_str()));
  return radar::parseCli(static_cast<int>(argv.size()), argv.data(), cli, err);
}

void testJsonParser() {
  JsonValue v;
  std::string err;
  check(radar::parseJson(
            R"({"a":1,"b":[true,null,"x\n"],"c":{"d":-1.5e2},"e":{}})", v, err),
        "parse a valid document");
  check(v.isObject(), "top level is an object");
  const JsonValue *a = v.find("a");
  check(a != nullptr && a->isNumber() && a->asNumber() == 1.0, "number field");
  const JsonValue *b = v.find("b");
  check(b != nullptr && b->isArray() && b->asArray().size() == 3, "array field");
  check(b->asArray()[0].isBool() && b->asArray()[0].asBool(), "bool element");
  check(b->asArray()[1].isNull(), "null element");
  checkEq(b->asArray()[2].asString(), "x\n", "escape sequence in string");
  const JsonValue *c = v.find("c");
  check(c != nullptr && c->find("d") != nullptr &&
            c->find("d")->asNumber() == -150.0,
        "nested object + exponent");
  check(v.find("e") != nullptr && v.find("e")->members().empty(),
        "empty object");
  check(v.find("missing") == nullptr, "missing key returns nullptr");
  check(a->find("a") == nullptr, "find on non-object returns nullptr");

  // \uXXXX（含代理对）应转成 UTF-8。
  JsonValue u;
  err.clear();
  check(radar::parseJson(R"({"k":"\u4e2d\u6587\ud83d\ude00"})", u, err),
        "parse unicode escapes");
  checkEq(u.find("k")->asString(), "\xe4\xb8\xad\xe6\x96\x87\xf0\x9f\x98\x80",
          "unicode escape to utf-8");

  // UTF-8 BOM 应被跳过。
  JsonValue bom;
  err.clear();
  check(radar::parseJson("\xEF\xBB\xBF{\"a\":1}", bom, err), "skip utf-8 BOM");

  JsonValue f;
  std::string e;
  check(!radar::parseJson("{", f, e) && contains(e, "line"),
        "unterminated object reports position");
  e.clear();
  check(!radar::parseJson(R"({"a":1,})", f, e), "trailing comma rejected");
  e.clear();
  check(!radar::parseJson(R"({"a":1} extra)", f, e),
        "trailing garbage rejected");
  e.clear();
  check(!radar::parseJson(R"({"a":1,"a":2})", f, e) && contains(e, "duplicate"),
        "duplicate key rejected");
  e.clear();
  check(!radar::parseJson(R"({"a":01})", f, e), "leading zero rejected");
  e.clear();
  check(!radar::parseJson(R"({"a":"\x"})", f, e), "invalid escape rejected");
  e.clear();
  check(!radar::parseJson(R"({"a":tru})", f, e), "invalid literal rejected");
  e.clear();
  check(!radar::parseJson("[1,2", f, e), "unterminated array rejected");
  e.clear();
  check(!radar::parseJson("", f, e), "empty document rejected");
}

void testDefaultsAndJsonMerge() {
  CaptureConfig cfg;
  checkEq(cfg.bindIp, "0.0.0.0", "default bindIp");
  checkEq(cfg.dcaIp, "192.168.33.180", "default dcaIp");
  check(cfg.dataPort == 4098 && cfg.configPort == 4096, "default ports");
  check(cfg.packetDelayUs == 25 && cfg.lvdsLanes == 4, "default packet/LVDS");
  checkEqU(cfg.frameBytes, 0, "default frameBytes");
  check(!cfg.noControl, "default noControl");

  JsonValue root;
  std::string err;
  check(radar::parseJson(
            R"({"capture":{"output":"captures/a.bin","maxFrames":1000,
                "noControl":true,"bindIp":"192.168.33.30"}})",
            root, err),
        "parse partial config");
  check(radar::applyCaptureJson(root, cfg, err), "apply partial config");
  checkEq(cfg.output, "captures/a.bin", "json sets output");
  checkEqU(cfg.maxFrames, 1000, "json sets maxFrames");
  checkEq(cfg.bindIp, "192.168.33.30", "json sets bindIp");
  check(cfg.noControl, "json sets noControl");
  // 未在 JSON 中出现的字段必须保持默认值（合并语义）。
  checkEq(cfg.dcaIp, "192.168.33.180", "unspecified field keeps default");
  check(cfg.dataPort == 4098, "unspecified port keeps default");
  checkEqU(cfg.frameBytes, 0, "unspecified frameBytes keeps default");
}

void testJsonErrors() {
  CaptureConfig cfg;
  std::string err;
  JsonValue root;

  check(radar::parseJson(R"({"capture":{"dataPort":"4098"}})", root, err),
        "parse string-for-int");
  std::string msg = expectError(radar::applyCaptureJson(root, cfg, err), err,
                               "string for integer");
  check(contains(msg, "capture.dataPort") && contains(msg, "string"),
        "type error names field and type");

  root = JsonValue();
  err.clear();
  check(radar::parseJson(R"({"capture":{"maxFrames":10.5}})", root, err),
        "parse fractional");
  msg = expectError(radar::applyCaptureJson(root, cfg, err), err,
                    "fractional for integer");
  check(contains(msg, "capture.maxFrames"), "fractional error names field");

  root = JsonValue();
  err.clear();
  check(radar::parseJson(R"({"capture":{"frameBytes":-1}})", root, err),
        "parse negative");
  msg = expectError(radar::applyCaptureJson(root, cfg, err), err,
                    "negative for unsigned");
  check(contains(msg, "negative"), "negative error explains");

  root = JsonValue();
  err.clear();
  check(radar::parseJson(R"({"capture":{"noControl":"yes"}})", root, err),
        "parse string-for-bool");
  msg = expectError(radar::applyCaptureJson(root, cfg, err), err,
                    "string for boolean");
  check(contains(msg, "capture.noControl"), "bool error names field");

  root = JsonValue();
  err.clear();
  check(radar::parseJson(R"({"capture":{"maxFrames":1000,"maxframe":1}})", root,
                         err),
        "parse unknown key");
  msg = expectError(radar::applyCaptureJson(root, cfg, err), err, "unknown key");
  check(contains(msg, "capture.maxframe"), "unknown key is reported");

  root = JsonValue();
  err.clear();
  check(radar::parseJson(R"({"maxFrames":1000})", root, err),
        "parse missing section");
  msg = expectError(radar::applyCaptureJson(root, cfg, err), err,
                    "missing capture section");
  check(contains(msg, "capture"), "missing section is reported");

  root = JsonValue();
  err.clear();
  check(radar::parseJson(R"({"capture":[]})", root, err),
        "parse wrong section type");
  msg = expectError(radar::applyCaptureJson(root, cfg, err), err,
                    "capture section must be object");
  check(contains(msg, "object"), "wrong section type is reported");

  root = JsonValue();
  err.clear();
  check(radar::parseJson("[1,2]", root, err), "parse top-level array");
  msg = expectError(radar::applyCaptureJson(root, cfg, err), err,
                    "top level must be object");
  check(contains(msg, "top-level"), "top-level type is reported");
}

void testFileLoading() {
  const std::string path = "radar_capture_cfg_test.json";
  CaptureConfig cfg;
  std::string err;
  check(writeFile(path, R"({"capture":{"output":"a.bin","maxFrames":7}})"),
        "write temp config");
  check(radar::loadCaptureConfigFile(path, cfg, err), "load config file");
  checkEq(cfg.output, "a.bin", "file sets output");
  checkEqU(cfg.maxFrames, 7, "file sets maxFrames");

  // 缺失文件
  cfg = CaptureConfig();
  err.clear();
  const std::string missing = "radar_capture_cfg_test_absent.json";
  std::remove(missing.c_str());
  std::string msg = expectError(
      radar::loadCaptureConfigFile(missing, cfg, err), err, "missing file");
  check(contains(msg, "cannot open"), "missing file message");

  // 格式错误（错误信息里应带文件名与行列）
  const std::string bad = "radar_capture_cfg_test_bad.json";
  check(writeFile(bad, "{\"capture\":{"), "write malformed config");
  cfg = CaptureConfig();
  err.clear();
  msg = expectError(radar::loadCaptureConfigFile(bad, cfg, err), err,
                    "malformed file");
  check(contains(msg, bad) && contains(msg, "line"), "malformed message has path+position");

  std::remove(path.c_str());
  std::remove(bad.c_str());
}

void testCliPrecedence() {
  const std::string path = "radar_capture_cfg_test_cli.json";
  check(writeFile(path,
                  R"({"capture":{"output":"a.bin","maxFrames":1000,"dataPort":5000,"noControl":false}})"),
        "write precedence config");

  CliOverrides cli;
  std::string err;
  check(runCli({"radar_capture", "--json", path, "--max-frames", "5",
                "--no-control", "--frame-bytes", "262144"},
               cli, err),
        "parse cli with json");
  checkEq(cli.jsonPath, path, "cli json path");

  // 默认值 -> JSON -> 命令行
  CaptureConfig cfg;
  check(radar::loadCaptureConfigFile(path, cfg, err), "load json layer");
  check(radar::applyCliOverrides(cli, cfg, err), "apply cli layer");
  check(radar::validateCaptureConfig(cfg, err), "merged config is valid");
  checkEqU(cfg.maxFrames, 5, "command line wins over json");
  check(cfg.dataPort == 5000, "json wins over default");
  checkEq(cfg.dcaIp, "192.168.33.180", "default kept when unset everywhere");
  checkEq(cfg.output, "a.bin", "json output kept when cli is silent");
  check(cfg.noControl, "bare --no-control sets the flag");
  checkEqU(cfg.frameBytes, 262144, "cli sets frameBytes");

  // 命令行错误
  CliOverrides bad;
  err.clear();
  check(!runCli({"radar_capture", "--bogus", "1"}, bad, err),
        "unknown option rejected");
  check(contains(err, "--bogus"), "unknown option is named");
  bad = CliOverrides();
  err.clear();
  check(!runCli({"radar_capture", "--max-frames"}, bad, err),
        "missing value rejected");
  check(contains(err, "--max-frames"), "missing value is named");
  bad = CliOverrides();
  err.clear();
  check(!runCli({"radar_capture", "--json"}, bad, err), "missing json path rejected");
  bad = CliOverrides();
  err.clear();
  check(runCli({"radar_capture", "--data-port", "abc"}, bad, err),
        "cli parse accepts raw text");
  check(!radar::applyCliOverrides(bad, cfg, err), "non-numeric cli value rejected");
  check(contains(err, "--data-port") && contains(err, "abc"),
        "bad cli value is reported with the flag and text");

  std::remove(path.c_str());
}

void testValidation() {
  CaptureConfig cfg;
  std::string err;
  std::string msg = expectError(radar::validateCaptureConfig(cfg, err), err,
                                "empty config");
  check(contains(msg, "output"), "output required message");

  cfg.output = "a.bin";
  err.clear();
  msg = expectError(radar::validateCaptureConfig(cfg, err), err,
                    "hardware mode without cfg/serial");
  check(contains(msg, "cfg") && contains(msg, "serial"), "hardware mode message");

  cfg.noControl = true;
  cfg.frameBytes = 1024;
  err.clear();
  check(radar::validateCaptureConfig(cfg, err), "passive mode with frameBytes");

  cfg.noControl = false;
  cfg.cfg = "x.cfg";
  err.clear();
  msg = expectError(radar::validateCaptureConfig(cfg, err), err,
                    "hardware mode without serial");
  check(contains(msg, "serial"), "serial still required");

  cfg.serial = "/dev/ttyACM0";
  cfg.dataPort = 0;
  err.clear();
  msg = expectError(radar::validateCaptureConfig(cfg, err), err, "dataPort range");
  check(contains(msg, "dataPort"), "dataPort message");

  cfg.dataPort = 4098;
  cfg.packetDelayUs = 4;
  err.clear();
  msg = expectError(radar::validateCaptureConfig(cfg, err), err, "packet delay range");
  check(contains(msg, "packetDelayUs"), "packetDelayUs message");

  cfg.packetDelayUs = 25;
  cfg.lvdsLanes = 3;
  err.clear();
  msg = expectError(radar::validateCaptureConfig(cfg, err), err, "lvds lanes");
  check(contains(msg, "lvdsLanes"), "lvdsLanes message");

  cfg.lvdsLanes = 2;
  cfg.rcvbuf = 1024;
  err.clear();
  msg = expectError(radar::validateCaptureConfig(cfg, err), err, "rcvbuf floor");
  check(contains(msg, "rcvbuf"), "rcvbuf message");

  cfg.rcvbuf = 16777216;
  cfg.configPort = 70000;
  err.clear();
  msg = expectError(radar::validateCaptureConfig(cfg, err), err, "configPort range");
  check(contains(msg, "configPort"), "configPort message");

  cfg.configPort = 4096;
  err.clear();
  check(radar::validateCaptureConfig(cfg, err), "fully specified config is valid");
}

} // namespace

int main() {
  testJsonParser();
  testDefaultsAndJsonMerge();
  testJsonErrors();
  testFileLoading();
  testCliPrecedence();
  testValidation();
  std::cout << (failures ? "config tests failed\n" : "config tests passed\n");
  return failures ? 1 : 0;
}
