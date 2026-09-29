#pragma once
// Json.h — 零依赖的最小 JSON DOM 与解析器（供配置入口使用）。
//
// 项目约定核心零第三方依赖（见 README「环境要求」），因此这里自实现一个
// 足够小的 JSON 解析器：支持 object / array / string / number / true /
// false / null、标准转义与 \uXXXX（含 UTF-16 代理对），失败时给出行列位置。
// 它只负责把配置文件读成 DOM，字段级类型校验由 CaptureConfig 负责；
// 不做通用 JSON 库的性能优化（配置文件是 KB 级、一次性读取）。

#include <map>
#include <string>
#include <vector>

namespace radar {

class JsonValue {
public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  JsonValue() = default; // null
  explicit JsonValue(bool v) : type_(Type::Bool), bool_(v) {}
  explicit JsonValue(double v) : type_(Type::Number), number_(v) {}
  explicit JsonValue(std::string v) : type_(Type::String), string_(std::move(v)) {}

  static JsonValue makeArray();
  static JsonValue makeObject();

  Type type() const { return type_; }
  bool isNull() const { return type_ == Type::Null; }
  bool isBool() const { return type_ == Type::Bool; }
  bool isNumber() const { return type_ == Type::Number; }
  bool isString() const { return type_ == Type::String; }
  bool isArray() const { return type_ == Type::Array; }
  bool isObject() const { return type_ == Type::Object; }

  bool asBool() const { return bool_; }
  double asNumber() const { return number_; }
  const std::string &asString() const { return string_; }
  const std::vector<JsonValue> &asArray() const { return array_; }
  const std::map<std::string, JsonValue> &members() const { return object_; }

  // 对象查键；非对象或键不存在时返回 nullptr。查找按文件出现顺序遍历
  // （成员用 std::map 存储，但错误提示按插入顺序更贴近用户所写内容——
  // 这里为简单起见直接用 map 的有序遍历）。
  const JsonValue *find(const std::string &key) const;

  // 供错误提示使用的类型名（"string" / "number" / ...）。
  const char *typeName() const;

  // 解析期构造接口：append 仅对数组有效，put 仅对对象有效（重复键返回 false）。
  bool append(JsonValue v);
  bool put(std::string key, JsonValue v);

private:
  Type type_ = Type::Null;
  bool bool_ = false;
  double number_ = 0.0;
  std::string string_;
  std::vector<JsonValue> array_;
  std::map<std::string, JsonValue> object_;
};

// 解析 JSON 文本；失败时返回 false 并把「原因 + 行列」写入 `err`。
bool parseJson(const std::string &text, JsonValue &out, std::string &err);

// 读取并解析文件；失败信息中带文件路径（缺失/不可读/语法错误）。
bool parseJsonFile(const std::string &path, JsonValue &out, std::string &err);

} // namespace radar
