// VMD（Vocaloid Motion Data）解析、关键帧采样与单帧导出。
// 时间轴与 MMD 一致：帧号按 30 fps 计。骨骼位移 / 旋转用逐轴贝塞尔缓动，
// 形态权重为线性插值。
#pragma once

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "core/math.hpp"
#include "core/pose.hpp"

namespace pmx {

// 单轴插值曲线：控制点 (x1, y1) / (x2, y2)，取值 0..1。
struct VmdCurve {
    float x1 = 0.0f;
    float y1 = 0.0f;
    float x2 = 1.0f;
    float y2 = 1.0f;
    float ease(float t) const;
};

struct VmdBoneKey {
    int frame = 0;
    Vec3 pos{};
    Quat rot{};
    // X / Y / Z / 旋转 四个轴各自的缓动曲线。
    std::array<VmdCurve, 4> curve{};
};

struct VmdMorphKey {
    int frame = 0;
    float weight = 0.0f;
};

struct VmdBoneTrack {
    std::string name;
    std::vector<VmdBoneKey> keys;
};

struct VmdMorphTrack {
    std::string name;
    std::vector<VmdMorphKey> keys;
};

// 「表示枠 / IK」帧：一帧里列出每个 IK 骨骼的开关状态。MMD 的足 IK 开关就是
// 通过它播放的：关掉足 IK 后，腿完全由骨骼轨道驱动（走路时常见）。
struct VmdIkState {
    std::string bone;
    bool enabled = true;
};

struct VmdIkFrame {
    int frame = 0;
    bool visible = true;
    std::vector<VmdIkState> states;
};

// 相机帧（61 字节）：注视点 + 距离 + 欧拉角 + FOV。MMD 的插值表只用前四段。
struct VmdCameraKey {
    int frame = 0;
    float distance = 45.0f;
    Vec3 target{};
    Vec3 rot{};
    std::array<VmdCurve, 4> curve{};
    int fov = 30;
    bool perspective = true;
};

struct VmdMotion {
    std::string source;
    std::string version = "0002";
    std::string model_name;
    std::vector<VmdBoneTrack> bone_tracks;
    std::vector<VmdMorphTrack> morph_tracks;
    // 表示枠 / IK 帧（按帧号升序）。
    std::vector<VmdIkFrame> ik_frames;
    // 相机轨（按帧号升序）；为空表示动作没有相机数据。
    std::vector<VmdCameraKey> camera_track;
    int camera_keys = 0;  // 文件里声明的相机帧数（即使解析失败也保留）
    int max_frame = 0;

    bool empty() const {
        return bone_tracks.empty() && morph_tracks.empty() && camera_track.empty();
    }
};

// 把动作轨道绑定到模型骨骼 / 形态索引；未匹配项保留名称供界面提示。
struct VmdBinding {
    std::vector<int> bone_target;   // 每个骨骼轨道对应模型骨骼索引，-1 未匹配
    std::vector<int> morph_target;  // 每个形态轨道对应模型形态索引，-1 未匹配
    // 每个表示枠帧里各 IK 名称绑定到的骨骼索引（与 ik_frames 一一对应）。
    std::vector<std::vector<std::pair<int, bool>>> ik_states;
    int ik_matched = 0;
    int bone_matched = 0;
    int morph_matched = 0;
    std::vector<std::string> missing_bones;
    std::vector<std::string> missing_morphs;
    std::vector<std::pair<std::string, std::string>> renamed_bones;
    std::vector<std::pair<std::string, std::string>> renamed_morphs;
};

VmdMotion load_vmd(const std::string& path);
VmdBinding bind_vmd(const Model& model, const VmdMotion& motion);

// 采样指定帧写入 pose（重置为静止姿态后覆盖 base 旋转 / 位移），形态权重写入 weights。
void evaluate_vmd(const VmdMotion& motion, const VmdBinding& binding, double frame,
                  Pose& pose, MorphWeights* morph_weights);

struct VmdExport {
    std::string path;
    int bones = 0;
    int morphs = 0;
    int frame = 0;
};

// 把当前姿态导出为单帧 VMD：默认只导出与静止姿态不同的骨骼和非零形态权重。
VmdExport save_vmd(const std::string& path, const Model& model, const Pose& pose,
                   const MorphWeights* morph_weights = nullptr,
                   const std::string& model_name = std::string(),
                   bool include_all = false, int frame = 0);

// 按 VmdMotion 的全部轨道（骨骼 / 形态 / 相机 / IK）写出多帧 VMD。
VmdExport save_vmd_motion(const std::string& path, const VmdMotion& motion);

}  // namespace pmx
