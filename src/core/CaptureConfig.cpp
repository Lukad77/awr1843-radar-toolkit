#include "core/CaptureConfig.h"

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <string>

namespace radar {
namespace {

// 规范字段名：JSON 键与命令行选项共用同一套名字，避免两个前端漂移。
const char *const kKeys[] = {"output",       "cfg",          "serial",
                             "bindIp",       "dcaIp",        "dataPort",
                             "configPort",   "packetDelayUs", "lvdsLanes",
                             "rcvbuf",       "frameBytes",   "maxFrames",
                             "noControl"};

struct FlagSpec {
  const char *flag;
  const char *key;
  bool takesValue;
};

const FlagSpec kFlags[] = {
    {"--output", "output", true},
    {"--cfg", "cfg", true},
    {"--serial", "serial", true},
    {"--bind-ip", "bindIp", true},
    {"--dca-ip", "dcaIp", true},
    {"--data-port", "dataPort", true},
    {"--config-port", "configPort", true},
    {"--packet-delay-us", "packetDelayUs", true},
    {"--lvds-lanes", "lvdsLanes", true},
    {"--rcvbuf", "rcvbuf", true},
    {"--frame-bytes", "frameBytes", true},
    {"--max-frames", "maxFrames", true},
    {"--no-control", "noControl", false},
};

bool isKnownKey(const std::string &key) {
  for (const char *candidate : kKeys) {
    if (key == candidate) return true;
  }
  return false;
}

std::string flagForKey(const std::string &key) {
  for (const FlagSpec &spec : kFlags) {
    if (key == spec.key) return spec.flag;
  }
  return key;
}

// ---- JSON 取值（带类型校验与字段名）----

bool wantString(const JsonValue &v, const std::string &where, std::string &out,
                std::string &err) {
  if (!v.isString()) {
    err = where + ": expected a string, got " + v.typeName();
    return false;
  }
  out = v.asString();
  return true;
}

bool wantInt(const JsonValue &v, const std::string &where, int &out,
             std::string &err) {
  if (!v.isNumber()) {
    err = where + ": expected an integer, got " + v.typeName();
    return false;
  }
  const double d = v.asNumber();
  if (std::floor(d) != d) {
    err = where + ": expected an integer, got a fractional number";
    return false;
  }
  if (d < static_cast<double>(INT_MIN) || d > static_cast<double>(INT_MAX)) {
    err = where + ": integer out of range";
    return false;
  }
  out = static_cast<int>(d);
  return true;
}

bool wantU64(const JsonValue &v, const std::string &where, std::uint64_t &out,
             std::string &err) {
  if (!v.isNumber()) {
    err = where + ": expected a non-negative integer, got " + v.typeName();
    return false;
  }
  const double d = v.asNumber();
  if (std::floor(d) != d) {
    err = where + ": expected an integer, got a fractional number";
    return false;
  }
  if (d < 0) {
    err = where + ": must not be negative";
    return false;
  }
  // 2^53 是可精确表示的上界，超过即认为写错了。
  if (d > 9007199254740992.0) {
    err = where + ": value is too large";
    return false;
  }
  out = static_cast<std::uint64_t>(d);
  return true;
}

bool wantBool(const JsonValue &v, const std::string &where, bool &out,
              std::string &err) {
  if (!v.isBool()) {
    err = where + ": expected true or false, got " + v.typeName();
    return false;
  }
  out = v.asBool();
  return true;
}

// ---- 命令行取值（原始文本 -> 类型）----

bool cliInt(const std::string &flag, const std::string &text, int &out,
            std::string &err) {
  errno = 0;
  char *end = nullptr;
  const long value = std::strtol(text.c_str(), &end, 10);
  if (errno != 0 || end == text.c_str() || *end != '\0' || value < INT_MIN ||
      value > INT_MAX) {
    err = flag + ": \"" + text + "\" is not a valid integer";
    return false;
  }
  out = static_cast<int>(value);
  return true;
}

bool cliU64(const std::string &flag, const std::string &text,
            std::uint64_t &out, std::string &err) {
  if (text.empty() || text[0] == '-') {
    err = flag + ": \"" + text + "\" is not a valid non-negative integer";
    return false;
  }
  errno = 0;
  char *end = nullptr;
  const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
  if (errno != 0 || end == text.c_str() || *end != '\0') {
    err = flag + ": \"" + text + "\" is not a valid non-negative integer";
    return false;
  }
  out = static_cast<std::uint64_t>(value);
  return true;
}

bool cliBool(const std::string &flag, const std::string &text, bool &out,
             std::string &err) {
  if (text == "true" || text == "1") {
    out = true;
    return true;
  }
  if (text == "false" || text == "0") {
    out = false;
    return true;
  }
  err = flag + ": \"" + text + "\" is not a boolean";
  return false;
}

} // namespace

bool applyCaptureJson(const JsonValue &root, CaptureConfig &cfg,
                      std::string &err) {
  if (!root.isObject()) {
    err = "the top-level JSON value must be an object";
    return false;
  }
  const JsonValue *section = root.find("capture");
  if (section == nullptr) {
    err = "missing required object \"capture\" "
          "(expected {\"capture\": { ... }})";
    return false;
  }
  if (!section->isObject()) {
    err = "\"capture\" must be an object, got " +
          std::string(section->typeName());
    return false;
  }

  for (const auto &member : section->members()) {
    const std::string &key = member.first;
    const JsonValue &value = member.second;
    const std::string where = "capture." + key;
    if (!isKnownKey(key)) {
      err = "unknown key \"" + where + "\"";
      return false;
    }
    bool ok = true;
    if (key == "output") ok = wantString(value, where, cfg.output, err);
    else if (key == "cfg") ok = wantString(value, where, cfg.cfg, err);
    else if (key == "serial") ok = wantString(value, where, cfg.serial, err);
    else if (key == "bindIp") ok = wantString(value, where, cfg.bindIp, err);
    else if (key == "dcaIp") ok = wantString(value, where, cfg.dcaIp, err);
    else if (key == "dataPort") ok = wantInt(value, where, cfg.dataPort, err);
    else if (key == "configPort") ok = wantInt(value, where, cfg.configPort, err);
    else if (key == "packetDelayUs") ok = wantInt(value, where, cfg.packetDelayUs, err);
    else if (key == "lvdsLanes") ok = wantInt(value, where, cfg.lvdsLanes, err);
    else if (key == "rcvbuf") ok = wantInt(value, where, cfg.rcvbuf, err);
    else if (key == "frameBytes") ok = wantU64(value, where, cfg.frameBytes, err);
    else if (key == "maxFrames") ok = wantU64(value, where, cfg.maxFrames, err);
    else if (key == "noControl") ok = wantBool(value, where, cfg.noControl, err);
    if (!ok) return false;
  }
  return true;
}

bool loadCaptureConfigFile(const std::string &path, CaptureConfig &cfg,
                           std::string &err) {
  JsonValue root;
  if (!parseJsonFile(path, root, err)) return false;
  return applyCaptureJson(root, cfg, err);
}

bool parseCli(int argc, char **argv, CliOverrides &out, std::string &err) {
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    if (flag == "--json") {
      if (i + 1 >= argc) {
        err = "missing value for --json";
        return false;
      }
      out.jsonPath = argv[++i];
      continue;
    }
    const FlagSpec *spec = nullptr;
    for (const FlagSpec &candidate : kFlags) {
      if (flag == candidate.flag) {
        spec = &candidate;
        break;
      }
    }
    if (spec == nullptr) {
      err = "unknown option " + flag;
      return false;
    }
    if (!spec->takesValue) {
      out.values[spec->key] = "true";
      continue;
    }
    if (i + 1 >= argc) {
      err = "missing value for " + flag;
      return false;
    }
    out.values[spec->key] = argv[++i];
  }
  return true;
}

bool applyCliOverrides(const CliOverrides &cli, CaptureConfig &cfg,
                       std::string &err) {
  for (const auto &entry : cli.values) {
    const std::string &key = entry.first;
    const std::string &text = entry.second;
    const std::string flag = flagForKey(key);
    if (key == "output") cfg.output = text;
    else if (key == "cfg") cfg.cfg = text;
    else if (key == "serial") cfg.serial = text;
    else if (key == "bindIp") cfg.bindIp = text;
    else if (key == "dcaIp") cfg.dcaIp = text;
    else if (key == "dataPort") { if (!cliInt(flag, text, cfg.dataPort, err)) return false; }
    else if (key == "configPort") { if (!cliInt(flag, text, cfg.configPort, err)) return false; }
    else if (key == "packetDelayUs") { if (!cliInt(flag, text, cfg.packetDelayUs, err)) return false; }
    else if (key == "lvdsLanes") { if (!cliInt(flag, text, cfg.lvdsLanes, err)) return false; }
    else if (key == "rcvbuf") { if (!cliInt(flag, text, cfg.rcvbuf, err)) return false; }
    else if (key == "frameBytes") { if (!cliU64(flag, text, cfg.frameBytes, err)) return false; }
    else if (key == "maxFrames") { if (!cliU64(flag, text, cfg.maxFrames, err)) return false; }
    else if (key == "noControl") { if (!cliBool(flag, text, cfg.noControl, err)) return false; }
    else {
      err = "internal error: unhandled option key \"" + key + "\"";
      return false;
    }
  }
  return true;
}

bool validateCaptureConfig(const CaptureConfig &cfg, std::string &err) {
  if (cfg.output.empty()) {
    err = "output is required (JSON capture.output or --output)";
    return false;
  }
  if (!cfg.noControl && (cfg.cfg.empty() || cfg.serial.empty())) {
    err = "hardware mode requires cfg and serial "
          "(JSON capture.cfg / capture.serial, or --cfg / --serial); "
          "use noControl for passive capture";
    return false;
  }
  if (cfg.noControl && cfg.frameBytes == 0 && cfg.cfg.empty()) {
    err = "passive mode requires frameBytes or cfg "
          "(JSON capture.frameBytes / capture.cfg, or --frame-bytes / --cfg)";
    return false;
  }
  if (cfg.dataPort < 1 || cfg.dataPort > 65535) {
    err = "dataPort must be in 1..65535, got " + std::to_string(cfg.dataPort);
    return false;
  }
  if (cfg.configPort < 1 || cfg.configPort > 65535) {
    err = "configPort must be in 1..65535, got " + std::to_string(cfg.configPort);
    return false;
  }
  if (cfg.packetDelayUs < 5 || cfg.packetDelayUs > 500) {
    err = "packetDelayUs must be in 5..500, got " +
          std::to_string(cfg.packetDelayUs);
    return false;
  }
  if (cfg.lvdsLanes != 2 && cfg.lvdsLanes != 4) {
    err = "lvdsLanes must be 2 or 4, got " + std::to_string(cfg.lvdsLanes);
    return false;
  }
  if (cfg.rcvbuf < 65536) {
    err = "rcvbuf must be at least 65536 bytes, got " + std::to_string(cfg.rcvbuf);
    return false;
  }
  return true;
}

} // namespace radar
