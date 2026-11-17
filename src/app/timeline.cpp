// 时间线核心操作与底部 dope sheet：关键帧增删 / 平移 / 复制，轨道行与曲目绘制。
#include "app/timeline.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "app/panels/common.hpp"
#include "app/ui_scale.hpp"
#include "app/viewer.hpp"
#include "core/anim.hpp"
#include "core/name_zh.hpp"
#include "core/pose.hpp"

namespace {

// ---------------------------------------------------------------- 关键帧访问

int key_count(const pmx::AnimClip& clip, TimelineTrackKind kind, int track) {
    switch (kind) {
        case TimelineTrackKind::Bone:
            if (track >= 0 && track < static_cast<int>(clip.bones.size()))
                return static_cast<int>(clip.bones[track].keys.size());
            break;
        case TimelineTrackKind::Morph:
            if (track >= 0 && track < static_cast<int>(clip.morphs.size()))
                return static_cast<int>(clip.morphs[track].keys.size());
            break;
        case TimelineTrackKind::Camera:
            return static_cast<int>(clip.camera.size());
        case TimelineTrackKind::Ik:
            // IK 是逐帧开关列表，界面上作为一条轨道展示。
            if (track == 0) return static_cast<int>(clip.ik.size());
            break;
    }
    return 0;
}

int key_frame(const pmx::AnimClip& clip, TimelineTrackKind kind, int track, int key) {
    if (key < 0 || key >= key_count(clip, kind, track)) return -1;
    switch (kind) {
        case TimelineTrackKind::Bone:
            return clip.bones[track].keys[key].frame;
        case TimelineTrackKind::Morph:
            return clip.morphs[track].keys[key].frame;
        case TimelineTrackKind::Camera:
            return clip.camera[key].frame;
        case TimelineTrackKind::Ik:
            return clip.ik[key].frame;
    }
    return -1;
}

// 返回是否成功写入新帧号。
bool set_key_frame(pmx::AnimClip& clip, const TimelineKeyRef& ref, int frame) {
    if (ref.key < 0 || ref.key >= key_count(clip, ref.kind, ref.track)) return false;
    switch (ref.kind) {
        case TimelineTrackKind::Bone:
            clip.bones[ref.track].keys[ref.key].frame = frame;
            return true;
        case TimelineTrackKind::Morph:
            clip.morphs[ref.track].keys[ref.key].frame = frame;
            return true;
        case TimelineTrackKind::Camera:
            clip.camera[ref.key].frame = frame;
            return true;
        case TimelineTrackKind::Ik:
            clip.ik[ref.key].frame = frame;
            return true;
    }
    return false;
}

void erase_key(pmx::AnimClip& clip, const TimelineKeyRef& ref) {
    switch (ref.kind) {
        case TimelineTrackKind::Bone: {
            auto& keys = clip.bones[ref.track].keys;
            if (ref.key >= 0 && ref.key < static_cast<int>(keys.size()))
                keys.erase(keys.begin() + ref.key);
            break;
        }
        case TimelineTrackKind::Morph: {
            auto& keys = clip.morphs[ref.track].keys;
            if (ref.key >= 0 && ref.key < static_cast<int>(keys.size()))
                keys.erase(keys.begin() + ref.key);
            break;
        }
        case TimelineTrackKind::Camera:
            if (ref.key >= 0 && ref.key < static_cast<int>(clip.camera.size()))
                clip.camera.erase(clip.camera.begin() + ref.key);
            break;
        case TimelineTrackKind::Ik:
            if (ref.key >= 0 && ref.key < static_cast<int>(clip.ik.size()))
                clip.ik.erase(clip.ik.begin() + ref.key);
            break;
    }
}

// 与 motion 面板保持一致的跳帧语义：写回帧号、标脏、物理只清速度。
void jump_to_frame(Viewer& v, float frame) {
    const int max_frame = std::max(0, v.timeline.max_frame());
    frame = std::max(0.0f, std::min(frame, static_cast<float>(max_frame)));
    v.timeline.frame = frame;
    v.motion_frame = frame;
    v.motion_dirty = true;
    if (v.physics_enabled) v.settle_physics();
    if (v.timeline.clip && v.pose) timeline_apply_frame(v);
}

// ---------------------------------------------------------------- 剪贴板

struct ClipboardKey {
    TimelineTrackKind kind = TimelineTrackKind::Bone;
    int track = 0;  // 复制时的轨道下标，粘贴时按绑定索引优先重查
    std::string name;
    int bone = -1;
    int morph = -1;
    int offset = 0;  // 相对选区最早帧的偏移
    pmx::AnimBoneKey bone_key;
    pmx::AnimMorphKey morph_key;
    pmx::AnimCameraKey camera_key;
    pmx::AnimIkKey ik_key;
};

std::vector<ClipboardKey> g_clipboard;

void insert_camera_key(pmx::AnimClip& clip, const pmx::AnimCameraKey& key) {
    auto& keys = clip.camera;
    keys.erase(std::remove_if(keys.begin(), keys.end(),
                              [&key](const pmx::AnimCameraKey& k) {
                                  return k.frame == key.frame;
                              }),
               keys.end());
    keys.push_back(key);
    std::sort(keys.begin(), keys.end(),
              [](const pmx::AnimCameraKey& a, const pmx::AnimCameraKey& b) {
                  return a.frame < b.frame;
              });
}

void insert_ik_key(pmx::AnimClip& clip, const pmx::AnimIkKey& key) {
    auto& keys = clip.ik;
    keys.erase(std::remove_if(keys.begin(), keys.end(),
                              [&key](const pmx::AnimIkKey& k) {
                                  return k.frame == key.frame;
                              }),
               keys.end());
    keys.push_back(key);
    std::sort(keys.begin(), keys.end(),
              [](const pmx::AnimIkKey& a, const pmx::AnimIkKey& b) {
                  return a.frame < b.frame;
              });
}

std::string display_name(pmx::NameKind kind, const std::string& name,
                         const std::string& name_en) {
    std::string text = pmx::name_with_zh(kind, name, name_en);
    if (text.empty()) text = "未命名";
    return text;
}

// ---------------------------------------------------------------- dope sheet 行

struct SheetRow {
    TimelineTrackKind kind = TimelineTrackKind::Bone;
    int track = 0;
    std::string label;
    int keys = 0;
};

void build_rows(Viewer& v, std::vector<SheetRow>& rows) {
    rows.clear();
    const pmx::AnimClip* clip = v.timeline.clip.get();
    if (!clip) return;
    const bool show_all = v.timeline.show_all_tracks;

    for (int i = 0; i < static_cast<int>(clip->bones.size()); ++i) {
        const pmx::AnimBoneTrack& track = clip->bones[i];
        if (!show_all && track.keys.empty()) continue;
        std::string name = track.name;
        std::string name_en;
        if (track.bone >= 0 && track.bone < static_cast<int>(v.model.bones.size())) {
            name = v.model.bones[track.bone].name;
            name_en = v.model.bones[track.bone].name_en;
        }
        std::string label = "骨骼 · " + display_name(pmx::NameKind::Bone, name, name_en);
        if (track.bone < 0) label += "（未绑定）";
        rows.push_back({TimelineTrackKind::Bone, i, label,
                        static_cast<int>(track.keys.size())});
    }

    for (int i = 0; i < static_cast<int>(clip->morphs.size()); ++i) {
        const pmx::AnimMorphTrack& track = clip->morphs[i];
        if (!show_all && track.keys.empty()) continue;
        std::string name = track.name;
        std::string name_en;
        if (track.morph >= 0 && track.morph < static_cast<int>(v.model.morphs.size())) {
            name = v.model.morphs[track.morph].name;
            name_en = v.model.morphs[track.morph].name_en;
        }
        std::string label = "形态 · " + display_name(pmx::NameKind::Morph, name, name_en);
        if (track.morph < 0) label += "（未绑定）";
        rows.push_back({TimelineTrackKind::Morph, i, label,
                        static_cast<int>(track.keys.size())});
    }

    if (v.timeline.camera_track && (show_all || !clip->camera.empty())) {
        rows.push_back({TimelineTrackKind::Camera, 0, "相机 · 相机轨",
                        static_cast<int>(clip->camera.size())});
    }

    if (show_all || !clip->ik.empty()) {
        rows.push_back({TimelineTrackKind::Ik, 0, "IK · IK 开关轨",
                        static_cast<int>(clip->ik.size())});
    }
}

bool track_selected(const TimelineState& timeline, TimelineTrackKind kind, int track) {
    for (const TimelineKeyRef& ref : timeline.selection)
        if (ref.kind == kind && ref.track == track) return true;
    return false;
}

bool key_selected(const TimelineState& timeline, TimelineTrackKind kind, int track, int key) {
    for (const TimelineKeyRef& ref : timeline.selection)
        if (ref.kind == kind && ref.track == track && ref.key == key) return true;
    return false;
}

void add_selection(Viewer& v, TimelineTrackKind kind, int track, int key) {
    for (const TimelineKeyRef& ref : v.timeline.selection)
        if (ref.kind == kind && ref.track == track && ref.key == key) return;
    v.timeline.selection.push_back({kind, track, key});
    v.timeline.active_key = key;
    if (kind == TimelineTrackKind::Bone) v.timeline.active_track = track;
}

ImU32 key_color(TimelineTrackKind kind) {
    switch (kind) {
        case TimelineTrackKind::Bone:
            return IM_COL32(255, 194, 92, 255);
        case TimelineTrackKind::Morph:
            return IM_COL32(116, 214, 140, 255);
        case TimelineTrackKind::Camera:
            return IM_COL32(112, 172, 255, 255);
        case TimelineTrackKind::Ik:
            return IM_COL32(202, 140, 255, 255);
    }
    return IM_COL32(220, 220, 220, 255);
}

// dope sheet 的框选 / 拖拽状态：TimelineState 没有这些字段，放在文件内。
bool g_box_selecting = false;
ImVec2 g_box_start{};
TimelineTrackKind g_active_kind = TimelineTrackKind::Bone;
int g_active_track = -1;

}  // namespace

// ================================================================ 状态同步

void timeline_sync_from_motion(Viewer& v) {
    if (!v.motion) return;
    v.timeline.clip =
        std::make_unique<pmx::AnimClip>(pmx::from_vmd(*v.motion, v.motion_binding));
    v.timeline.binding = v.motion_binding;
    v.timeline.source = v.motion->source;
    v.timeline.selection.clear();
    v.timeline.active_track = -1;
    v.timeline.active_key = -1;
    v.timeline.frame = 0.0;
    v.timeline.dragging_playhead = false;
    v.motion_frame = 0.0f;
    v.motion_dirty = true;
    if (v.timeline.px_per_frame <= 0.0f) v.timeline.px_per_frame = 6.0f;
}

void timeline_set_clip(Viewer& v, pmx::AnimClip clip) {
    v.timeline.clip = std::make_unique<pmx::AnimClip>(std::move(clip));
    // 轨道已自带骨骼 / 形态索引，不需要旧的 VMD 绑定。
    v.timeline.binding = pmx::VmdBinding{};
    v.timeline.source = v.timeline.clip->name;
    v.timeline.selection.clear();
    v.timeline.active_track = -1;
    v.timeline.active_key = -1;
    v.timeline.dragging_playhead = false;
    // 保留当前帧，新剪辑更短时夹到末帧。
    const int max_frame = v.timeline.clip->max_frame();
    if (v.motion_frame > static_cast<float>(max_frame))
        v.motion_frame = static_cast<float>(max_frame);
    v.timeline.frame = v.motion_frame;
    v.motion_dirty = true;
}

void timeline_clear(Viewer& v) {
    v.timeline.clip.reset();
    v.timeline.binding = pmx::VmdBinding{};
    v.timeline.source.clear();
    v.timeline.selection.clear();
    v.timeline.active_track = -1;
    v.timeline.active_key = -1;
    v.timeline.dragging_playhead = false;
}

void timeline_apply_frame(Viewer& v) {
    if (!v.timeline.clip || !v.pose) return;
    pmx::evaluate_clip(*v.timeline.clip, v.motion_frame, *v.pose, &v.morph_weights);
    std::fill(v.morph_values.begin(), v.morph_values.end(), 0.0f);
    for (const auto& [index, weight] : v.morph_weights)
        if (index >= 0 && index < static_cast<int>(v.morph_values.size()))
            v.morph_values[static_cast<std::size_t>(index)] = weight;
    v.morph_dirty = true;
    v.timeline.frame = v.motion_frame;
}

// ================================================================ 关键帧编辑

void timeline_insert_key(Viewer& v) {
    pmx::AnimClip* clip = v.timeline.clip.get();
    if (!clip || !v.pose) return;

    const int frame = static_cast<int>(std::lround(v.motion_frame));
    const pmx::Pose& pose = *v.pose;
    int bone_count = 0;
    int morph_count = 0;

    const int bones = std::min(pose.n, static_cast<int>(v.model.bones.size()));
    for (int b = 0; b < bones; ++b) {
        const pmx::AnimBoneKey key = pmx::make_bone_key(pose, b, frame);
        const float pos_delta = pmx::length(key.pos);
        const float rot_delta =
            std::sqrt(key.rot.x * key.rot.x + key.rot.y * key.rot.y +
                      key.rot.z * key.rot.z + (key.rot.w - 1.0f) * (key.rot.w - 1.0f));
        // 与静止姿态没有明显差异的骨骼不写关键帧，避免每次打帧生成大量零轨道。
        if (pos_delta <= 1e-5f && rot_delta <= 1e-5f) continue;
        const std::string& name = v.model.bones[static_cast<std::size_t>(b)].name;
        int track = clip->find_bone_track(b, name);
        if (track < 0) {
            clip->ensure_bone_track(b, name);
            track = clip->find_bone_track(b, name);
        }
        if (track < 0) continue;
        clip->set_bone_key(track, key);
        ++bone_count;
    }

    for (int m = 0; m < static_cast<int>(v.model.morphs.size()); ++m) {
        const pmx::AnimMorphKey key = pmx::make_morph_key(v.morph_weights, m, frame);
        if (std::fabs(key.weight) <= 1e-5f) continue;
        const std::string& name = v.model.morphs[static_cast<std::size_t>(m)].name;
        int track = clip->find_morph_track(m, name);
        if (track < 0) {
            clip->ensure_morph_track(m, name);
            track = clip->find_morph_track(m, name);
        }
        if (track < 0) continue;
        clip->set_morph_key(track, key);
        ++morph_count;
    }

    v.motion_dirty_edit = true;
    v.timeline.frame = v.motion_frame;
    if (bone_count == 0 && morph_count == 0) {
        v.status = "当前姿态与静止姿态一致，没有写入关键帧";
        return;
    }
    char message[128];
    std::snprintf(message, sizeof message, "已在第 %d 帧打关键帧（骨骼 %d · 形态 %d）", frame,
                  bone_count, morph_count);
    v.status = message;
}

void timeline_delete_selection(Viewer& v) {
    pmx::AnimClip* clip = v.timeline.clip.get();
    if (!clip || v.timeline.selection.empty()) return;

    std::vector<TimelineKeyRef> refs = v.timeline.selection;
    // 同一轨道按 key 下标从大到小删除，前面的删除不会让后面的下标失效。
    std::sort(refs.begin(), refs.end(), [](const TimelineKeyRef& a, const TimelineKeyRef& b) {
        if (a.kind != b.kind)
            return static_cast<int>(a.kind) < static_cast<int>(b.kind);
        if (a.track != b.track) return a.track < b.track;
        return a.key > b.key;
    });
    refs.erase(std::unique(refs.begin(), refs.end(),
                           [](const TimelineKeyRef& a, const TimelineKeyRef& b) {
                               return a.kind == b.kind && a.track == b.track &&
                                      a.key == b.key;
                           }),
               refs.end());

    int removed = 0;
    for (const TimelineKeyRef& ref : refs) {
        if (ref.key < 0 || ref.key >= key_count(*clip, ref.kind, ref.track)) continue;
        erase_key(*clip, ref);
        ++removed;
    }

    v.timeline.selection.clear();
    v.timeline.active_key = -1;
    g_active_track = -1;
    if (removed > 0) {
        v.motion_dirty_edit = true;
        char message[96];
        std::snprintf(message, sizeof message, "已删除 %d 个关键帧", removed);
        v.status = message;
    }
}

void timeline_move_selection(Viewer& v, int delta_frames) {
    pmx::AnimClip* clip = v.timeline.clip.get();
    if (!clip || v.timeline.selection.empty() || delta_frames == 0) return;

    int moved = 0;
    bool clamped = false;
    for (const TimelineKeyRef& ref : v.timeline.selection) {
        const int frame = key_frame(*clip, ref.kind, ref.track, ref.key);
        if (frame < 0) continue;
        const int target = frame + delta_frames;
        if (target < 0) clamped = true;
        if (set_key_frame(*clip, ref, std::max(0, target))) ++moved;
    }
    if (moved == 0) return;

    clip->sort_keys();
    v.timeline.selection.clear();
    v.timeline.active_key = -1;
    g_active_track = -1;
    v.motion_dirty_edit = true;

    char message[160];
    std::snprintf(message, sizeof message, "已移动 %d 个关键帧 %+d 帧%s", moved, delta_frames,
                  clamped ? "（左侧已贴到第 0 帧）" : "");
    v.status = message;
}

void timeline_copy_selection(Viewer& v) {
    const pmx::AnimClip* clip = v.timeline.clip.get();
    if (!clip) return;

    struct Entry {
        const TimelineKeyRef* ref;
        int frame;
    };
    std::vector<Entry> entries;
    int base = std::numeric_limits<int>::max();
    for (const TimelineKeyRef& ref : v.timeline.selection) {
        const int frame = key_frame(*clip, ref.kind, ref.track, ref.key);
        if (frame < 0) continue;
        entries.push_back({&ref, frame});
        base = std::min(base, frame);
    }
    if (entries.empty()) {
        v.status = "没有可复制的关键帧";
        return;
    }

    g_clipboard.clear();
    g_clipboard.reserve(entries.size());
    for (const Entry& entry : entries) {
        const TimelineKeyRef& ref = *entry.ref;
        ClipboardKey item;
        item.kind = ref.kind;
        item.track = ref.track;
        item.offset = entry.frame - base;
        switch (ref.kind) {
            case TimelineTrackKind::Bone: {
                const pmx::AnimBoneTrack& track = clip->bones[ref.track];
                item.bone_key = track.keys[ref.key];
                item.bone = track.bone;
                item.name = track.name;
                break;
            }
            case TimelineTrackKind::Morph: {
                const pmx::AnimMorphTrack& track = clip->morphs[ref.track];
                item.morph_key = track.keys[ref.key];
                item.morph = track.morph;
                item.name = track.name;
                break;
            }
            case TimelineTrackKind::Camera:
                item.camera_key = clip->camera[ref.key];
                break;
            case TimelineTrackKind::Ik:
                item.ik_key = clip->ik[ref.key];
                item.name = "IK";
                break;
        }
        g_clipboard.push_back(std::move(item));
    }

    char message[96];
    std::snprintf(message, sizeof message, "已复制 %d 个关键帧", static_cast<int>(g_clipboard.size()));
    v.status = message;
}

void timeline_paste(Viewer& v, int frame) {
    pmx::AnimClip* clip = v.timeline.clip.get();
    if (!clip) return;
    if (g_clipboard.empty()) {
        v.status = "剪贴板没有关键帧";
        return;
    }

    frame = std::max(0, frame);
    int added = 0;
    for (const ClipboardKey& item : g_clipboard) {
        const int target_frame = std::max(0, frame + item.offset);
        switch (item.kind) {
            case TimelineTrackKind::Bone: {
                int track = clip->find_bone_track(item.bone, item.name);
                if (track < 0 && item.track >= 0 &&
                    item.track < static_cast<int>(clip->bones.size())) {
                    const pmx::AnimBoneTrack& candidate = clip->bones[item.track];
                    if (candidate.bone == item.bone &&
                        (item.bone >= 0 || candidate.name == item.name))
                        track = item.track;
                }
                if (track < 0) {
                    clip->ensure_bone_track(item.bone, item.name);
                    track = clip->find_bone_track(item.bone, item.name);
                }
                if (track < 0) break;
                pmx::AnimBoneKey key = item.bone_key;
                key.frame = target_frame;
                clip->set_bone_key(track, key);
                ++added;
                break;
            }
            case TimelineTrackKind::Morph: {
                int track = clip->find_morph_track(item.morph, item.name);
                if (track < 0 && item.track >= 0 &&
                    item.track < static_cast<int>(clip->morphs.size())) {
                    const pmx::AnimMorphTrack& candidate = clip->morphs[item.track];
                    if (candidate.morph == item.morph &&
                        (item.morph >= 0 || candidate.name == item.name))
                        track = item.track;
                }
                if (track < 0) {
                    clip->ensure_morph_track(item.morph, item.name);
                    track = clip->find_morph_track(item.morph, item.name);
                }
                if (track < 0) break;
                pmx::AnimMorphKey key = item.morph_key;
                key.frame = target_frame;
                clip->set_morph_key(track, key);
                ++added;
                break;
            }
            case TimelineTrackKind::Camera: {
                pmx::AnimCameraKey key = item.camera_key;
                key.frame = target_frame;
                insert_camera_key(*clip, key);
                ++added;
                break;
            }
            case TimelineTrackKind::Ik: {
                pmx::AnimIkKey key = item.ik_key;
                key.frame = target_frame;
                insert_ik_key(*clip, key);
                ++added;
                break;
            }
        }
    }

    clip->sort_keys();
    if (added > 0) {
        v.motion_dirty_edit = true;
        char message[128];
        std::snprintf(message, sizeof message, "已粘贴 %d 个关键帧到第 %d 帧", added, frame);
        v.status = message;
    }
}

// ================================================================ 底部面板

void draw_timeline_panel(Viewer& v) {
    if (!v.timeline.enabled) return;

    TimelineState& timeline = v.timeline;
    const float ui = pmxui::scale(1.0f);
    const float status_h = ImGui::GetFrameHeight() + pmxui::scale(4.0f);
    const float panel_h = std::clamp(timeline.panel_height, 120.0f, 600.0f) * ui;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(viewport->Pos.x, viewport->Pos.y + viewport->Size.y - status_h - panel_h),
        ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, panel_h), ImGuiCond_Always);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(pmxui::scale(6.0f), pmxui::scale(3.0f)));
    ImGui::Begin("###timeline_panel", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoSavedSettings);

    // 播放头与 motion_frame 双向同步：外部播放推进 motion_frame 时面板跟随。
    if (!timeline.dragging_playhead) timeline.frame = v.motion_frame;

    // ------------------------------------------------------------ 顶部拖拽条
    const float bar_h = pmxui::scale(4.0f);
    ImGui::InvisibleButton("##timeline_resize_bar",
                           ImVec2(std::max(pmxui::scale(1.0f), ImGui::GetContentRegionAvail().x),
                                  bar_h));
    const bool resize_hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (resize_hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive()) {
        timeline.panel_height -= ImGui::GetIO().MouseDelta.y / ui;
        timeline.panel_height = std::clamp(timeline.panel_height, 120.0f, 600.0f);
    }
    {
        const ImVec2 a = ImGui::GetItemRectMin();
        const ImVec2 b = ImGui::GetItemRectMax();
        const float cx = (a.x + b.x) * 0.5f;
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(cx - pmxui::scale(18.0f), (a.y + b.y) * 0.5f),
            ImVec2(cx + pmxui::scale(18.0f), (a.y + b.y) * 0.5f),
            ImGui::GetColorU32(ImGuiCol_Separator), pmxui::scale(1.0f));
    }

    pmx::AnimClip* clip = timeline.clip.get();
    const bool loaded = clip != nullptr;
    const int max_frame = loaded ? clip->max_frame() : 0;

    // ------------------------------------------------------------ 工具栏
    ImGui::BeginDisabled(!loaded);
    if (ImGui::Button(v.motion_playing ? "暂停" : "播放", ImVec2(pmxui::scale(52.0f), 0.0f))) {
        if (!v.motion_playing) {
            if (v.motion_frame >= static_cast<float>(max_frame)) jump_to_frame(v, 0.0f);
            v.motion_dirty = true;
            if (v.physics_enabled) v.settle_physics();
        }
        v.motion_playing = !v.motion_playing;
    }
    ImGui::SetItemTooltip("播放 / 暂停（30 fps 时间轴）");
    ImGui::SameLine();
    if (ImGui::Button("回到起点", ImVec2(pmxui::scale(74.0f), 0.0f)))
        jump_to_frame(v, 0.0f);
    ImGui::SetItemTooltip("播放头回到第 0 帧");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox("循环", &v.motion_loop);
    ImGui::SameLine();
    ImGui::BeginDisabled(!loaded);
    int frame_input = static_cast<int>(std::lround(v.motion_frame));
    ImGui::SetNextItemWidth(pmxui::scale(78.0f));
    if (ImGui::InputInt("##timeline_frame_input", &frame_input, 1, 10))
        jump_to_frame(v, static_cast<float>(frame_input));
    ImGui::SetItemTooltip("当前帧");
    ImGui::SameLine();
    ImGui::TextDisabled("/ %d", max_frame);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(pmxui::scale(118.0f));
    ImGui::SliderFloat("##timeline_zoom", &timeline.px_per_frame, 0.5f, 40.0f, "%.1f 像素/帧");
    ImGui::SetItemTooltip("横向缩放：滚轮以鼠标为锚点缩放");
    ImGui::SameLine();
    if (ImGui::Button("K 打帧", ImVec2(pmxui::scale(58.0f), 0.0f))) timeline_insert_key(v);
    ImGui::SetItemTooltip("把当前姿态打成关键帧（快捷键 K）");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!loaded || timeline.selection.empty());
    if (ImGui::Button("删除", ImVec2(pmxui::scale(46.0f), 0.0f)))
        timeline_delete_selection(v);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("删除选中的关键帧（Del）");
    ImGui::SameLine();
    ImGui::Checkbox("相机轨", &timeline.camera_track);
    ImGui::SameLine();
    ImGui::Checkbox("自动关键", &timeline.auto_key);
    ImGui::SetItemTooltip("拖动骨骼后自动打帧");
    ImGui::SameLine();
    if (ImGui::Button("关闭", ImVec2(pmxui::scale(46.0f), 0.0f))) timeline.enabled = false;

    // ------------------------------------------------------------ 轨道树
    std::vector<SheetRow> rows;
    float rows_top_screen = 0.0f;
    build_rows(v, rows);

    ImGui::BeginChild("##timeline_tracks", ImVec2(pmxui::scale(240.0f), 0.0f),
                      ImGuiChildFlags_Borders);
    ImGui::Checkbox("全部轨道", &timeline.show_all_tracks);
    ImGui::SameLine();
    ImGui::TextDisabled("%d 轨", static_cast<int>(rows.size()));
    rows_top_screen = ImGui::GetCursorScreenPos().y;

    if (!loaded) {
        ImGui::TextDisabled("未载入动作");
        ImGui::TextDisabled("导入 VMD 后自动生成时间线");
    } else if (rows.empty()) {
        ImGui::TextDisabled("没有可显示的轨道");
        ImGui::TextDisabled("按 K 为当前姿态打帧");
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const SheetRow& row = rows[static_cast<std::size_t>(i)];
        const bool selected = track_selected(timeline, row.kind, row.track) ||
                              (g_active_track == row.track && g_active_kind == row.kind);
        ImGui::PushID(i);
        if (ImGui::Selectable(row.label.c_str(), selected)) {
            g_active_kind = row.kind;
            g_active_track = row.track;
            timeline.active_track =
                row.kind == TimelineTrackKind::Bone ? row.track : -1;
            timeline.active_key = -1;
        }
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();

    // ------------------------------------------------------------ dope sheet
    ImGui::SameLine();
    ImGui::BeginChild("##timeline_dope", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 canvas = ImGui::GetContentRegionAvail();
    canvas.x = std::max(canvas.x, 1.0f);
    canvas.y = std::max(canvas.y, 1.0f);
    const ImVec2 canvas_end(origin.x + canvas.x, origin.y + canvas.y);
    ImGui::InvisibleButton("##timeline_canvas", canvas, ImGuiButtonFlags_MouseButtonLeft);

    const ImGuiIO& io = ImGui::GetIO();
    const float px = std::clamp(timeline.px_per_frame, 0.5f, 40.0f);
    const float header_h = pmxui::scale(20.0f);
    const float row_h = ImGui::GetTextLineHeight() + ImGui::GetStyle().FramePadding.y * 2.0f;
    const float diamond_r = pmxui::scale(4.5f);
    // rows_top_screen 已包含轨道树的滚动偏移，直接作为行区域的屏幕起点。
    const float row_top = rows_top_screen;
    const float content_top = origin.y + header_h;

    auto frame_to_x = [&](double frame) {
        return origin.x + static_cast<float>((frame - timeline.scroll_frame) * px);
    };
    auto x_to_frame = [&](float x) {
        const double frame =
            timeline.scroll_frame + (x - origin.x) / std::max(0.5f, px);
        return static_cast<float>(std::max(0.0f, static_cast<float>(std::round(frame))));
    };
    auto row_center_y = [&](int row) {
        return row_top + static_cast<float>(row) * row_h + row_h * 0.5f;
    };
    auto key_center = [&](int row_index, int key) {
        const SheetRow& row = rows[static_cast<std::size_t>(row_index)];
        return ImVec2(frame_to_x(key_frame(*clip, row.kind, row.track, key)),
                      row_center_y(row_index));
    };

    if (clip) {
        // 滚轮：缩放（以鼠标为锚点）或 Shift 水平滚动。
        if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f) {
            if (io.KeyShift) {
                timeline.scroll_frame =
                    std::max(0.0f, timeline.scroll_frame - io.MouseWheel * 10.0f);
            } else {
                const float anchor_x = io.MousePos.x - origin.x;
                const double anchor_frame =
                    timeline.scroll_frame + anchor_x / std::max(0.5f, px);
                const float next_px =
                    std::clamp(px * std::pow(1.15f, io.MouseWheel), 0.5f, 40.0f);
                timeline.px_per_frame = next_px;
                timeline.scroll_frame = std::max(
                    0.0f, static_cast<float>(anchor_frame - anchor_x / next_px));
            }
        }

        // 命中测试：返回行 / 关键帧下标。
        auto hit_key = [&](const ImVec2& mouse, int& out_row, int& out_key) {
            if (mouse.y < content_top) return false;
            const int row = static_cast<int>((mouse.y - row_top) / row_h);
            if (row < 0 || row >= static_cast<int>(rows.size())) return false;
            const SheetRow& item = rows[static_cast<std::size_t>(row)];
            const int count = key_count(*clip, item.kind, item.track);
            for (int k = 0; k < count; ++k) {
                const ImVec2 c = key_center(row, k);
                if (std::fabs(mouse.x - c.x) <= diamond_r * 1.6f &&
                    std::fabs(mouse.y - c.y) <= diamond_r * 1.6f) {
                    out_row = row;
                    out_key = k;
                    return true;
                }
            }
            return false;
        };

        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            int hit_row = -1;
            int hit_index = -1;
            const bool on_key = hit_key(io.MousePos, hit_row, hit_index);
            if (io.KeyCtrl) {
                // Ctrl + 左键框选。
                g_box_selecting = true;
                g_box_start = io.MousePos;
                if (!io.KeyShift) timeline.selection.clear();
            } else if (on_key) {
                if (!io.KeyShift) timeline.selection.clear();
                const SheetRow& item = rows[static_cast<std::size_t>(hit_row)];
                add_selection(v, item.kind, item.track, hit_index);
                g_active_kind = item.kind;
                g_active_track = item.track;
            } else {
                // 空白处：清空选择并把播放头移到这里，按住即拖动播放头。
                timeline.selection.clear();
                timeline.active_key = -1;
                timeline.dragging_playhead = true;
                jump_to_frame(v, x_to_frame(io.MousePos.x));
            }
        }

        if (g_box_selecting) {
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                const ImVec2 a(std::min(g_box_start.x, io.MousePos.x),
                               std::min(g_box_start.y, io.MousePos.y));
                const ImVec2 b(std::max(g_box_start.x, io.MousePos.x),
                               std::max(g_box_start.y, io.MousePos.y));
                for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
                    const SheetRow& item = rows[static_cast<std::size_t>(i)];
                    const int count = key_count(*clip, item.kind, item.track);
                    for (int k = 0; k < count; ++k) {
                        const ImVec2 c = key_center(i, k);
                        if (c.x >= a.x && c.x <= b.x && c.y >= a.y && c.y <= b.y)
                            add_selection(v, item.kind, item.track, k);
                    }
                }
                g_box_selecting = false;
            }
        }

        if (timeline.dragging_playhead) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
                jump_to_frame(v, x_to_frame(io.MousePos.x));
            else
                timeline.dragging_playhead = false;
        }

        if (!io.WantTextInput &&
            ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            ImGui::IsKeyPressed(ImGuiKey_Delete, false))
            timeline_delete_selection(v);
        if (!io.WantTextInput &&
            ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && io.KeyCtrl &&
            ImGui::IsKeyPressed(ImGuiKey_C, false))
            timeline_copy_selection(v);
        if (!io.WantTextInput &&
            ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && io.KeyCtrl &&
            ImGui::IsKeyPressed(ImGuiKey_V, false))
            timeline_paste(v, static_cast<int>(std::lround(timeline.frame)));
    }

    // ------------------------------------------------------------ 绘制
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(origin, canvas_end, true);
    draw->AddRectFilled(origin, canvas_end, IM_COL32(22, 23, 26, 255));
    draw->AddRectFilled(origin, ImVec2(canvas_end.x, origin.y + header_h),
                        IM_COL32(33, 35, 40, 255));

    if (!clip) {
        const char* hint = "未载入动作";
        const ImVec2 size = ImGui::CalcTextSize(hint);
        draw->AddText(ImVec2(origin.x + (canvas.x - size.x) * 0.5f,
                             origin.y + (canvas.y - size.y) * 0.5f),
                      ImGui::GetColorU32(ImGuiCol_TextDisabled), hint);
        draw->PopClipRect();
        ImGui::EndChild();
        ImGui::End();
        ImGui::PopStyleVar(3);
        return;
    }

    // 每 10 帧竖线、每 30 帧标注。
    {
        const double first = std::floor(timeline.scroll_frame / 10.0) * 10.0;
        const double last = timeline.scroll_frame + canvas.x / std::max(0.5f, px);
        for (double f = first; f <= last; f += 10.0) {
            const float x = frame_to_x(f);
            if (x < origin.x - 1.0f || x > canvas_end.x + 1.0f) continue;
            const bool major = std::fmod(f, 30.0) == 0.0;
            draw->AddLine(ImVec2(x, origin.y + header_h), ImVec2(x, canvas_end.y),
                          major ? IM_COL32(78, 82, 92, 255) : IM_COL32(50, 52, 58, 255), 1.0f);
            if (major) {
                char label[24];
                std::snprintf(label, sizeof label, "%d", static_cast<int>(f));
                draw->AddText(ImVec2(x + pmxui::scale(2.0f), origin.y + pmxui::scale(2.0f)),
                              ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
            }
        }
    }

    // 轨道行。
    draw->PushClipRect(ImVec2(origin.x, content_top), canvas_end, true);
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const SheetRow& item = rows[static_cast<std::size_t>(i)];
        const float y0 = row_top + static_cast<float>(i) * row_h;
        const float y1 = y0 + row_h;
        if (y1 < content_top || y0 > canvas_end.y) continue;
        const bool active =
            (g_active_track == item.track && g_active_kind == item.kind) ||
            track_selected(timeline, item.kind, item.track);
        draw->AddRectFilled(ImVec2(origin.x, y0), ImVec2(canvas_end.x, y1),
                            active ? IM_COL32(44, 49, 60, 255)
                                   : (i % 2 ? IM_COL32(28, 30, 34, 255)
                                            : IM_COL32(25, 27, 31, 255)));
        draw->AddLine(ImVec2(origin.x, y1), ImVec2(canvas_end.x, y1),
                      IM_COL32(42, 44, 50, 255), 1.0f);

        const int count = key_count(*clip, item.kind, item.track);
        for (int k = 0; k < count; ++k) {
            const int frame = key_frame(*clip, item.kind, item.track, k);
            const float x = frame_to_x(frame);
            if (x < origin.x - diamond_r || x > canvas_end.x + diamond_r) continue;
            const float cy = y0 + row_h * 0.5f;
            const bool selected = key_selected(timeline, item.kind, item.track, k);
            const float r = diamond_r + (selected ? pmxui::scale(1.5f) : 0.0f);
            const ImVec2 points[4] = {ImVec2(x, cy - r), ImVec2(x + r, cy),
                                      ImVec2(x, cy + r), ImVec2(x - r, cy)};
            draw->AddConvexPolyFilled(points, 4,
                                      selected ? IM_COL32(255, 255, 255, 255)
                                               : key_color(item.kind));
            draw->AddPolyline(points, 4, IM_COL32(18, 18, 20, 255), ImDrawFlags_Closed,
                              pmxui::scale(1.0f));
        }
    }
    draw->PopClipRect();

    // 框选矩形。
    if (g_box_selecting) {
        const ImVec2 a(std::min(g_box_start.x, io.MousePos.x),
                       std::min(g_box_start.y, io.MousePos.y));
        const ImVec2 b(std::max(g_box_start.x, io.MousePos.x),
                       std::max(g_box_start.y, io.MousePos.y));
        draw->AddRectFilled(a, b, IM_COL32(90, 150, 255, 40));
        draw->AddRect(a, b, IM_COL32(120, 180, 255, 200));
    }

    // 播放头。
    {
        const float x = frame_to_x(timeline.frame);
        if (x >= origin.x - 1.0f && x <= canvas_end.x + 1.0f) {
            draw->AddLine(ImVec2(x, origin.y), ImVec2(x, canvas_end.y),
                          IM_COL32(255, 84, 84, 235), pmxui::scale(1.5f));
            draw->AddTriangleFilled(ImVec2(x - pmxui::scale(5.0f), origin.y),
                                    ImVec2(x + pmxui::scale(5.0f), origin.y),
                                    ImVec2(x, origin.y + pmxui::scale(8.0f)),
                                    IM_COL32(255, 84, 84, 255));
        }
    }

    draw->PopClipRect();
    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleVar(3);
}
