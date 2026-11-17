#include "core/script.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "core/fs.hpp"

namespace pmx {
namespace {

// ---------------------------------------------------------------- 最小 JSON
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

    const JsonValue* find(const std::string& key) const {
        if (type != Type::Object) return nullptr;
        for (const auto& item : object)
            if (item.first == key) return &item.second;
        return nullptr;
    }
};

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

std::string position_text(const std::string& text, std::size_t pos) {
    std::size_t line = 1;
    std::size_t column = 1;
    for (std::size_t i = 0; i < pos && i < text.size(); ++i) {
        if (text[i] == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
    return "第 " + std::to_string(line) + " 行第 " + std::to_string(column) + " 列";
}

// ---------------------------------------------------------------- 命令执行
struct CommandError {
    std::string message;
};

const JsonValue& require_object(const JsonValue& node) {
    if (node.type != JsonValue::Type::Object)
        throw CommandError{"命令必须是 JSON 对象"};
    return node;
}

const std::string& require_string(const JsonValue& node, const char* key) {
    const JsonValue* field = node.find(key);
    if (!field) throw CommandError{std::string("缺少参数 \"") + key + "\""};
    if (field->type != JsonValue::Type::String)
        throw CommandError{std::string("参数 \"") + key + "\" 必须是字符串"};
    return field->string;
}

const JsonValue* optional_number(const JsonValue& node, const char* key) {
    const JsonValue* field = node.find(key);
    if (field && field->type != JsonValue::Type::Number)
        throw CommandError{std::string("参数 \"") + key + "\" 必须是数字"};
    return field;
}

template <typename T>
void rename_entries(std::vector<T>& items, const std::string& from, const std::string& to) {
    for (auto& item : items)
        if (item.name == from || item.name_en == from) item.name = to;
}

void execute_command(Model& model, const JsonValue& node, std::string& message) {
    require_object(node);
    const std::string& cmd = require_string(node, "cmd");

    if (cmd == "rename_bone") {
        const std::string& from = require_string(node, "from");
        const std::string& to = require_string(node, "to");
        rename_entries(model.bones, from, to);
    } else if (cmd == "rename_material") {
        const std::string& from = require_string(node, "from");
        const std::string& to = require_string(node, "to");
        rename_entries(model.materials, from, to);
    } else if (cmd == "rename_morph") {
        const std::string& from = require_string(node, "from");
        const std::string& to = require_string(node, "to");
        rename_entries(model.morphs, from, to);
    } else if (cmd == "rename_bone_prefix") {
        const std::string& from = require_string(node, "from");
        const std::string& to = require_string(node, "to");
        if (!from.empty()) {
            for (auto& bone : model.bones)
                if (bone.name.rfind(from, 0) == 0) bone.name = to + bone.name.substr(from.size());
        }
    } else if (cmd == "set_rigid") {
        const std::string& name = require_string(node, "name");
        const JsonValue* mass = optional_number(node, "mass");
        const JsonValue* friction = optional_number(node, "friction");
        const JsonValue* restitution = optional_number(node, "restitution");
        const JsonValue* mode = optional_number(node, "mode");
        int mode_value = 0;
        if (mode) {
            if (!std::isfinite(mode->number) || std::floor(mode->number) != mode->number ||
                mode->number < 0.0 || mode->number > 255.0)
                throw CommandError{"参数 \"mode\" 必须是 0..255 的整数"};
            mode_value = static_cast<int>(mode->number);
        }
        for (auto& rb : model.rigid) {
            if (rb.name != name && rb.name_en != name) continue;
            if (mass) rb.mass = static_cast<float>(mass->number);
            if (friction) rb.friction = static_cast<float>(friction->number);
            // PMX 的「反弹力」在数据模型中叫 repel，语义即 restitution。
            if (restitution) rb.repel = static_cast<float>(restitution->number);
            if (mode) rb.mode = static_cast<std::uint8_t>(mode_value);
        }
    } else if (cmd == "count_bones") {
        message += "骨骼数量：" + std::to_string(model.bones.size()) + "\n";
    } else if (cmd == "count_rigid") {
        message += "刚体数量：" + std::to_string(model.rigid.size()) + "\n";
    } else {
        throw CommandError{"未知命令 \"" + cmd + "\""};
    }
}

}  // namespace

ScriptResult run_script_json(Model& model, const std::string& json,
                             const std::string& base_dir) {
    (void)base_dir;  // 当前命令集不涉及外部文件。

    ScriptResult result;
    JsonValue root;
    try {
        root = JsonParser(json).parse_document();
    } catch (const ParseError& error) {
        result.ok = false;
        result.message = "JSON 解析失败（" + position_text(json, error.pos) + "）：" +
                         error.message;
        return result;
    }

    if (root.type != JsonValue::Type::Array) {
        result.ok = false;
        result.message = "脚本必须是 JSON 命令数组";
        return result;
    }

    for (std::size_t i = 0; i < root.array.size(); ++i) {
        try {
            execute_command(model, root.array[i], result.message);
        } catch (const CommandError& error) {
            result.ok = false;
            result.message = "第 " + std::to_string(i + 1) + " 条命令：" + error.message;
            return result;
        }
        ++result.commands;
    }

    if (!result.message.empty()) result.message.pop_back();  // 去掉末尾换行
    return result;
}

ScriptResult run_script_file(Model& model, const std::string& path) {
    std::ifstream in = open_ifstream(path);
    if (!in) {
        ScriptResult result;
        result.ok = false;
        result.message = "无法打开脚本文件：" + path;
        return result;
    }
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    if (in.bad()) {
        ScriptResult result;
        result.ok = false;
        result.message = "读取脚本文件失败：" + path;
        return result;
    }
    const std::string base_dir = utf8_string(utf8_path(path).parent_path());
    return run_script_json(model, text, base_dir);
}

}  // namespace pmx
