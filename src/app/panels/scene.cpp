// 「场景」面板：主模型之外的额外 PMX 列表、摆放与显示开关。
#include "app/panels.hpp"

#include <cstdio>
#include <string>

#include <imgui.h>

#include "app/panels/common.hpp"

void panel_scene(Viewer& v) {
    if (!v.pose) {
        ImGui::TextDisabled("未加载主模型");
        return;
    }

    if (ImGui::Button("导入 PMX 到场景…")) {
        const std::string path = pick_open_file(kPmxFilter, L"导入场景 PMX");
        if (!path.empty()) {
            std::string err;
            if (v.add_scene_model(path, &err)) {
                v.error.clear();
                v.status = "已导入场景模型 " + path;
            } else {
                v.error = err;
            }
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("也可把多个 .pmx 一起拖进窗口");
    ImGui::TextDisabled("主模型仍可编辑 / 保存；额外模型各自带姿态与物理，只做展示。");

    ImGui::SeparatorText("主模型");
    ImGui::TextUnformatted(v.model.name.empty() ? "（未命名）" : v.model.name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("· 骨骼 %zu · 刚体 %zu",
                        v.model.bones.size(), v.model.rigid.size());

    ImGui::SeparatorText("场景模型");
    if (v.scene_models.empty()) {
        ImGui::TextDisabled("尚未导入额外模型。");
        return;
    }

    int remove_at = -1;
    for (std::size_t i = 0; i < v.scene_models.size(); ++i) {
        SceneModel& sm = v.scene_models[i];
        ImGui::PushID(static_cast<int>(i));
        bool visible = sm.visible;
        if (ImGui::Checkbox("##visible", &visible)) sm.visible = visible;
        ImGui::SameLine();
        ImGui::TextUnformatted(sm.name.empty() ? "（未命名）" : sm.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("· 骨骼 %zu", sm.model.bones.size());

        // 文件路径单独一行，长路径裁剪显示，悬停看完整路径。
        const std::size_t slash = sm.path.find_last_of("/\\");
        const std::string file =
            slash == std::string::npos ? sm.path : sm.path.substr(slash + 1);
        ImGui::TextDisabled("%s", file.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", sm.path.c_str());

        bool rebuild = false;
        ImGui::SetNextItemWidth(pmxui::scale(180.0f));
        ImGui::DragFloat3("位置", sm.pos, 0.01f, 0.0f, 0.0f, "%.3f");
        rebuild |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SetNextItemWidth(pmxui::scale(120.0f));
        ImGui::DragFloat("朝向", &sm.yaw, 0.5f, -360.0f, 360.0f, "%.1f°");
        rebuild |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SetNextItemWidth(pmxui::scale(120.0f));
        ImGui::DragFloat("缩放", &sm.scale, 0.005f, 0.01f, 100.0f, "%.3f");
        rebuild |= ImGui::IsItemDeactivatedAfterEdit();
        if (rebuild) {
            std::string err;
            if (!v.rebuild_scene_model(static_cast<int>(i), &err)) v.error = err;
        }
        if (ImGui::Button("移除")) remove_at = static_cast<int>(i);
        ImGui::Spacing();
        ImGui::PopID();
    }
    if (remove_at >= 0) v.remove_scene_model(remove_at);
}
