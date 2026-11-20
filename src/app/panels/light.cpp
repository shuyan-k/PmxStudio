// 灯光：参考 MMD 的照明面板（方向 / 照明色 / 環境色）与 Blender 的太阳光，
// 只保留一盏方向光。改动只影响视图与渲染输出，不写入 PMX。
#include "app/panels.hpp"

#include <algorithm>
#include <cmath>

#include <imgui.h>

#include "app/theme.hpp"
#include "app/ui_scale.hpp"
#include "app/viewer.hpp"

namespace {

struct LightPreset {
    const char* name;
    float dir[3];
};

const LightPreset kPresets[] = {
    {"默认", {0.35f, 0.72f, 0.45f}},
    {"正面", {0.0f, 0.35f, 1.0f}},
    {"左侧", {-1.0f, 0.40f, 0.20f}},
    {"右侧", {1.0f, 0.40f, 0.20f}},
    {"逆光", {0.0f, 0.35f, -1.0f}},
    {"顶光", {0.0f, 1.0f, 0.0f}},
};

void apply_preset(Viewer& v, const LightPreset& preset) {
    for (int i = 0; i < 3; ++i) v.light[i] = preset.dir[i];
}

}  // namespace

void panel_light(Viewer& v) {
    ImGui::TextWrapped(
        "与 MMD 照明面板一致：一盏方向光加环境色。方向指向光源（Y 向上为正），"
        "照明色 × 强度作用在受光面，环境色乘在暗部。");
    ImGui::Spacing();

    // ------------------------------------------------------------ 方向
    ImGui::SeparatorText("方向（指向光源）");
    ImGui::SetNextItemWidth(pmxui::scale(-90.0f));
    ImGui::SliderFloat("X##light_x", &v.light[0], -1.0f, 1.0f, "%.2f");
    ImGui::SetNextItemWidth(pmxui::scale(-90.0f));
    ImGui::SliderFloat("Y##light_y", &v.light[1], -1.0f, 1.0f, "%.2f");
    ImGui::SetNextItemWidth(pmxui::scale(-90.0f));
    ImGui::SliderFloat("Z##light_z", &v.light[2], -1.0f, 1.0f, "%.2f");

    const float len = std::sqrt(v.light[0] * v.light[0] + v.light[1] * v.light[1] +
                               v.light[2] * v.light[2]);
    if (len < 1e-3f) {
        ImGui::TextColored(pmxtheme::warning(), "方向为零：渲染时按正上方处理");
    } else {
        ImGui::TextDisabled("归一化后：%.2f, %.2f, %.2f", v.light[0] / len, v.light[1] / len,
                            v.light[2] / len);
    }

    // ------------------------------------------------------------ 颜色
    ImGui::SeparatorText("颜色");
    ImGui::ColorEdit3("照明色##light_color", v.light_color,
                      ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_Float);
    ImGui::SetItemTooltip("受光面的颜色，MMD 的「照明色」");
    ImGui::SetNextItemWidth(pmxui::scale(-90.0f));
    ImGui::SliderFloat("强度##light_intensity", &v.light_intensity, 0.0f, 3.0f, "%.2f");
    ImGui::SetItemTooltip("照明色的整体倍率；1.0 为默认亮度");
    ImGui::ColorEdit3("环境色##scene_ambient", v.ambient,
                      ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_Float);
    ImGui::SetItemTooltip("乘在暗部，相当于 MMD 的「環境色」：调暗则阴影更深");

    // ------------------------------------------------------------ 阴影
    ImGui::SeparatorText("阴影");
    ImGui::Checkbox("启用方向光阴影##shadows", &v.shadows);
    ImGui::SetItemTooltip("2048² 深度贴图 + 5×5 PCF；关闭后回到纯卡通明暗，没有投影");
    if (v.shadows) {
        ImGui::SetNextItemWidth(pmxui::scale(-90.0f));
        ImGui::SliderFloat("强度##shadow_strength", &v.shadow_strength, 0.0f, 1.0f, "%.2f");
        ImGui::SetItemTooltip("遮挡区压暗的比例；阴影里仍保留环境色，不会变成纯黑");
        ImGui::SetNextItemWidth(pmxui::scale(-90.0f));
        ImGui::SliderFloat("柔和度##shadow_softness", &v.shadow_softness, 0.5f, 3.0f, "%.2f");
        ImGui::SetItemTooltip("PCF 采样间距（纹素）：越小边缘越硬，越接近 MMD 的硬边阴影");
        ImGui::Checkbox("地面接影##shadow_ground", &v.shadow_ground);
        ImGui::SetItemTooltip("在地面网格上显示模型投下的阴影，只压暗被遮挡区域");
    }

    // ------------------------------------------------------------ 预设
    ImGui::SeparatorText("预设");
    for (int i = 0; i < static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0])); ++i) {
        if (i % 3 != 0) ImGui::SameLine();
        if (ImGui::Button(kPresets[i].name)) apply_preset(v, kPresets[i]);
    }

    // ------------------------------------------------------------ 视口指示
    ImGui::SeparatorText("视口");
    ImGui::Checkbox("显示光源指示##show_light", &v.show_light);
    ImGui::SetItemTooltip("在视口画出太阳标记与照射方向（与 Blender 的灯光对象一致）");
    ImGui::SameLine();
    if (ImGui::Button("重置灯光")) {
        apply_preset(v, kPresets[0]);
        v.light_color[0] = v.light_color[1] = v.light_color[2] = 1.0f;
        v.light_intensity = 1.0f;
        v.ambient[0] = v.ambient[1] = v.ambient[2] = 1.0f;
        v.shadows = true;
        v.shadow_strength = 0.65f;
        v.shadow_softness = 1.0f;
        v.shadow_ground = true;
    }
    ImGui::TextDisabled("骨骼 / 刚体 / 关节叠加层使用独立固定光照，不随上方灯光方向变化。");
    ImGui::TextDisabled("灯光随 settings.ini 保存，下次启动沿用。");
}
