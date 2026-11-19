// 设置页：环境亮度与颜色、叠加层透明度、物理参数与快捷键。显示开关与主题统一放在「视图」菜单。
#include "app/panels.hpp"

#include <string>

#include <imgui.h>

#include "app/panels/common.hpp"
#include "app/profiler.hpp"
#include "app/theme.hpp"

void panel_settings(Viewer& v) {
    ImGui::BeginChild("##settings_scroll", ImVec2(0, 0));

    ImGui::TextDisabled("显示开关、物理开关与界面主题见顶部「视图」菜单。");

    // ------------------------------------------------------------ 环境与亮度
    ImGui::SeparatorText("环境与亮度");
    {
        bool bg_follow = !v.bg_custom;
        if (ImGui::Checkbox("背景色跟随主题##bg_follow", &bg_follow)) {
            v.bg_custom = !bg_follow;
            if (bg_follow) pmxtheme::apply_scene_theme(v);
        }
        ImGui::SetItemTooltip("关闭后使用自定义背景色，切换深色 / 亮色主题不再覆盖");
        ImGui::SetNextItemWidth(pmxui::scale(-90.0f));
        if (ImGui::ColorEdit3("背景色##bg_color", v.bg, ImGuiColorEditFlags_Float))
            v.bg_custom = true;
        ImGui::SetNextItemWidth(pmxui::scale(-90.0f));
        ImGui::SliderFloat("背景亮度##bg_brightness", &v.bg_brightness, 0.2f, 3.0f, "%.2f×");
        ImGui::SetItemTooltip("压暗或提亮视口底色；提亮时向白色过渡，亮色主题更容易看清暗部");
        ImGui::SetNextItemWidth(pmxui::scale(-90.0f));
        ImGui::SliderFloat("模型亮度##model_brightness", &v.model_brightness, 0.2f, 2.2f,
                           "%.2f×");
        ImGui::SetItemTooltip("乘在材质漫反射上，只影响显示与渲染输出，不写入 PMX");

        bool grid_follow = !v.grid_custom;
        if (ImGui::Checkbox("地面网格跟随主题##grid_follow", &grid_follow)) {
            v.grid_custom = !grid_follow;
            if (grid_follow) pmxtheme::apply_scene_theme(v);
        }
        if (ImGui::TreeNode("地面网格颜色##grid_colors")) {
            ImGui::SetNextItemWidth(pmxui::scale(-90.0f));
            if (ImGui::ColorEdit3("次线##grid_minor", v.grid_minor, ImGuiColorEditFlags_Float))
                v.grid_custom = true;
            ImGui::SetNextItemWidth(pmxui::scale(-90.0f));
            if (ImGui::ColorEdit3("主线##grid_major", v.grid_major, ImGuiColorEditFlags_Float))
                v.grid_custom = true;
            ImGui::SetNextItemWidth(pmxui::scale(-90.0f));
            if (ImGui::ColorEdit3("X 轴##grid_axis_x", v.grid_axis_x,
                                  ImGuiColorEditFlags_Float))
                v.grid_custom = true;
            ImGui::SetNextItemWidth(pmxui::scale(-90.0f));
            if (ImGui::ColorEdit3("Z 轴##grid_axis_z", v.grid_axis_z,
                                  ImGuiColorEditFlags_Float))
                v.grid_custom = true;
            ImGui::TreePop();
        }
    }

    // ------------------------------------------------------------ 叠加层透明度
    ImGui::SeparatorText("叠加层不透明度");
    ImGui::SetNextItemWidth(pmxui::scale(-90.0f));
    ImGui::SliderFloat("骨骼##bone_alpha", &v.bone_alpha, 0.0f, 1.0f, "%.2f");
    ImGui::SetNextItemWidth(pmxui::scale(-90.0f));
    ImGui::SliderFloat("刚体##rigid_alpha", &v.rigid_alpha, 0.0f, 1.0f, "%.2f");
    ImGui::SetNextItemWidth(pmxui::scale(-90.0f));
    ImGui::SliderFloat("关节##joint_alpha", &v.joint_alpha, 0.0f, 1.0f, "%.2f");

    // ------------------------------------------------------------ 卡通着色
    ImGui::SeparatorText("卡通着色");
    ImGui::SetNextItemWidth(pmxui::scale(-90.0f));
    ImGui::SliderFloat("边缘光##rim", &v.rim, 0.0f, 0.6f, "%.2f");
    ImGui::SetItemTooltip("沿轮廓叠加本体色的强度；0 为关闭，接近 MMD 原版效果");
    ImGui::SetNextItemWidth(pmxui::scale(-90.0f));
    ImGui::SliderFloat("明暗分界##toon_edge", &v.toon_edge, 0.02f, 0.45f, "%.2f");
    ImGui::SetItemTooltip("卡通阴影过渡宽度：越小越接近 MMD 的硬边，越大越像渐变");

    // ------------------------------------------------------------ Bullet 物理
    ImGui::SeparatorText("Bullet 物理");
    ImGui::SetNextItemWidth(pmxui::scale(-90.0f));
    ImGui::DragFloat("重力", &v.gravity, 0.1f, -100.0f, 100.0f, "%.1f");
    ImGui::SetNextItemWidth(pmxui::scale(-90.0f));
    ImGui::DragInt("迭代次数", &v.iterations, 1.0f, 1, 60);
    ImGui::Checkbox("显示地面", &v.ground);
    if (ImGui::Button("应用物理参数")) {
        v.reset_physics();
        pmxtheme::save_settings(v);
    }
    ImGui::SetItemTooltip("按当前重力 / 迭代次数重建物理世界，并写入配置文件");

    // ------------------------------------------------------------ 配置文件
    ImGui::SeparatorText("配置文件");
    ImGui::TextWrapped("%s", pmxtheme::settings_path().c_str());
    if (ImGui::Button("重新载入文件")) {
        std::string err;
        const bool loaded = pmxtheme::load_settings(v, &err);
        pmxtheme::apply_theme(v.theme);
        pmxtheme::apply_scene_theme(v);
        if (!err.empty()) v.error = err;
        else if (loaded) v.status = "设置已重新载入";
    }
    ImGui::TextDisabled("文件不存在时使用默认值。");

    // ------------------------------------------------------------ 性能
    ImGui::SeparatorText("性能");
    {
        const pmx::Profiler& profiler = pmx::Profiler::instance();
        const pmx::ProfileScope scopes[] = {pmx::ProfileScope::Frame, pmx::ProfileScope::Physics,
                                            pmx::ProfileScope::Ik, pmx::ProfileScope::Skin,
                                            pmx::ProfileScope::Ui, pmx::ProfileScope::Io};
        for (pmx::ProfileScope scope : scopes) {
            ImGui::Text("%s：%.2f ms", profiler.name(scope),
                        profiler.milliseconds(scope));
        }
        ImGui::TextDisabled("滑动平均，用于判断瓶颈在物理、IK、蒙皮还是界面。");
    }

    // ------------------------------------------------------------ 快捷键
    ImGui::SeparatorText("快捷键");
    if (ImGui::BeginTable("##settings_shortcuts", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("##key", ImGuiTableColumnFlags_WidthFixed,
                                pmxui::scale(150.0f));
        ImGui::TableSetupColumn("##desc", ImGuiTableColumnFlags_WidthStretch);
        struct Shortcut {
            const char* key;
            const char* desc;
        };
        const Shortcut shortcuts[] = {
            {"Ctrl+O", "打开模型"},
            {"Ctrl+M", "切换编辑 / 预览模式"},
            {"Ctrl+F", "预览模式：检索骨骼 / 刚体 / 关节"},
            {"Ctrl+T", "预览模式：展开 / 收起时间线"},
            {"K", "时间线：当前姿态打关键帧（相机视角下记录相机帧）"},
            {"0 / 小键盘 0", "预览模式：进入 / 退出相机视角"},
            {"Ctrl+S", "保存模型（编辑）/ 保存动作（预览）"},
            {"Ctrl+Shift+S", "模型另存为（仅编辑模式）"},
            {"Ctrl+Z", "撤销"},
            {"Ctrl+Y", "重做"},
            {"Ctrl+Shift+Z", "重做"},
            {"F", "视图全部"},
            {"R", "重置姿态"},
            {"P", "物理开关"},
            {"双击", "选择骨骼 / 刚体 / 关节"},
            {"左键拖动骨骼", "位移（仅预览模式）"},
            {"右键拖动骨骼", "旋转（仅预览模式）"},
            {"中键拖动", "旋转视图"},
            {"Shift+中键拖动", "平移视图"},
            {"滚轮", "缩放"},
        };
        for (const Shortcut& shortcut : shortcuts) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(shortcut.key);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", shortcut.desc);
        }
        ImGui::EndTable();
    }

    ImGui::EndChild();
}
