// 相机：独立于模型动作的相机动作（导入 / 保存 / 打帧 / 视角）。
#include "app/panels.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include <imgui.h>

#include "app/timeline.hpp"
#include "app/ui_scale.hpp"
#include "app/viewer.hpp"

namespace {

const wchar_t* kCameraVmdFilter = L"VMD 相机动作 (*.vmd)\0*.vmd\0所有文件 (*.*)\0*.*\0";

void import_camera_dialog(Viewer& v) {
    if (!v.pose) {
        v.status = "先载入模型";
        return;
    }
    const std::string path =
        pick_open_file(kCameraVmdFilter, L"导入相机 VMD（只取相机帧）");
    if (path.empty()) return;
    std::string err;
    if (v.load_camera_file(path, &err))
        v.error.clear();
    else
        v.error = err;
}

void jump_to_key(Viewer& v, int frame) {
    v.motion_frame = static_cast<float>(std::max(0, frame));
    v.timeline.frame = v.motion_frame;
    v.motion_dirty = true;
    if (v.physics_enabled) v.settle_physics();
}

}  // namespace

void panel_camera(Viewer& v) {
    if (!v.pose) {
        ImGui::TextDisabled("未加载模型");
        return;
    }

    ImGui::TextWrapped(
        "相机动作是独立的一份数据：可以和模型动作来自同一个 VMD，但导入、保存、"
        "清除互不影响。");
    ImGui::Spacing();

    ImGui::SeparatorText("相机动作");
    if (v.camera_motion.loaded()) {
        ImGui::Text("来源：%s", v.camera_motion.source.c_str());
        ImGui::Text("关键帧：%d · 范围 0 – %d 帧", static_cast<int>(v.camera_motion.track.keys.size()),
                    v.camera_motion.max_frame());
        if (v.camera_motion.dirty) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "有未保存的修改");
    } else {
        ImGui::TextDisabled("尚未载入相机动作。");
    }

    if (ImGui::Button("导入相机 VMD…")) import_camera_dialog(v);
    ImGui::SameLine();
    ImGui::BeginDisabled(!v.camera_motion.loaded());
    if (ImGui::Button("保存相机 VMD…")) v.save_camera_dialog();
    ImGui::SameLine();
    if (ImGui::Button("清除相机动作")) v.clear_camera_motion();
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::SeparatorText("取景");
    if (ImGui::Button(v.camera_view ? "退出相机视角 (0)" : "进入相机视角 (0)"))
        v.toggle_camera_view();
    ImGui::SameLine();
    ImGui::Checkbox("显示相机框", &v.camera_gizmo);
    ImGui::SetItemTooltip("在视口画出机位、镜头方向与取景锥");

    ImGui::BeginDisabled(!v.camera_view);
    if (ImGui::Button("记录当前视角为关键帧 (K)")) timeline_insert_camera_key(v);
    ImGui::EndDisabled();
    ImGui::TextDisabled("进入相机视角后调整视角，再按 K 覆盖当前帧。");
    ImGui::TextDisabled("渲染输出有相机动作时按相机轨取景。");

    ImGui::Spacing();
    ImGui::SeparatorText("关键帧");

    if (!v.camera_motion.loaded()) {
        ImGui::TextDisabled("没有相机关键帧。");
        return;
    }

    ImGui::BeginChild("##camera_keys", ImVec2(0.0f, pmxui::scale(220.0f)),
                      ImGuiChildFlags_Borders);
    const auto& keys = v.camera_motion.track.keys;
    int remove = -1;
    for (int i = 0; i < static_cast<int>(keys.size()); ++i) {
        const pmx::AnimCameraKey& key = keys[static_cast<std::size_t>(i)];
        char label[160];
        std::snprintf(label, sizeof label, "帧 %d · 距离 %.1f · 注视 (%.1f, %.1f, %.1f) · FOV %d°",
                      key.frame, key.distance, key.target.x, key.target.y, key.target.z,
                      key.fov);
        ImGui::PushID(i);
        if (ImGui::SmallButton("删除")) remove = i;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("删除这一帧");
        ImGui::SameLine();
        if (ImGui::Selectable(label, false, ImGuiSelectableFlags_AllowDoubleClick) &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            jump_to_key(v, key.frame);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    if (remove >= 0) {
        auto& mutable_keys = v.camera_motion.track.keys;
        mutable_keys.erase(mutable_keys.begin() + remove);
        v.camera_motion.dirty = true;
        v.timeline.selection.clear();
        v.status = "已删除一个相机关键帧";
    }
}
