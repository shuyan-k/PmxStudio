// 接触式肉体变形：自接触凹陷 / 周边隆起 / 区域软硬 / Corner Taper /
// 外部碰撞体 / 烘焙缓存。只影响视图与渲染输出。
#include "app/panels.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

#include <imgui.h>

#include "app/viewer.hpp"

namespace {

const std::string& bone_label(const pmx::Bone& bone) {
    return bone.name.empty() ? bone.name_en : bone.name;
}

void help(const char* text) {
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

// 骨骼下拉：-1 表示“无”，其余为骨骼索引。
void bone_combo(const char* label, const pmx::Model& model, int& value) {
    const int count = static_cast<int>(model.bones.size());
    if (value >= count) value = -1;
    const char* current =
        value >= 0 ? bone_label(model.bones[static_cast<std::size_t>(value)]).c_str() : "（无）";
    if (ImGui::BeginCombo(label, current)) {
        if (ImGui::Selectable("（无）", value < 0)) value = -1;
        for (int i = 0; i < count; ++i) {
            const bool selected = value == i;
            const auto& bone = model.bones[static_cast<std::size_t>(i)];
            const std::string& name = bone_label(bone);
            if (ImGui::Selectable(name.empty() ? "（未命名骨骼）" : name.c_str(), selected))
                value = i;
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

int motion_max_frame(const Viewer& v) {
    if (v.timeline.clip) return v.timeline.model_max_frame();
    if (v.motion) return std::max(0, v.motion->max_frame);
    return 0;
}

float model_height(const pmx::Model& model) {
    float lo = 1e30f;
    float hi = -1e30f;
    for (const pmx::Vec3& p : model.vertices.pos) {
        lo = std::min(lo, p.y);
        hi = std::max(hi, p.y);
    }
    return hi > lo ? hi - lo : 1.0f;
}

bool has_action(const Viewer& v) {
    return v.motion != nullptr || v.timeline.clip != nullptr;
}

void draw_colliders(Viewer& v) {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("外部碰撞体");
    ImGui::TextWrapped(
        "挂在骨骼上的球 / 胶囊会随动作压入肉体：陷进去的顶点被推到碰撞体表面，"
        "周围一圈轻微外翻。每颗碰撞体有独立硬度，可用来做手肘、膝盖、胸腹的挤压。");

    const int bone_count = static_cast<int>(v.model.bones.size());
    int remove = -1;
    for (std::size_t i = 0; i < v.flesh_colliders.size(); ++i) {
        pmx::FleshColliderSpec& c = v.flesh_colliders[i];
        if (c.bone >= bone_count) c.bone = -1;
        ImGui::PushID(static_cast<int>(i));
        char header[64];
        std::snprintf(header, sizeof(header), "碰撞体 %d", static_cast<int>(i) + 1);
        if (ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) {
            const char* shapes[] = {"球", "胶囊"};
            int shape = std::clamp(c.shape, 0, 1);
            if (ImGui::Combo("形状", &shape, shapes, 2)) c.shape = shape;
            bone_combo("骨骼", v.model, c.bone);
            ImGui::DragFloat3("局部偏移", &c.offset.x, 0.01f);
            help("相对骨骼原点的平移，随骨骼旋转 / 缩放。");
            if (c.shape == 1) {
                ImGui::DragFloat("长度", &c.length, 0.01f, 0.0f, 1000.0f, "%.3f");
                help("胶囊沿骨骼局部 +Y 伸展的长度。");
            }
            ImGui::DragFloat("半径", &c.radius, 0.005f, 0.001f, 1000.0f, "%.3f");
            ImGui::SliderFloat("硬度", &c.firmness, 0.0f, 1.0f, "%.2f");
            if (ImGui::Button("删除这个碰撞体")) remove = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    if (remove >= 0)
        v.flesh_colliders.erase(v.flesh_colliders.begin() + remove);
    if (ImGui::Button("添加碰撞体")) {
        pmx::FleshColliderSpec c;
        c.bone = bone_count > 0 ? 0 : -1;
        c.radius = std::max(0.05f, model_height(v.model) * 0.03f);
        v.flesh_colliders.push_back(c);
    }
    ImGui::SameLine();
    ImGui::Checkbox("显示碰撞体", &v.flesh_colliders_visible);
}

void draw_bake(Viewer& v) {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("烘焙缓存与 Smooth Pops");
    ImGui::TextWrapped(
        "烘焙把每一帧的蒙皮 + 肉体结果缓存下来，播放时直接复用并做时间平滑，"
        "避免接触状态翻转产生的单帧跳变。缓存只在参数、形态与静止坐标一致时使用。");

    ImGui::Checkbox("启用烘焙缓存", &v.flesh_cache);
    help("播放到已烘焙的整数帧时直接使用缓存；相邻帧之间线性插值。");
    ImGui::SliderFloat("Smooth Pops", &v.flesh_smooth, 0.0f, 0.9f, "%.2f");
    help("对连续帧的肉体位移做指数平滑；0 关闭，越大越柔和但跟随动作略慢。");

    ImGui::Text("已缓存 %d 帧，%.1f MB", v.renderer.flesh_cache_frames(),
                static_cast<double>(v.renderer.flesh_cache_bytes()) / (1024.0 * 1024.0));

    ImGui::BeginDisabled(!has_action(v) || !v.preview_mode || v.flesh_bake_active);
    if (ImGui::Button("烘焙当前动作")) {
        v.flesh_bake_active = true;
        v.flesh_bake_next = 0;
        v.flesh_bake_end = motion_max_frame(v);
        v.flesh_bake_saved_frame = v.motion_frame;
        v.flesh_bake_saved_playing = v.motion_playing;
        v.motion_playing = false;
        v.renderer.clear_flesh_cache();
        v.status = "开始烘焙肉体…";
    }
    ImGui::EndDisabled();
    if (!has_action(v))
        ImGui::TextDisabled("先导入动作（VMD）才能烘焙。");
    else if (!v.preview_mode)
        ImGui::TextDisabled("切换到预览模式后才能烘焙。");
    ImGui::SameLine();
    ImGui::BeginDisabled(v.flesh_bake_active || v.renderer.flesh_cache_frames() == 0);
    if (ImGui::Button("清除缓存")) {
        v.renderer.clear_flesh_cache();
        v.status = "已清除肉体烘焙缓存";
    }
    ImGui::EndDisabled();

    if (v.flesh_bake_active) {
        const int total = std::max(1, v.flesh_bake_end + 1);
        const float progress =
            std::clamp(static_cast<float>(v.flesh_bake_next) / static_cast<float>(total), 0.0f,
                       1.0f);
        char overlay[64];
        std::snprintf(overlay, sizeof(overlay), "%d / %d", v.flesh_bake_next,
                      v.flesh_bake_end + 1);
        ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f), overlay);
        if (ImGui::Button("取消烘焙")) {
            v.flesh_bake_active = false;
            v.motion_frame = v.flesh_bake_saved_frame;
            v.motion_playing = v.flesh_bake_saved_playing;
            v.motion_dirty = true;
            v.status = "已取消肉体烘焙";
        }
        // 每帧最多占用约 15 ms，拖慢但不至于让界面卡死。
        const double deadline = ImGui::GetTime() + 0.015;
        std::string error;
        while (v.flesh_bake_next <= v.flesh_bake_end) {
            if (!v.bake_flesh_frame(v.flesh_bake_next, &error)) {
                v.flesh_bake_active = false;
                v.motion_frame = v.flesh_bake_saved_frame;
                v.motion_playing = v.flesh_bake_saved_playing;
                v.motion_dirty = true;
                v.status = "肉体烘焙失败：" + error;
                break;
            }
            ++v.flesh_bake_next;
            if (ImGui::GetTime() >= deadline) break;
        }
        if (v.flesh_bake_active && v.flesh_bake_next > v.flesh_bake_end) {
            v.flesh_bake_active = false;
            v.motion_frame = v.flesh_bake_saved_frame;
            v.motion_playing = v.flesh_bake_saved_playing;
            v.motion_dirty = true;
            v.status = "肉体烘焙完成：" + std::to_string(v.renderer.flesh_cache_frames()) + " 帧";
        }
    }
}

}  // namespace

void panel_flesh(Viewer& v) {
    ImGui::TextWrapped(
        "弯曲关节时自动检测网格自身的挤压：接触处压出凹陷，周围一圈向外隆起。"
        "默认逐帧求解、拖动时间轴结果一致；也可烘焙缓存并做 Smooth Pops 时间平滑。");
    ImGui::Spacing();

    ImGui::Checkbox("启用肉体接触变形", &v.flesh);
    ImGui::BeginDisabled(!v.flesh);

    ImGui::Checkbox("GPU 求解", &v.flesh_gpu);
    help("60k 级以上顶点推荐开启：拓扑一次上传，逐帧只在 GPU 求解，无 CPU 回读。");
    if (!v.flesh_gpu_available)
        ImGui::TextDisabled("GPU 求解不可用，已回退 CPU 多线程。");
    else if (v.flesh && v.flesh_gpu && !v.flesh_gpu_active)
        ImGui::TextDisabled("本帧未走 GPU（烘焙缓存命中或正在回退 CPU）。");

    float pct = v.flesh_range * 100.0f;
    if (ImGui::SliderFloat("接触距离", &pct, 0.2f, 8.0f, "%.2f%% 身高"))
        v.flesh_range = pct / 100.0f;
    help("小于该距离且法线相对的两处表面视为自接触；过大容易把薄衣物误判为接触。");

    ImGui::SliderFloat("凹陷深度", &v.flesh_dent, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("隆起强度", &v.flesh_bulge, 0.0f, 2.0f, "%.2f");
    ImGui::SliderFloat("隆起范围", &v.flesh_width, 1.0f, 8.0f, "%.1f 倍接触距离");
    ImGui::SliderFloat("整体硬度", &v.flesh_firmness, 0.0f, 1.0f, "%.2f");
    help("1 = 完全不形变，可用于整体收住效果。");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Corner Taper（折痕端回收）");
    ImGui::TextWrapped(
        "折痕闭合的端部只朝一侧受压，隆起容易鼓出尖角；这里按端部程度回收隆起。");
    ImGui::SliderFloat("回收强度", &v.flesh_taper, 0.0f, 1.0f, "%.2f");
    help("0 = 关闭；1 = 折痕端部完全不留隆起。");
    ImGui::SliderFloat("回收范围", &v.flesh_taper_reach, 0.05f, 3.0f, "%.2f 倍隆起半径");
    ImGui::SliderFloat("过渡曲线", &v.flesh_taper_falloff, 0.1f, 4.0f, "%.2f");
    help("越大越集中在端点，越小过渡越平缓。");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("区域软硬");
    ImGui::TextWrapped("指定一根骨骼：受它影响的顶点额外变硬，模拟骨骼附近的紧实部位。");

    bone_combo("硬区域骨骼", v.model, v.flesh_region_bone);
    ImGui::BeginDisabled(v.flesh_region_bone < 0);
    ImGui::SliderFloat("该区域硬度", &v.flesh_region_firmness, 0.0f, 1.0f, "%.2f");
    ImGui::EndDisabled();

    draw_colliders(v);
    draw_bake(v);

    ImGui::Spacing();
    ImGui::TextDisabled("GPU 求解走 GPU 蒙皮；回退 CPU 时蒙皮改走 CPU 路径，超大模型预览可能变慢。");
    ImGui::EndDisabled();
}
