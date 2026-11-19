// 最小 JSON：解析与序列化。
//
// 脚本批处理（core/script）、Agent API（app/api）与插件桥接共用这一份实现，
// 不引入第三方依赖；只覆盖 PMX 工具链需要的那部分 JSON（UTF-8、无注释）。
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace pmx {

struct ParseError {
    std::size_t pos = 0;
    std::string message;
};

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;

    bool is_null() const { return type == Type::Null; }
    bool is_object() const { return type == Type::Object; }
    bool is_array() const { return type == Type::Array; }
    bool is_string() const { return type == Type::String; }
    bool is_number() const { return type == Type::Number; }
    bool is_bool() const { return type == Type::Bool; }

    // 仅对 Object 有效；找不到返回 nullptr。
    const JsonValue* find(const std::string& key) const;
};

// 解析失败抛出 ParseError（pos 为 UTF-8 字节偏移）。
JsonValue parse_json(const std::string& text);

// 序列化成单行 JSON；字符串按 UTF-8 原样输出，非有限数写成 null。
std::string json_dump(const JsonValue& value);

// ---------------------------------------------------------------- 构造辅助
JsonValue json_null();
JsonValue json_bool(bool value);
JsonValue json_number(double value);
JsonValue json_string(std::string value);
JsonValue json_array();
JsonValue json_object();

void json_push(JsonValue& array, JsonValue value);
void json_set(JsonValue& object, const std::string& key, JsonValue value);

}  // namespace pmx
