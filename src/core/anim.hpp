// 内存动画模型：时间线编辑以 AnimClip 为唯一真源，VMD 只作为导入 / 导出格式。
//
// 与 VmdMotion 的区别：
//  - 轨道直接记录绑定到的骨骼 / 形态索引，UI 不必每次重查名称；
//  - 相机轨、IK 帧是正式成员，可编辑、可增删；
//  - 关键帧按帧号排序，同帧唯一（重复插入时覆盖）。
#pragma once

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "core/math.hpp"
#include "core/pose.hpp"
#include "core/vmd.hpp"

namespace pmx {

struct AnimBoneKey {
    int frame = 0;
    Vec3 pos{};
    Quat rot{};
    // X / Y / Z / 旋转 四轴缓动，语义与 VmdBoneKey::curve 相同。
    std::array<VmdCurve, 4> curve{};
};

struct AnimMorphKey {
    int frame = 0;
    float weight = 0.0f;
};

struct AnimBoneTrack {
    std::string name;  // 轨道原名（VMD 里的名字），未匹配时保留供界面提示
    int bone = -1;     // 绑定到的模型骨骼索引
    std::vector<AnimBoneKey> keys;
};

struct AnimMorphTrack {
    std::string name;
    int morph = -1;
    std::vector<AnimMorphKey> keys;
};

// MMD 相机：注视点 + 距离 + 欧拉角 + FOV。
struct AnimCameraKey {
    int frame = 0;
    float distance = 45.0f;
    Vec3 target{};  // 注视点
    Vec3 rot{};     // 欧拉角（弧度），与 MMD 的相机旋转一致
    std::array<VmdCurve, 4> curve{};
    int fov = 30;
    bool perspective = true;
};

struct AnimIkState {
    std::string name;
    int bone = -1;
    bool enabled = true;
};

struct AnimIkKey {
    int frame = 0;
    bool visible = true;
    std::vector<AnimIkState> states;
};

class AnimClip {
public:
    std::string name;
    std::vector<AnimBoneTrack> bones;
    std::vector<AnimMorphTrack> morphs;
    std::vector<AnimCameraKey> camera;
    std::vector<AnimIkKey> ik;

    bool empty() const;
    int max_frame() const;
    void refresh_length() {}  // 长度由 max_frame() 按需计算，保留给调用方语义清晰
    // 按帧号排序，同帧只保留最后一条。
    void sort_keys();

    // 找轨道，不存在时创建（bone < 0 时按 name 匹配已有轨道）。
    int find_bone_track(int bone, const std::string& name) const;
    int find_morph_track(int morph, const std::string& name) const;
    AnimBoneTrack& ensure_bone_track(int bone, const std::string& name);
    AnimMorphTrack& ensure_morph_track(int morph, const std::string& name);

    // 插入或覆盖同帧关键帧，返回该帧在轨道内的下标。
    int set_bone_key(int track_index, const AnimBoneKey& key);
    int set_morph_key(int track_index, const AnimMorphKey& key);
};

// VMD -> AnimClip：绑定结果写入轨道的 bone / morph，IK 帧解析成骨骼索引。
AnimClip from_vmd(const VmdMotion& motion, const VmdBinding& binding);

// AnimClip -> VMD：未匹配的轨道按原名写出，绑定索引只用于补名字。
VmdMotion to_vmd(const AnimClip& clip, const Model* model = nullptr,
                 const std::string& model_name = std::string());

// 采样并写入 pose 的 base_rot / base_trans，形态权重写入 weights。
// 语义与 evaluate_vmd 一致（不含 IK 求解与形态展开，由调用方完成）。
void evaluate_clip(const AnimClip& clip, double frame, Pose& pose, MorphWeights* morph_weights);

bool sample_bone_track(const AnimBoneTrack& track, double frame, Vec3& pos, Quat& rot);
float sample_morph_track(const AnimMorphTrack& track, double frame);
// 相机轨采样；空轨返回 false。frame 超界时取首 / 末帧。
bool sample_camera_track(const std::vector<AnimCameraKey>& keys, double frame, AnimCameraKey& out);

// 打关键帧：从当前姿态取骨骼局部量 / 形态权重。
AnimBoneKey make_bone_key(const Pose& pose, int bone, int frame);
AnimMorphKey make_morph_key(const MorphWeights& weights, int morph, int frame);

// 相机欧拉角与四元数互转（短路径插值用）。
Quat camera_rot_quat(const Vec3& euler);
Vec3 camera_rot_euler(const Quat& q, const Vec3& previous);

}  // namespace pmx
