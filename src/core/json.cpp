#include "core/json.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pmx {

const JsonValue* JsonValue::find(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& item : object)
        if (item.first == key) return &item.second;
    return nullptr;
}

namespace {

bool is_digit(char c) { return c >= '0' && c <= '9'; }

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : text_(text) {}

    JsonValue parse_document() {
        // 允许 UTF-8 BOM。
        if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xEF &&
            static_cast<unsigned char>(text_[1]) == 0xBB &&
            static_cast<unsigned char>(text_[2]) == 0xBF) {
            pos_ = 3;
        }
        skip_ws();
        JsonValue value = parse_value();
        skip_ws();
        if (pos_ != text_.size()) fail("文档末尾有多余字符");
        return value;
    }

private:
    [[noreturn]] void fail(const std::string& message) const {
        throw ParseError{pos_, message};
    }

    char peek() const {
        if (pos_ >= text_.size()) fail("意外的输入结束");
        return text_[pos_];
    }

    void skip_ws() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                ++pos_;
            else
                break;
        }
    }

    void expect(char c, const char* message) {
        if (pos_ >= text_.size() || text_[pos_] != c) fail(message);
        ++pos_;
    }

    JsonValue parse_value() {
        skip_ws();
        const char c = peek();
        switch (c) {
            case '{':
                return parse_object();
            case '[':
                return parse_array();
            case '"': {
                JsonValue value;
                value.type = JsonValue::Type::String;
                value.string = parse_string();
                return value;
            }
            case 't':
                parse_literal("true");
                return make_bool(true);
            case 'f':
                parse_literal("false");
                return make_bool(false);
            case 'n':
                parse_literal("null");
                return JsonValue{};
            default:
                if (c == '-' || is_digit(c)) return parse_number();
                fail(std::string("无法识别的字符 '") + c + "'");
        }
    }

    static JsonValue make_bool(bool value) {
        JsonValue out;
        out.type = JsonValue::Type::Bool;
        out.boolean = value;
        return out;
    }

    void parse_literal(const char* literal) {
        const std::size_t len = std::char_traits<char>::length(literal);
        if (text_.compare(pos_, len, literal) != 0) fail("无效的字面量");
        pos_ += len;
    }

    JsonValue parse_number() {
        const std::size_t start = pos_;
        if (peek() == '-') ++pos_;
        if (pos_ >= text_.size() || !is_digit(text_[pos_])) fail("数字缺少整数部分");
        if (text_[pos_] == '0') {
            ++pos_;
        } else {
            while (pos_ < text_.size() && is_digit(text_[pos_])) ++pos_;
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            if (pos_ >= text_.size() || !is_digit(text_[pos_]))
                fail("小数点后缺少数字");
            while (pos_ < text_.size() && is_digit(text_[pos_])) ++pos_;
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            if (pos_ >= text_.size() || !is_digit(text_[pos_]))
                fail("指数部分缺少数字");
            while (pos_ < text_.size() && is_digit(text_[pos_])) ++pos_;
        }

        JsonValue value;
        value.type = JsonValue::Type::Number;
        const char* first = text_.data() + start;
        const char* last = text_.data() + pos_;
        const auto result = std::from_chars(first, last, value.number);
        if (result.ec != std::errc() || result.ptr != last) fail("数字超出可表示范围");
        return value;
    }

    std::string parse_string() {
        expect('"', "期望字符串");
        std::string out;
        while (true) {
            if (pos_ >= text_.size()) fail("字符串未闭合");
            const char c = text_[pos_];
            if (c == '"') {
                ++pos_;
                return out;
            }
            if (c == '\\') {
                ++pos_;
                parse_escape(out);
                continue;
            }
            if (static_cast<unsigned char>(c) < 0x20)
                fail("字符串中含未转义的控制字符");
            out.push_back(c);
            ++pos_;
        }
    }

    void parse_escape(std::string& out) {
        if (pos_ >= text_.size()) fail("转义序列不完整");
        const char c = text_[pos_++];
        switch (c) {
            case '"': out.push_back('"'); return;
            case '\\': out.push_back('\\'); return;
            case '/': out.push_back('/'); return;
            case 'b': out.push_back('\b'); return;
            case 'f': out.push_back('\f'); return;
            case 'n': out.push_back('\n'); return;
            case 'r': out.push_back('\r'); return;
            case 't': out.push_back('\t'); return;
            case 'u': break;
            default: fail("无效的转义字符");
        }

        std::uint32_t code = parse_hex4();
        if (code >= 0xD800 && code <= 0xDBFF) {
            // 高代理项必须跟一个低代理项。
            if (pos_ + 1 >= text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u')
                fail("Unicode 代理项不成对");
            pos_ += 2;
            const std::uint32_t low = parse_hex4();
            if (low < 0xDC00 || low > 0xDFFF) fail("Unicode 代理项不成对");
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
        } else if (code >= 0xDC00 && code <= 0xDFFF) {
            fail("Unicode 代理项不成对");
        }
        append_utf8(out, code);
    }

    std::uint32_t parse_hex4() {
        if (pos_ + 4 > text_.size()) fail("Unicode 转义不足 4 位");
        std::uint32_t code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            code <<= 4;
            if (c >= '0' && c <= '9')
                code |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                code |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                code |= static_cast<std::uint32_t>(c - 'A' + 10);
            else
                fail("Unicode 转义含非十六进制字符");
        }
        return code;
    }

    static void append_utf8(std::string& out, std::uint32_t code) {
        if (code <= 0x7F) {
            out.push_back(static_cast<char>(code));
        } else if (code <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    JsonValue parse_array() {
        expect('[', "期望数组");
        JsonValue value;
        value.type = JsonValue::Type::Array;
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return value;
        }
        while (true) {
            value.array.push_back(parse_value());
            skip_ws();
            const char c = peek();
            if (c == ',') {
                ++pos_;
                skip_ws();
                continue;
            }
            if (c == ']') {
                ++pos_;
                return value;
            }
            fail("数组元素之间缺少逗号或右括号");
        }
    }

    JsonValue parse_object() {
        expect('{', "期望对象");
        JsonValue value;
        value.type = JsonValue::Type::Object;
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return value;
        }
        while (true) {
            skip_ws();
            if (pos_ >= text_.size() || text_[pos_] != '"')
                fail("对象的键必须是字符串");
            std::string key = parse_string();
            skip_ws();
            expect(':', "键与值之间缺少冒号");
            value.object.emplace_back(std::move(key), parse_value());
            skip_ws();
            const char c = peek();
            if (c == ',') {
                ++pos_;
                skip_ws();
                continue;
            }
            if (c == '}') {
                ++pos_;
                return value;
            }
            fail("对象成员之间缺少逗号或右括号");
        }
    }

    const std::string& text_;
    std::size_t pos_ = 0;
};

void dump_string(const std::string& text, std::string& out) {
    out.push_back('"');
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x80) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                default:
                    if (u < 0x20) {
                        static const char* kHex = "0123456789abcdef";
                        out += "\\u00";
                        out.push_back(kHex[(u >> 4) & 0xF]);
                        out.push_back(kHex[u & 0xF]);
                    } else {
                        out.push_back(c);
                    }
                    break;
            }
            ++i;
            continue;
        }

        // JSON 必须是合法 UTF-8。模型名可能来自 CP932 等编码，遇到非法字节
        // 用 U+FFFD 替换，避免把无效字节写进协议导致客户端解析失败。
        std::size_t length = 0;
        if (u >= 0xC2 && u <= 0xDF)
            length = 2;
        else if (u >= 0xE0 && u <= 0xEF)
            length = 3;
        else if (u >= 0xF0 && u <= 0xF4)
            length = 4;
        bool valid = length > 0 && i + length <= text.size();
        if (valid) {
            for (std::size_t k = 1; k < length; ++k) {
                if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) {
                    valid = false;
                    break;
                }
            }
        }
        if (valid) {
            const unsigned char second = static_cast<unsigned char>(text[i + 1]);
            if (u == 0xE0 && second < 0xA0) valid = false;
            if (u == 0xED && second > 0x9F) valid = false;
            if (u == 0xF0 && second < 0x90) valid = false;
            if (u == 0xF4 && second > 0x8F) valid = false;
        }
        if (!valid) {
            out += "\xEF\xBF\xBD";
            ++i;
            continue;
        }
        out.append(text, i, length);
        i += length;
    }
    out.push_back('"');
}

void dump_value(const JsonValue& value, std::string& out) {
    switch (value.type) {
        case JsonValue::Type::Null:
            out += "null";
            break;
        case JsonValue::Type::Bool:
            out += value.boolean ? "true" : "false";
            break;
        case JsonValue::Type::Number: {
            if (!std::isfinite(value.number)) {
                out += "null";
                break;
            }
            char buffer[64];
            const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value.number);
            if (result.ec == std::errc()) {
                out.append(buffer, result.ptr);
            } else {
                out += "0";
            }
            break;
        }
        case JsonValue::Type::String:
            dump_string(value.string, out);
            break;
        case JsonValue::Type::Array:
            out.push_back('[');
            for (std::size_t i = 0; i < value.array.size(); ++i) {
                if (i > 0) out.push_back(',');
                dump_value(value.array[i], out);
            }
            out.push_back(']');
            break;
        case JsonValue::Type::Object:
            out.push_back('{');
            for (std::size_t i = 0; i < value.object.size(); ++i) {
                if (i > 0) out.push_back(',');
                dump_string(value.object[i].first, out);
                out.push_back(':');
                dump_value(value.object[i].second, out);
            }
            out.push_back('}');
            break;
    }
}

}  // namespace

JsonValue parse_json(const std::string& text) {
    return JsonParser(text).parse_document();
}

std::string json_dump(const JsonValue& value) {
    std::string out;
    out.reserve(256);
    dump_value(value, out);
    return out;
}

JsonValue json_null() { return JsonValue{}; }

JsonValue json_bool(bool value) {
    JsonValue out;
    out.type = JsonValue::Type::Bool;
    out.boolean = value;
    return out;
}

JsonValue json_number(double value) {
    JsonValue out;
    out.type = JsonValue::Type::Number;
    out.number = value;
    return out;
}

JsonValue json_string(std::string value) {
    JsonValue out;
    out.type = JsonValue::Type::String;
    out.string = std::move(value);
    return out;
}

JsonValue json_array() {
    JsonValue out;
    out.type = JsonValue::Type::Array;
    return out;
}

JsonValue json_object() {
    JsonValue out;
    out.type = JsonValue::Type::Object;
    return out;
}

void json_push(JsonValue& array, JsonValue value) {
    if (array.type == JsonValue::Type::Array) array.array.push_back(std::move(value));
}

void json_set(JsonValue& object, const std::string& key, JsonValue value) {
    if (object.type != JsonValue::Type::Object) return;
    object.object.emplace_back(key, std::move(value));
}

}  // namespace pmx
