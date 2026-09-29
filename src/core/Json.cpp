#include "core/Json.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace radar {

const JsonValue *JsonValue::find(const std::string &key) const {
  if (type_ != Type::Object) return nullptr;
  const auto it = object_.find(key);
  return it == object_.end() ? nullptr : &it->second;
}

const char *JsonValue::typeName() const {
  switch (type_) {
    case Type::Null: return "null";
    case Type::Bool: return "boolean";
    case Type::Number: return "number";
    case Type::String: return "string";
    case Type::Array: return "array";
    case Type::Object: return "object";
  }
  return "unknown";
}

JsonValue JsonValue::makeArray() {
  JsonValue v;
  v.type_ = Type::Array;
  return v;
}

JsonValue JsonValue::makeObject() {
  JsonValue v;
  v.type_ = Type::Object;
  return v;
}

bool JsonValue::append(JsonValue v) {
  if (type_ != Type::Array) return false;
  array_.push_back(std::move(v));
  return true;
}

bool JsonValue::put(std::string key, JsonValue v) {
  if (type_ != Type::Object) return false;
  return object_.emplace(std::move(key), std::move(v)).second; // false on duplicate
}

namespace {

// 防御恶意/误生成的深层嵌套，避免递归下降把栈打爆。
constexpr int kMaxDepth = 64;

class Parser {
public:
  explicit Parser(const std::string &text) : text_(text) {}

  bool run(JsonValue &out, std::string &err) {
    skipBom();
    skipWs();
    if (!parseValue(out, 0)) {
      err = err_;
      return false;
    }
    skipWs();
    if (!eof()) {
      fail("unexpected trailing characters after the top-level value");
      err = err_;
      return false;
    }
    return true;
  }

private:
  const std::string &text_;
  std::size_t pos_ = 0;
  std::string err_;

  bool eof() const { return pos_ >= text_.size(); }
  char peek() const { return text_[pos_]; }

  // 只保留最先（最深）的错误信息，避免外层用泛化消息覆盖精确原因。
  void fail(const std::string &msg) {
    if (!err_.empty()) return;
    std::size_t line = 1, column = 1;
    for (std::size_t i = 0; i < pos_ && i < text_.size(); ++i) {
      if (text_[i] == '\n') {
        ++line;
        column = 1;
      } else {
        ++column;
      }
    }
    err_ = msg + " (line " + std::to_string(line) + ", column " +
           std::to_string(column) + ")";
  }

  // 允许 Windows 编辑器写入的 UTF-8 BOM，避免它被当成非法字符。
  void skipBom() {
    if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xEF &&
        static_cast<unsigned char>(text_[1]) == 0xBB &&
        static_cast<unsigned char>(text_[2]) == 0xBF) {
      pos_ = 3;
    }
  }

  void skipWs() {
    while (!eof()) {
      const char c = peek();
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
        continue;
      }
      break;
    }
  }

  bool parseValue(JsonValue &out, int depth) {
    if (depth > kMaxDepth) {
      fail("nesting is too deep");
      return false;
    }
    if (eof()) {
      fail("unexpected end of input");
      return false;
    }
    switch (peek()) {
      case '{': return parseObject(out, depth);
      case '[': return parseArray(out, depth);
      case '"': {
        std::string s;
        if (!parseString(s)) return false;
        out = JsonValue(std::move(s));
        return true;
      }
      case 't': return parseLiteral("true", JsonValue(true), out);
      case 'f': return parseLiteral("false", JsonValue(false), out);
      case 'n': return parseLiteral("null", JsonValue(), out);
      default:
        if (peek() == '-' || (peek() >= '0' && peek() <= '9')) return parseNumber(out);
        fail(std::string("unexpected character '") + peek() + "'");
        return false;
    }
  }

  bool parseLiteral(const char *literal, const JsonValue &value, JsonValue &out) {
    const std::size_t n = std::strlen(literal);
    if (text_.compare(pos_, n, literal) != 0) {
      fail(std::string("invalid literal, expected \"") + literal + "\"");
      return false;
    }
    pos_ += n;
    out = value;
    return true;
  }

  bool parseNumber(JsonValue &out) {
    const std::size_t start = pos_;
    if (!eof() && peek() == '-') ++pos_;
    if (eof() || peek() < '0' || peek() > '9') {
      fail("invalid number: expected a digit");
      return false;
    }
    if (peek() == '0') {
      ++pos_; // 前导零只允许单独的 0
    } else {
      while (!eof() && peek() >= '0' && peek() <= '9') ++pos_;
    }
    if (!eof() && peek() == '.') {
      ++pos_;
      if (eof() || peek() < '0' || peek() > '9') {
        fail("invalid number: expected a digit after '.'");
        return false;
      }
      while (!eof() && peek() >= '0' && peek() <= '9') ++pos_;
    }
    if (!eof() && (peek() == 'e' || peek() == 'E')) {
      ++pos_;
      if (!eof() && (peek() == '+' || peek() == '-')) ++pos_;
      if (eof() || peek() < '0' || peek() > '9') {
        fail("invalid number: expected a digit in the exponent");
        return false;
      }
      while (!eof() && peek() >= '0' && peek() <= '9') ++pos_;
    }
    const std::string token = text_.substr(start, pos_ - start);
    out = JsonValue(std::strtod(token.c_str(), nullptr));
    return true;
  }

  static void appendUtf8(std::string &out, std::uint32_t cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }

  bool parseHex4(std::uint32_t &out) {
    if (pos_ + 4 > text_.size()) {
      fail("truncated \\u escape sequence");
      return false;
    }
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[pos_ + i];
      v <<= 4;
      if (c >= '0' && c <= '9') {
        v |= static_cast<std::uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        v |= static_cast<std::uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        v |= static_cast<std::uint32_t>(c - 'A' + 10);
      } else {
        fail("invalid hex digit in \\u escape sequence");
        return false;
      }
    }
    pos_ += 4;
    out = v;
    return true;
  }

  bool parseString(std::string &out) {
    ++pos_; // 开头引号
    out.clear();
    for (;;) {
      if (eof()) {
        fail("unterminated string");
        return false;
      }
      const unsigned char c = static_cast<unsigned char>(peek());
      if (c == '"') {
        ++pos_;
        return true;
      }
      if (c == '\\') {
        ++pos_;
        if (eof()) {
          fail("unterminated escape sequence");
          return false;
        }
        const char e = peek();
        ++pos_;
        switch (e) {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          case 'u': {
            std::uint32_t cp = 0;
            if (!parseHex4(cp)) return false;
            if (cp >= 0xD800 && cp <= 0xDBFF) { // 高代理：必须跟低位代理
              if (pos_ + 1 < text_.size() && text_[pos_] == '\\' &&
                  text_[pos_ + 1] == 'u') {
                pos_ += 2;
                std::uint32_t low = 0;
                if (!parseHex4(low)) return false;
                if (low < 0xDC00 || low > 0xDFFF) {
                  fail("invalid low surrogate in \\u escape sequence");
                  return false;
                }
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
              } else {
                fail("unpaired high surrogate in \\u escape sequence");
                return false;
              }
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
              fail("unexpected low surrogate in \\u escape sequence");
              return false;
            }
            appendUtf8(out, cp);
            break;
          }
          default:
            fail(std::string("invalid escape sequence \"\\") + e + "\"");
            return false;
        }
        continue;
      }
      if (c < 0x20) {
        fail("control character in string must be escaped");
        return false;
      }
      out += static_cast<char>(c);
      ++pos_;
    }
  }

  bool parseObject(JsonValue &out, int depth) {
    ++pos_; // '{'
    JsonValue obj = JsonValue::makeObject();
    skipWs();
    if (!eof() && peek() == '}') {
      ++pos_;
      out = std::move(obj);
      return true;
    }
    for (;;) {
      skipWs();
      if (eof() || peek() != '"') {
        fail("expected a quoted string key");
        return false;
      }
      std::string key;
      if (!parseString(key)) return false;
      skipWs();
      if (eof() || peek() != ':') {
        fail("expected ':' after key \"" + key + "\"");
        return false;
      }
      ++pos_;
      skipWs();
      JsonValue value;
      if (!parseValue(value, depth + 1)) return false;
      if (!obj.put(key, std::move(value))) {
        fail("duplicate key \"" + key + "\"");
        return false;
      }
      skipWs();
      if (eof()) {
        fail("unterminated object");
        return false;
      }
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      if (peek() == '}') {
        ++pos_;
        out = std::move(obj);
        return true;
      }
      fail("expected ',' or '}' in object");
      return false;
    }
  }

  bool parseArray(JsonValue &out, int depth) {
    ++pos_; // '['
    JsonValue arr = JsonValue::makeArray();
    skipWs();
    if (!eof() && peek() == ']') {
      ++pos_;
      out = std::move(arr);
      return true;
    }
    for (;;) {
      skipWs();
      JsonValue value;
      if (!parseValue(value, depth + 1)) return false;
      arr.append(std::move(value));
      skipWs();
      if (eof()) {
        fail("unterminated array");
        return false;
      }
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      if (peek() == ']') {
        ++pos_;
        out = std::move(arr);
        return true;
      }
      fail("expected ',' or ']' in array");
      return false;
    }
  }
};

} // namespace

bool parseJson(const std::string &text, JsonValue &out, std::string &err) {
  Parser parser(text);
  return parser.run(out, err);
}

bool parseJsonFile(const std::string &path, JsonValue &out, std::string &err) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    err = "cannot open config file \"" + path + "\"";
    return false;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  if (in.bad()) {
    err = "cannot read config file \"" + path + "\"";
    return false;
  }
  std::string detail;
  if (!parseJson(buffer.str(), out, detail)) {
    err = path + ": " + detail;
    return false;
  }
  return true;
}

} // namespace radar
