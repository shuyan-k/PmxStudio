// ImGui 主题与偏好持久化：亮/暗配色、settings.ini 读写。
#pragma once

#include <string>

#include <imgui.h>

struct Viewer;

namespace pmxtheme {

// 视口背景与地面网格配色。亮色主题不只是换界面：视口底色、网格与轴线
// 也要一起提亮，否则模型会在深色底上显得突兀。
struct SceneColors {
    float background[3];
    float grid_minor[3];
    float grid_major[3];
    float grid_axis_x[3];
    float grid_axis_z[3];
};

// "light" 使用亮色主题，其余值统一按深色处理。
void apply_theme(const std::string& name);

// 当前主题的视口配色。
SceneColors scene_colors(const std::string& name);

// 把主题的视口配色写入 Viewer；用户自定义过的背景 / 网格颜色保持不变。
void apply_scene_theme(Viewer& v);

// 当前主题的强调色与语义色，供面板与标题区统一使用。
const ImVec4& accent();
const ImVec4& accent_text();
const ImVec4& success();
const ImVec4& warning();
const ImVec4& danger();

// Windows: %APPDATA%/PmxStudioCpp/settings.ini；取不到 APPDATA 时退回当前目录。
std::string settings_path();

// 读取 key=value 文本；文件不存在时返回 false 且不修改 err。
bool load_settings(Viewer& v, std::string* err);

// 尽力保存，内部捕获异常，失败时静默返回。
void save_settings(const Viewer& v);

}  // namespace pmxtheme
