// 批处理脚本：JSON 命令数组，作用于已加载的模型。
//
// 命令示例：
//   [{"cmd":"rename_bone","from":"センター","to":"center"},
//    {"cmd":"set_rigid","name":"スカート_0_1","mass":1.0,"friction":0.8}]
// 只依赖 pmx_core，不需要 GL / ImGui。
#pragma once

#include <string>

#include "core/pmx.hpp"

namespace pmx {

struct ScriptResult {
    bool ok = true;
    int commands = 0;
    std::string message;
};

// 执行一段 JSON 脚本；base_dir 用于解析脚本里的相对路径。
ScriptResult run_script_json(Model& model, const std::string& json,
                             const std::string& base_dir = std::string());

// 读取文件并执行。文件读不到时返回 ok=false。
ScriptResult run_script_file(Model& model, const std::string& path);

}  // namespace pmx
