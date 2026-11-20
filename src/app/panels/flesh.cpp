// 接触式肉体变形：自接触凹陷 / 周边隆起 / 区域软硬。只影响视图与渲染输出。
#include "app/panels.hpp"

#include <imgui.h>

#include "app/viewer.hpp"

namespace {

const std::string& bone_label(const pmx::Bone& bone) {
    return bone.name.empty() ? bone.name_en : bone.name;
}

}  // namespace

void panel_flesh(Viewer& v) {
    ImGui::TextWrapped(
        "弯曲关节时自动检测网格自身的挤压：接触处压出凹陷，周围一圈向外隆起。"
        "逐帧求解、无模拟缓存，拖动时间轴结果一致。");
    ImGui::Spacing();

    ImGui::Checkbox("启用肉体接触变形", &v.flesh);
    ImGui::BeginDisabled(!v.flesh);

    float pct = v.flesh_range * 100.0f;
    if (ImGui::SliderFloat("接触距离", &pct, 0.2f, 8.0f, "%.2f%% 身高"))
        v.flesh_range = pct / 100.0f;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("小于该距离且法线相对的两处表面视为自接触；过大容易把薄衣物误判为接触。");

    ImGui::SliderFloat("凹陷深度", &v.flesh_dent, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("隆起强度", &v.flesh_bulge, 0.0f, 2.0f, "%.2f");
    ImGui::SliderFloat("隆起范围", &v.flesh_width, 1.0f, 8.0f, "%.1f 倍接触距离");
    ImGui::SliderFloat("整体硬度", &v.flesh_firmness, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("1 = 完全不形变，可用于整体收住效果。");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("区域软硬");
    ImGui::TextWrapped("指定一根骨骼：受它影响的顶点额外变硬，模拟骨骼附近的紧实部位。");

    const int bone_count = static_cast<int>(v.model.bones.size());
    if (v.flesh_region_bone >= bone_count) v.flesh_region_bone = -1;
    const char* current = v.flesh_region_bone >= 0
                              ? bone_label(v.model.bones[static_cast<std::size_t>(
                                    v.flesh_region_bone)]).c_str()
                              : "（无）";
    if (ImGui::BeginCombo("硬区域骨骼", current)) {
        if (ImGui::Selectable("（无）", v.flesh_region_bone < 0)) v.flesh_region_bone = -1;
        for (int i = 0; i < bone_count; ++i) {
            const bool selected = v.flesh_region_bone == i;
            const auto& bone = v.model.bones[static_cast<std::size_t>(i)];
            const std::string& label = bone_label(bone);
            if (ImGui::Selectable(label.empty() ? "（未命名骨骼）" : label.c_str(), selected))
                v.flesh_region_bone = i;
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::BeginDisabled(v.flesh_region_bone < 0);
    ImGui::SliderFloat("该区域硬度", &v.flesh_region_firmness, 0.0f, 1.0f, "%.2f");
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::TextDisabled("启用后蒙皮改走 CPU 路径，超大模型预览可能变慢。");
    ImGui::EndDisabled();
}
