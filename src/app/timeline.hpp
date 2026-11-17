// 时间线状态与操作：预览模式下编辑 AnimClip 的关键帧。
//
// 数据来源有两份，共用同一个播放头：
//  - timeline.clip      模型动作（骨骼 / 形态 / IK）
//  - viewer.camera_motion.track  相机动作（独立导入 / 保存 / 清除）
// 未载入 clip 时模型部分退回 motion 播放路径。
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "app/camera.hpp"
#include "core/anim.hpp"

struct Viewer;

enum class TimelineTrackKind { Bone, Morph, Camera, Ik };

struct TimelineKeyRef {
    TimelineTrackKind kind = TimelineTrackKind::Bone;
    int track = 0;  // clip.bones / clip.morphs / clip.ik 的下标；相机恒为 0
    int key = 0;
};

struct TimelineState {
    std::unique_ptr<pmx::AnimClip> clip;
    pmx::VmdBinding binding;
    std::string source;

    bool enabled = false;       // 底部时间线面板是否展开
    bool auto_key = false;      // 拖动骨骼时自动打帧
    bool show_all_tracks = false;
    std::string filter;

    float px_per_frame = 6.0f;   // 横向缩放
    float scroll_frame = 0.0f;   // 左侧起始帧
    float panel_height = 220.0f; // 底部面板高度（像素，已乘 UI 缩放）
    bool dragging_playhead = false;

    std::vector<TimelineKeyRef> selection;
    int active_track = -1;  // clip.bones 下标，关键帧详情用
    int active_key = -1;

    double frame = 0.0;  // 播放头，与 Viewer::motion_frame 双向同步

    // 只算模型动作长度；场景长度（含相机动作）用 timeline_max_frame。
    int model_max_frame() const { return clip ? clip->max_frame() : 0; }
};

// 时间线总长度：模型动作与相机动作取较长者。
int timeline_max_frame(const Viewer& v);

// 用当前 motion / motion_binding 重建 clip（导入 VMD 后调用）。
void timeline_sync_from_motion(Viewer& v);
// 载入外部 clip（例如烘焙结果）；同时更新绑定。
void timeline_set_clip(Viewer& v, pmx::AnimClip clip);
void timeline_clear(Viewer& v);

// 把 clip 的当前帧写入姿态（预览模式下替代 evaluate_vmd）。
void timeline_apply_frame(Viewer& v);
// 当前姿态打关键帧（骨骼 + 形态）。
void timeline_insert_key(Viewer& v);
// 把当前视口相机记成相机关键帧（相机视角下 K 走这里）。
void timeline_insert_camera_key(Viewer& v);
// 视口相机 -> MMD 相机关键帧（当前帧、当前距离 / 注视点 / FOV）。
pmx::AnimCameraKey camera_key_from_orbit(const pmx::OrbitCamera& camera, int frame);
// 选中关键帧的删除 / 平移 / 复制粘贴。
void timeline_delete_selection(Viewer& v);
void timeline_move_selection(Viewer& v, int delta_frames);
void timeline_copy_selection(Viewer& v);
void timeline_paste(Viewer& v, int frame);

// 底部 dope sheet（主循环每帧调用；未展开时立即返回）。
void draw_timeline_panel(Viewer& v);
// 检查器「时间线」标签：轨道树、关键帧详情、曲线编辑。
void panel_timeline(Viewer& v);
