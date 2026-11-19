// 相机：独立于模型动作的相机动作（导入 / 保存 / 打帧 / 视角）。
#include "app/panels.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include <imgui.h>

#include "app/theme.hpp"
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

    // ------------------------------------------------------------ 当前视角
    // 参数化视角（参考 MMD 相机面板 / Blender 相机的焦距与裁剪面）：
    // 只影响查看器与渲染输出，不写入 PMX；进入相机视角后由相机动作接管。
    ImGui::SeparatorText("当前视角");
    // 相机视角下由相机动作驱动，参数只读；没有相机动作时当前视角就是默认相机，
    // 这些参数仍然可改，改完按 K 即可记成第一帧。
    const bool driven_by_motion = v.camera_view && v.camera_motion.loaded();
    ImGui::BeginDisabled(driven_by_motion);

    float fov = v.camera.fov;
    ImGui::SetNextItemWidth(pmxui::scale(-90.0f));
    if (ImGui::SliderFloat("视角 FOV##cam_fov", &fov, 5.0f, 120.0f, "%.1f°"))
        v.camera.fov = fov;

    // 35mm 全画幅（24mm 高）等效焦距：f = 12 / tan(垂直 FOV / 2)。
    float focal = 12.0f / std::tan(v.camera.fov * pmx::kPi / 360.0f);
    ImGui::SetNextItemWidth(pmxui::scale(-90.0f));
    // 焦距下限与 FOV 上限一致：f = 12 / tan(120° / 2) ≈ 6.93mm。
    if (ImGui::DragFloat("焦距##cam_focal", &focal, 0.5f, 6.93f, 300.0f, "%.1f mm"))
        v.camera.fov = 2.0f * std::atan(12.0f / std::max(1.0f, focal)) * 180.0f / pmx::kPi;
    ImGui::SetItemTooltip("按 35mm 全画幅换算，与视角 FOV 联动");

    ImGui::SetNextItemWidth(pmxui::scale(-90.0f));
    ImGui::DragFloat("距离##cam_distance", &v.camera.distance, 0.2f, 0.6f, 2000.0f, "%.1f");
    ImGui::DragFloat3("注视点##cam_target", &v.camera.target.x, 0.1f, -10000.0f, 10000.0f,
                      "%.1f");

    if (ImGui::Checkbox("正交投影##cam_ortho", &v.camera.ortho) && v.camera.ortho) {
        // 切到正交时按当前透视画幅换算取景高度，切换瞬间构图不跳。
        v.camera.ortho_scale =
            2.0f * v.camera.distance * std::tan(v.camera.fov * pmx::kPi / 360.0f);
    }
    ImGui::SetItemTooltip("对应 MMD 相机关闭「透视」");
    if (v.camera.ortho) {
        ImGui::SetNextItemWidth(pmxui::scale(-90.0f));
        ImGui::DragFloat("取景高度##cam_ortho_scale", &v.camera.ortho_scale, 0.1f, 0.05f, 4000.0f,
                         "%.1f");
    }

    ImGui::SetNextItemWidth(pmxui::scale(-90.0f));
    ImGui::DragFloat("近裁剪面##cam_near", &v.camera.near_plane, 0.01f, 0.01f, 100.0f, "%.2f");
    ImGui::SetNextItemWidth(pmxui::scale(-90.0f));
    ImGui::DragFloat("远裁剪面##cam_far", &v.camera.far_plane, 10.0f, 10.0f, 20000.0f, "%.0f");
    // 近裁剪面必须小于远裁剪面，否则投影矩阵会退化、画面全黑。
    if (v.camera.far_plane <= v.camera.near_plane)
        v.camera.far_plane = v.camera.near_plane + 10.0f;

    if (ImGui::Button("重置视角参数")) {
        v.camera.fov = 38.0f;
        v.camera.near_plane = 0.1f;
        v.camera.far_plane = 4000.0f;
        v.camera.ortho = false;
        v.camera.ortho_scale = 30.0f;
    }
    ImGui::EndDisabled();
    if (driven_by_motion)
        ImGui::TextDisabled("相机视角下由相机动作接管；按 0 退出后可手动调整。");
    else
        ImGui::TextDisabled("这些参数只影响查看器与渲染输出，不写入 PMX / VMD。");

    ImGui::Spacing();
    ImGui::SeparatorText("相机动作");
    if (v.camera_motion.loaded()) {
        ImGui::TextWrapped("来源：%s", v.camera_motion.source.c_str());
        ImGui::Text("关键帧：%d · 范围 0 – %d 帧", static_cast<int>(v.camera_motion.track.keys.size()),
                    v.camera_motion.max_frame());
        if (v.camera_motion.dirty)
            ImGui::TextColored(pmxtheme::warning(), "有未保存的修改");
    } else {
        ImGui::TextDisabled("当前使用默认相机（= 当前视角），还没有记录关键帧。");
    }

    if (ImGui::Button("导入 VMD…")) import_camera_dialog(v);
    ImGui::SetItemTooltip("只导入 VMD 里的相机帧，不影响模型动作");
    ImGui::SameLine();
    ImGui::BeginDisabled(!v.camera_motion.loaded());
    if (ImGui::Button("保存 VMD…")) v.save_camera_dialog();
    ImGui::SetItemTooltip("把相机动作保存为只含相机帧的 VMD");
    ImGui::SameLine();
    if (ImGui::Button("清除")) v.clear_camera_motion();
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::SeparatorText("取景");
    if (ImGui::Button(v.camera_view ? "退出相机视角 (0)" : "进入相机视角 (0)"))
        v.toggle_camera_view();
    ImGui::SameLine();
    ImGui::Checkbox("显示相机框", &v.camera_gizmo);
    ImGui::SetItemTooltip("在视口画出机位、镜头方向与取景锥");

    ImGui::BeginDisabled(v.camera_motion.loaded() && !v.camera_view);
    if (ImGui::Button("记录当前视角为关键帧 (K)")) timeline_insert_camera_key(v);
    ImGui::EndDisabled();
    ImGui::TextDisabled("在相机视角 (0) 下调整构图后按 K 记录；记录第一帧后默认相机即变为相机动作。");
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
