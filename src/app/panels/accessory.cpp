// 「附件」面板：额外加载一个 PMX（如头部挂饰），绑定到主模型骨骼跟随动作。
#include "app/panels.hpp"

#include <cstdio>
#include <string>

#include <imgui.h>

#include "app/panels/common.hpp"
#include "core/pmx.hpp"

void panel_accessory(Viewer& v) {
    if (!v.pose) {
        ImGui::TextDisabled("未加载模型");
        return;
    }

    if (!v.accessory_loaded) {
        if (ImGui::Button("导入附件模型…")) {
            const std::string path = pick_open_file(kPmxFilter, L"导入附件 PMX");
            if (!path.empty()) {
                std::string err;
                if (v.load_accessory(path, &err)) v.error.clear();
                else v.error = err;
            }
        }
        ImGui::Spacing();
        ImGui::TextDisabled("附件与主模型一起渲染，并绑定到指定骨骼跟随动作；");
        ImGui::TextDisabled("不会写入主模型，也不出现在编辑面板里。");
        return;
    }

    ImGui::TextWrapped("%s", v.accessory_path.c_str());
    ImGui::TextDisabled("顶点 %zu · 材质 %zu · 骨骼 %zu · 刚体 %zu · 关节 %zu",
                        v.accessory.vertices.pos.size(), v.accessory.materials.size(),
                        v.accessory.bones.size(), v.accessory.rigid.size(),
                        v.accessory.joints.size());
    if (v.accessory.rigid.empty()) {
        ImGui::TextDisabled("附件没有刚体：只按自带骨骼层级跟随锚点。");
    } else if (v.physics_enabled) {
        ImGui::TextDisabled("附件自带骨骼/物理已接入：重力与迭代次数跟随主模型。");
    } else {
        ImGui::TextDisabled("物理已关闭：附件按自带骨骼静止姿态刚性跟随。");
    }

    ImGui::SeparatorText("锚点骨骼");
    const int bone_count = static_cast<int>(v.model.bones.size());
    const int anchor = (v.accessory_bone >= 0 && v.accessory_bone < bone_count)
                           ? v.accessory_bone
                           : -1;
    if (ImGui::BeginCombo("跟随骨骼", anchor >= 0 ? v.model.bones[static_cast<std::size_t>(anchor)].name.c_str()
                                                   : "（未选择）")) {
        int pick = -1;
        for (int i = 0; i < bone_count; ++i) {
            ImGui::PushID(i);
            const bool selected = i == anchor;
            if (ImGui::Selectable(v.model.bones[static_cast<std::size_t>(i)].name.c_str(),
                                  selected) &&
                !selected) {
                pick = i;
            }
            if (selected) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
        if (pick >= 0) {
            v.accessory_bone = pick;
            v.apply_accessory_structure();
        }
    }

    ImGui::SeparatorText("摆放（相对静止姿态）");
    bool changed = false;
    changed |= ImGui::DragFloat3("位置", v.accessory_pos, 0.01f, 0.0f, 0.0f, "%.3f");
    ImGui::SetItemTooltip("世界坐标下的偏移；拖动时只更新附件顶点，不重建模型");
    changed |= ImGui::DragFloat3("旋转", v.accessory_rot, 0.5f, 0.0f, 0.0f, "%.1f°");
    float scale = v.accessory_scale;
    if (ImGui::DragFloat("缩放", &scale, 0.005f, 0.01f, 100.0f, "%.3f")) {
        v.accessory_scale = scale;
        // 缩放需要烘焙进附件几何 / 骨骼 / 刚体，重建运行时不重传贴图。
        v.rebuild_accessory_scaled();
        changed = true;
    }
    if (changed) v.update_accessory_transform();

    if (ImGui::Button("重置变换")) {
        v.accessory_rot[0] = v.accessory_rot[1] = v.accessory_rot[2] = 0.0f;
        v.accessory_scale = 1.0f;
        v.rebuild_accessory_scaled();
        if (anchor >= 0) {
            const pmx::Vec3 p = v.model.bones[static_cast<std::size_t>(anchor)].pos;
            v.accessory_pos[0] = p.x;
            v.accessory_pos[1] = p.y;
            v.accessory_pos[2] = p.z;
        } else {
            v.accessory_pos[0] = v.accessory_pos[1] = v.accessory_pos[2] = 0.0f;
        }
        v.update_accessory_transform();
    }
    ImGui::SameLine();
    if (ImGui::Button("移除附件")) v.clear_accessory();
    if (!v.accessory.rigid.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("重置附件物理")) v.accessory_reset = true;
    }
}
