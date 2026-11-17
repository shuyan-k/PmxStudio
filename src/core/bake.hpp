// 物理烘焙：把实时 Bullet 演算逐帧固化为骨骼轨道。
//
// 确定性来自「固定步长 + 单线程 + 从起始帧重放」，而不是序列化 Bullet 世界。
// 同一构建、同一平台、同一输入两次烘焙应产生逐字节一致的 VMD。
#pragma once

#include <string>

#include "core/anim.hpp"
#include "core/pmx.hpp"
#include "core/pose.hpp"

namespace pmx {

class Physics;

struct BakeOptions {
    int begin_frame = 0;
    int end_frame = -1;             // -1 表示 clip.max_frame()
    bool include_all_bones = false; // true 烘焙全部骨骼，false 只写动力学骨骼
    bool replace = false;           // true 覆盖同名轨道，false 新建同名轨道
    bool warmup = true;             // 起始帧先 reset + 预热
    int warmup_steps = 1;
};

struct BakeProgress {
    int frame = 0;
    int total = 0;
    bool done = false;
    bool cancel_requested = false;
};

// 同步烘焙。source 只读（逐帧采样），output 接收烘焙出的骨骼轨；
// 两者分开是必要的：写入 output 的关键帧不能反过来影响后续帧的采样。
// progress 可为空；返回写入的关键帧数，失败返回 -1 并填 error。
int bake_physics(const AnimClip& source, AnimClip& output, const Model& model, Pose& pose,
                 Physics& physics, const MorphWeights* morph_weights,
                 const BakeOptions& options, BakeProgress* progress, std::string* error);

// 按动画时间推进物理（与渲染帧率解耦）。
// from_frame / to_frame 单位为 VMD 帧（30 fps），内部按 1/120 秒固定子步累加。
void advance_physics_to(Physics& physics, Pose& pose, const MorphWeights* weights,
                        double from_frame, double to_frame);

}  // namespace pmx
