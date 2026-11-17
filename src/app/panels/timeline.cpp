// 检查器「时间线」标签：四类轨道树、关键帧列表与缓动曲线编辑。
#include "app/timeline.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include <imgui.h>

#include "app/panels/common.hpp"
#include "app/ui_scale.hpp"
#include "app/viewer.hpp"
#include "core/anim.hpp"
#include "core/name_zh.hpp"

namespace {

constexpr float kRadToDeg = 57.29577951308232f;

// 面板自己的选择状态：TimelineState 只记录骨骼轨的活动下标。
TimelineTrackKind g_kind = TimelineTrackKind::Bone;
int g_track = -1;
int g_key = -1;
int g_axis = 3;
int g_drag_control = -1;

int track_total(const Viewer& v, TimelineTrackKind kind) {
    const pmx::AnimClip* clip = v.timeline.clip.get();
    switch (kind) {
        case TimelineTrackKind::Bone:
            return clip ? static_cast<int>(clip->bones.size()) : 0;
        case TimelineTrackKind::Morph:
            return clip ? static_cast<int>(clip->morphs.size()) : 0;
        case TimelineTrackKind::Camera:
            return 1;
        case TimelineTrackKind::Ik:
            return clip ? 1 : 0;
    }
    return 0;
}

int track_key_count(const Viewer& v, TimelineTrackKind kind, int track) {
    if (track < 0 || track >= track_total(v, kind)) return 0;
    const pmx::AnimClip* clip = v.timeline.clip.get();
    switch (kind) {
        case TimelineTrackKind::Bone:
            return static_cast<int>(clip->bones[track].keys.size());
        case TimelineTrackKind::Morph:
            return static_cast<int>(clip->morphs[track].keys.size());
        case TimelineTrackKind::Camera:
            return static_cast<int>(v.camera_motion.track.keys.size());
        case TimelineTrackKind::Ik:
            return static_cast<int>(clip->ik.size());
    }
    return 0;
}

std::string track_name(const Viewer& v, TimelineTrackKind kind, int track,
                       std::string* name_en) {
    name_en->clear();
    switch (kind) {
        case TimelineTrackKind::Bone: {
            const pmx::AnimBoneTrack& t = v.timeline.clip->bones[track];
            if (t.bone >= 0 && t.bone < static_cast<int>(v.model.bones.size())) {
                *name_en = v.model.bones[t.bone].name_en;
                return v.model.bones[t.bone].name;
            }
            return t.name;
        }
        case TimelineTrackKind::Morph: {
            const pmx::AnimMorphTrack& t = v.timeline.clip->morphs[track];
            if (t.morph >= 0 && t.morph < static_cast<int>(v.model.morphs.size())) {
                *name_en = v.model.morphs[t.morph].name_en;
                return v.model.morphs[t.morph].name;
            }
            return t.name;
        }
        case TimelineTrackKind::Camera:
            return "相机动作";
        case TimelineTrackKind::Ik:
            return "IK 开关轨";
    }
    return std::string();
}

pmx::NameKind name_kind(TimelineTrackKind kind) {
    return kind == TimelineTrackKind::Morph ? pmx::NameKind::Morph : pmx::NameKind::Bone;
}

std::string display_text(const Viewer& v, TimelineTrackKind kind, int track) {
    std::string name_en;
    const std::string name = track_name(v, kind, track, &name_en);
    std::string text = pmx::name_with_zh(name_kind(kind), name, name_en);
    if (text.empty()) text = "未命名";
    return text;
}

const char* kind_tag(TimelineTrackKind kind) {
    switch (kind) {
        case TimelineTrackKind::Bone:
            return "骨骼";
        case TimelineTrackKind::Morph:
            return "形态";
        case TimelineTrackKind::Camera:
            return "相机";
        case TimelineTrackKind::Ik:
            return "IK";
    }
    return "";
}

void select_track(Viewer& v, TimelineTrackKind kind, int track) {
    g_kind = kind;
    g_track = track;
    g_key = -1;
    v.timeline.active_key = -1;
    v.timeline.active_track = kind == TimelineTrackKind::Bone ? track : -1;
}

// 每个轨道树里的一行。filter 同时匹配原名 / 英文名 / 中文译名。
void track_row(Viewer& v, TimelineTrackKind kind, int track, const std::string& text) {
    ImGui::PushID(track);
    const bool selected = g_kind == kind && g_track == track;
    if (ImGui::Selectable(text.c_str(), selected)) select_track(v, kind, track);
    ImGui::PopID();
}

void draw_bone_tree(Viewer& v, pmx::AnimClip& clip, const std::string& filter) {
    if (!ImGui::TreeNodeEx("骨骼##timeline_tree_bones",
                           ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth))
        return;
    int shown = 0;
    for (int i = 0; i < static_cast<int>(clip.bones.size()); ++i) {
        const pmx::AnimBoneTrack& t = clip.bones[i];
        std::string name = t.name;
        std::string name_en;
        if (t.bone >= 0 && t.bone < static_cast<int>(v.model.bones.size())) {
            name = v.model.bones[t.bone].name;
            name_en = v.model.bones[t.bone].name_en;
        }
        const std::string zh = pmx::name_zh(pmx::NameKind::Bone, name, name_en);
        if (!pmxui::match_filter(filter, name, name_en, zh)) continue;
        ++shown;
        std::string text = display_text(v, TimelineTrackKind::Bone, i) + "  (" +
                           std::to_string(t.keys.size()) + ")";
        if (t.bone < 0) text += "（未绑定）";
        track_row(v, TimelineTrackKind::Bone, i, text);
    }
    if (shown == 0) ImGui::TextDisabled("无匹配骨骼轨道");
    ImGui::TreePop();
}

void draw_morph_tree(Viewer& v, pmx::AnimClip& clip, const std::string& filter) {
    if (!ImGui::TreeNodeEx("形态##timeline_tree_morphs",
                           ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth))
        return;
    int shown = 0;
    for (int i = 0; i < static_cast<int>(clip.morphs.size()); ++i) {
        const pmx::AnimMorphTrack& t = clip.morphs[i];
        std::string name = t.name;
        std::string name_en;
        if (t.morph >= 0 && t.morph < static_cast<int>(v.model.morphs.size())) {
            name = v.model.morphs[t.morph].name;
            name_en = v.model.morphs[t.morph].name_en;
        }
        const std::string zh = pmx::name_zh(pmx::NameKind::Morph, name, name_en);
        if (!pmxui::match_filter(filter, name, name_en, zh)) continue;
        ++shown;
        std::string text = display_text(v, TimelineTrackKind::Morph, i) + "  (" +
                           std::to_string(t.keys.size()) + ")";
        if (t.morph < 0) text += "（未绑定）";
        track_row(v, TimelineTrackKind::Morph, i, text);
    }
    if (shown == 0) ImGui::TextDisabled("无匹配形态轨道");
    ImGui::TreePop();
}

void draw_camera_tree(Viewer& v, const std::string& filter) {
    if (!ImGui::TreeNodeEx("相机##timeline_tree_camera",
                           ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth))
        return;
    if (!pmxui::match_filter(filter, "相机", "Camera", "相机")) {
        ImGui::TextDisabled("无匹配相机轨道");
        ImGui::TreePop();
        return;
    }
    std::string text = "相机动作（独立）  (" +
                       std::to_string(v.camera_motion.track.keys.size()) + ")";
    track_row(v, TimelineTrackKind::Camera, 0, text);
    if (v.camera_motion.track.empty()) {
        ImGui::TextDisabled("没有相机关键帧");
        ImGui::TextDisabled("在「相机」页导入相机 VMD，或记录当前视角");
    }
    ImGui::TreePop();
}

void draw_ik_tree(Viewer& v, pmx::AnimClip& clip, const std::string& filter) {
    if (!ImGui::TreeNodeEx("IK##timeline_tree_ik",
                           ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth))
        return;
    if (!pmxui::match_filter(filter, "IK", "IK", "IK")) {
        ImGui::TextDisabled("无匹配 IK 轨道");
        ImGui::TreePop();
        return;
    }
    std::string text = "IK 开关轨  (" + std::to_string(clip.ik.size()) + ")";
    track_row(v, TimelineTrackKind::Ik, 0, text);
    if (clip.ik.empty()) ImGui::TextDisabled("没有 IK 开关帧");
    ImGui::TreePop();
}

std::string key_text(const Viewer& v, TimelineTrackKind kind, int track, int key) {
    const pmx::AnimClip* clip = v.timeline.clip.get();
    char buffer[256];
    switch (kind) {
        case TimelineTrackKind::Bone: {
            const pmx::AnimBoneKey& k = clip->bones[track].keys[key];
            float rx = 0.0f, ry = 0.0f, rz = 0.0f;
            pmx::quat_to_euler(k.rot, rx, ry, rz);
            std::snprintf(buffer, sizeof buffer,
                          "帧 %d · 位移 (%.3f, %.3f, %.3f) · 旋转 (%.1f°, %.1f°, %.1f°)",
                          k.frame, k.pos.x, k.pos.y, k.pos.z, rx * kRadToDeg, ry * kRadToDeg,
                          rz * kRadToDeg);
            return buffer;
        }
        case TimelineTrackKind::Morph: {
            const pmx::AnimMorphKey& k = clip->morphs[track].keys[key];
            std::snprintf(buffer, sizeof buffer, "帧 %d · 权重 %.3f", k.frame, k.weight);
            return buffer;
        }
        case TimelineTrackKind::Camera: {
            const pmx::AnimCameraKey& k = v.camera_motion.track.keys[key];
            std::snprintf(buffer, sizeof buffer, "帧 %d · 距离 %.1f · FOV %d", k.frame,
                          k.distance, k.fov);
            return buffer;
        }
        case TimelineTrackKind::Ik: {
            const pmx::AnimIkKey& k = clip->ik[key];
            std::snprintf(buffer, sizeof buffer, "帧 %d · %d 条链%s", k.frame,
                          static_cast<int>(k.states.size()), k.visible ? "" : "（隐藏）");
            return buffer;
        }
    }
    return std::string();
}

void draw_curve_editor(Viewer& v) {
    ImGui::SeparatorText("曲线");
    if (g_kind == TimelineTrackKind::Morph || g_kind == TimelineTrackKind::Ik) {
        ImGui::TextDisabled("形态 / IK 关键帧为线性插值，没有缓动曲线。");
        return;
    }
    if (g_track < 0 || g_key < 0 ||
        g_key >= track_key_count(v, g_kind, g_track)) {
        ImGui::TextDisabled("未选择关键帧（在上方列表点选一帧）");
        return;
    }

    pmx::AnimClip* clip = v.timeline.clip.get();
    pmx::VmdCurve* curve = nullptr;
    if (g_kind == TimelineTrackKind::Bone && clip)
        curve = &clip->bones[g_track].keys[g_key].curve[g_axis];
    else if (g_kind == TimelineTrackKind::Camera)
        curve = &v.camera_motion.track.keys[g_key].curve[g_axis];
    if (!curve) return;

    // 相机关键帧属于独立相机动作，编辑后标相机脏而不是模型动作。
    const bool camera_curve = g_kind == TimelineTrackKind::Camera;

    const char* axes[4] = {"X 轴", "Y 轴", "Z 轴", "旋转"};
    ImGui::SetNextItemWidth(pmxui::scale(110.0f));
    ImGui::Combo("缓动轴##timeline_curve_axis", &g_axis, axes, 4);

    auto preset = [&](float x1, float y1, float x2, float y2) {
        curve->x1 = x1;
        curve->y1 = y1;
        curve->x2 = x2;
        curve->y2 = y2;
        if (camera_curve) v.camera_motion.dirty = true;
        else v.motion_dirty_edit = true;
    };
    if (ImGui::Button("线性##timeline_curve_linear")) preset(0.0f, 0.0f, 1.0f, 1.0f);
    ImGui::SameLine();
    if (ImGui::Button("缓入##timeline_curve_in")) preset(0.42f, 0.0f, 1.0f, 1.0f);
    ImGui::SameLine();
    if (ImGui::Button("缓出##timeline_curve_out")) preset(0.0f, 0.0f, 0.58f, 1.0f);
    ImGui::SameLine();
    if (ImGui::Button("缓入缓出##timeline_curve_inout")) preset(0.42f, 0.0f, 0.58f, 1.0f);

    const float available = ImGui::GetContentRegionAvail().x;
    const float side = std::clamp(pmxui::scale(180.0f), pmxui::scale(100.0f),
                                  std::max(pmxui::scale(100.0f), available));
    ImGui::InvisibleButton("##timeline_curve_box", ImVec2(side, side),
                           ImGuiButtonFlags_MouseButtonLeft);
    const ImVec2 box_min = ImGui::GetItemRectMin();
    const ImVec2 box_max = ImGui::GetItemRectMax();
    const float draw_side = std::min(box_max.x - box_min.x, box_max.y - box_min.y);

    auto to_px = [&](float x, float y) {
        return ImVec2(box_min.x + x * draw_side, box_min.y + (1.0f - y) * draw_side);
    };

    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemActivated()) {
        const ImVec2 p0 = to_px(curve->x1, curve->y1);
        const ImVec2 p1 = to_px(curve->x2, curve->y2);
        const float d0 = std::hypot(io.MousePos.x - p0.x, io.MousePos.y - p0.y);
        const float d1 = std::hypot(io.MousePos.x - p1.x, io.MousePos.y - p1.y);
        const float reach = draw_side * 0.15f;
        g_drag_control = d0 <= d1 ? 0 : 1;
        if (std::min(d0, d1) > reach) g_drag_control = -1;
    }
    if (ImGui::IsItemActive() && g_drag_control >= 0) {
        const float nx = std::clamp((io.MousePos.x - box_min.x) / draw_side, 0.0f, 1.0f);
        const float ny =
            std::clamp(1.0f - (io.MousePos.y - box_min.y) / draw_side, 0.0f, 1.0f);
        if (g_drag_control == 0) {
            curve->x1 = nx;
            curve->y1 = ny;
        } else {
            curve->x2 = nx;
            curve->y2 = ny;
        }
        if (camera_curve) v.camera_motion.dirty = true;
        else v.motion_dirty_edit = true;
    }
    if (ImGui::IsItemDeactivated()) g_drag_control = -1;

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(box_min, box_max, IM_COL32(24, 25, 29, 255));
    for (int i = 1; i < 4; ++i) {
        const float t = static_cast<float>(i) / 4.0f;
        draw->AddLine(ImVec2(box_min.x + t * draw_side, box_min.y),
                      ImVec2(box_min.x + t * draw_side, box_max.y),
                      IM_COL32(48, 50, 56, 255), 1.0f);
        draw->AddLine(ImVec2(box_min.x, box_min.y + t * draw_side),
                      ImVec2(box_max.x, box_min.y + t * draw_side),
                      IM_COL32(48, 50, 56, 255), 1.0f);
    }
    draw->AddRect(box_min, box_max, IM_COL32(82, 86, 96, 255));

    // 曲线预览：横轴时间（0..1），纵轴权重（0..1）。
    ImVec2 previous = to_px(0.0f, 0.0f);
    const int steps = 32;
    for (int i = 1; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float mt = 1.0f - t;
        const float bx = 3.0f * mt * mt * t * curve->x1 + 3.0f * mt * t * t * curve->x2 +
                         t * t * t;
        const float by = 3.0f * mt * mt * t * curve->y1 + 3.0f * mt * t * t * curve->y2 +
                         t * t * t;
        const ImVec2 point = to_px(bx, by);
        draw->AddLine(previous, point, IM_COL32(120, 200, 255, 255), pmxui::scale(1.5f));
        previous = point;
    }

    const ImVec2 p0 = to_px(curve->x1, curve->y1);
    const ImVec2 p1 = to_px(curve->x2, curve->y2);
    draw->AddLine(to_px(0.0f, 0.0f), p0, IM_COL32(255, 190, 90, 180), 1.0f);
    draw->AddLine(to_px(1.0f, 1.0f), p1, IM_COL32(120, 200, 255, 180), 1.0f);
    draw->AddCircleFilled(p0, pmxui::scale(4.0f), IM_COL32(255, 190, 90, 255));
    draw->AddCircleFilled(p1, pmxui::scale(4.0f), IM_COL32(120, 200, 255, 255));
    draw->AddText(ImVec2(p0.x + pmxui::scale(5.0f), p0.y - pmxui::scale(14.0f)),
                  ImGui::GetColorU32(ImGuiCol_TextDisabled), "1");
    draw->AddText(ImVec2(p1.x + pmxui::scale(5.0f), p1.y + pmxui::scale(2.0f)),
                  ImGui::GetColorU32(ImGuiCol_TextDisabled), "2");
    ImGui::TextDisabled("方框内拖动控制点 1 / 2 调整缓动。");
}

}  // namespace

void panel_timeline(Viewer& v) {
    if (!v.pose) {
        ImGui::TextDisabled("未加载模型");
        return;
    }
    pmx::AnimClip* clip = v.timeline.clip.get();
    if (!clip && !v.camera_motion.loaded()) {
        ImGui::TextDisabled("尚未载入模型动作或相机动作。");
        ImGui::TextDisabled("导入 VMD 会自动拆分：骨骼 / 形态进「动作」，相机进独立「相机动作」。");
        return;
    }

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##timeline_filter", "搜索轨道名 / 中文", &v.timeline.filter);

    const int total = track_total(v, g_kind);
    if (g_track >= total) {
        g_track = -1;
        g_key = -1;
    }
    if (g_key >= track_key_count(v, g_kind, g_track)) g_key = -1;

    ImGui::BeginChild("##timeline_panel_tracks", ImVec2(0.0f, pmxui::scale(190.0f)),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY);
    if (clip) {
        draw_bone_tree(v, *clip, v.timeline.filter);
        draw_morph_tree(v, *clip, v.timeline.filter);
    }
    draw_camera_tree(v, v.timeline.filter);
    if (clip) draw_ik_tree(v, *clip, v.timeline.filter);
    ImGui::EndChild();

    ImGui::SeparatorText("关键帧");
    if (g_track < 0) {
        ImGui::TextDisabled("在上方轨道树中选择一条轨道。");
    } else {
        ImGui::TextDisabled("%s · %s", kind_tag(g_kind),
                            display_text(v, g_kind, g_track).c_str());
        const int keys = track_key_count(v, g_kind, g_track);
        if (keys == 0) {
            ImGui::TextDisabled("这条轨道还没有关键帧。");
        } else {
            ImGui::BeginChild("##timeline_panel_keys", ImVec2(0.0f, pmxui::scale(160.0f)),
                              ImGuiChildFlags_Borders);
            for (int k = 0; k < keys; ++k) {
                ImGui::PushID(k);
                const std::string text = key_text(v, g_kind, g_track, k);
                if (ImGui::Selectable(text.c_str(), g_key == k)) {
                    g_key = k;
                    v.timeline.active_key =
                        g_kind == TimelineTrackKind::Bone ? k : -1;
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
        }
    }

    draw_curve_editor(v);
}
