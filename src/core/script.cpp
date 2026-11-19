#include "core/script.hpp"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "core/fs.hpp"
#include "core/json.hpp"

namespace pmx {
namespace {

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
        root = parse_json(json);
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
