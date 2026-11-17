// 烘焙页：把实时物理逐帧固化成骨骼轨，输出到时间线。
//
// 烘焙是同步的（首版）：长动作期间界面会短暂无响应，完成后一次性替换 clip。
// 采样源与输出分开，避免写入的关键帧影响后续帧的物理输入。
#include "app/panels.hpp"

#include <algorithm>
#include <string>

#include <imgui.h>

#include "app/modules.hpp"
#include "app/panels/common.hpp"
#include "app/timeline.hpp"
#include "core/anim.hpp"
#include "core/bake.hpp"
#include "physics/physics.hpp"

void panel_bake(Viewer& v) {
#ifdef PMX_ENABLE_PHYSICS
    if (!v.pose || !v.physics_ptr) {
        ImGui::TextDisabled("需要载入模型并启用物理后才可烘焙");
        return;
    }
    if (!pmx::modules().bake) {
        ImGui::TextDisabled("当前构建未启用烘焙模块");
        return;
    }
    if (!v.timeline.clip) {
        ImGui::TextDisabled("先导入 VMD，或在时间线中按 K 打一帧后再烘焙");
        return;
    }

    static pmx::BakeOptions options;
    static bool initialized = false;
    const int last_frame = v.timeline.model_max_frame();
    if (!initialized) {
        options.end_frame = last_frame;
        initialized = true;
    }
    options.begin_frame = std::clamp(options.begin_frame, 0, std::max(0, last_frame));
    options.end_frame = std::clamp(options.end_frame, options.begin_frame, std::max(0, last_frame));

    ImGui::SeparatorText("范围");
    ImGui::SetNextItemWidth(pmxui::scale(-96.0f));
    ImGui::DragInt("起始帧", &options.begin_frame, 1.0f, 0, last_frame);
    ImGui::SetNextItemWidth(pmxui::scale(-96.0f));
    ImGui::DragInt("结束帧", &options.end_frame, 1.0f, 0, last_frame);
    ImGui::TextDisabled("共 %d 帧（30 fps）", options.end_frame - options.begin_frame + 1);

    ImGui::SeparatorText("输出");
    ImGui::Checkbox("烘焙全部骨骼", &options.include_all_bones);
    ImGui::SameLine();
    ImGui::Checkbox("覆盖同帧旧关键帧", &options.replace);
    ImGui::Checkbox("起始帧先复位并预热", &options.warmup);

    ImGui::SeparatorText("物理");
    ImGui::Text("重力 %.2f · 迭代 %d · 子步 %d", v.physics_ptr->gravity,
                v.physics_ptr->iterations, v.physics_ptr->warmup_steps);
    ImGui::TextDisabled("烘焙只写刚体驱动的骨骼；柔体变形无法写入 VMD，仅在引擎内可见。");

    ImGui::Spacing();
    if (ImGui::Button("开始烘焙", ImVec2(pmxui::scale(160.0f), 0.0f))) {
        pmx::AnimClip source = *v.timeline.clip;
        pmx::AnimClip output = source;
        pmx::BakeProgress progress;
        progress.total = options.end_frame - options.begin_frame + 1;
        std::string error;
        v.physics_ptr->warmup_steps = std::max(1, options.warmup_steps);
        const int written = pmx::bake_physics(source, output, v.model, *v.pose,
                                              *v.physics_ptr, &v.morph_weights, options,
                                              &progress, &error);
        if (written < 0) {
            v.status = "烘焙失败：" + error;
        } else {
            output.sort_keys();
            timeline_set_clip(v, std::move(output));
            v.timeline.enabled = true;
            v.motion_dirty_edit = true;
            v.status = "已烘焙 " + std::to_string(written) + " 个关键帧";
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("重置物理")) {
        v.physics_ptr->reset(*v.pose, &v.morph_weights, true);
        v.status = "物理已重置";
    }
#else
    ImGui::TextDisabled("当前构建未启用物理");
#endif
}
