// 物理烘焙实现：固定 1/30 秒一帧重放，每帧内部按 1/120 固定子步推进，
// 然后把物理结果写成普通骨骼关键帧。相同构建 / 平台 / 输入必须逐位一致。
#include "core/bake.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "physics/physics.hpp"

namespace pmx {
namespace {

constexpr double kBakeFrameSeconds = 1.0 / 30.0;

// 烘焙关键帧使用 VMD 默认线性缓动（20/20/107/107，按 0..127 归一化）。
std::array<VmdCurve, 4> linear_curve() {
    VmdCurve c;
    c.x1 = 20.0f / 127.0f;
    c.y1 = 20.0f / 127.0f;
    c.x2 = 107.0f / 127.0f;
    c.y2 = 107.0f / 127.0f;
    return {c, c, c, c};
}

void set_error(std::string* error, const std::string& message) {
    if (error) *error = message;
}

int track_index_of(const AnimClip& clip, const AnimBoneTrack& track) {
    return static_cast<int>(&track - clip.bones.data());
}

}  // namespace

int bake_physics(const AnimClip& source, AnimClip& output, const Model& model, Pose& pose,
                 Physics& physics, const MorphWeights* morph_weights,
                 const BakeOptions& options, BakeProgress* progress, std::string* error) {
    if (error) error->clear();
    // 烘焙不接管形态权重参数：每帧权重一律取自 source 采样结果（保证可重放）。
    (void)morph_weights;

    if (!physics.has_world()) {
        set_error(error, "物理世界未构建");
        return -1;
    }
    if (source.empty()) {
        set_error(error, "动画片段为空");
        return -1;
    }
    if (model.bones.empty()) {
        set_error(error, "模型没有骨骼");
        return -1;
    }

    const int nb = static_cast<int>(model.bones.size());
    const int begin = std::max(0, options.begin_frame);
    const int end = options.end_frame >= 0 ? options.end_frame : source.max_frame();
    if (end < begin) {
        if (progress) {
            progress->frame = begin;
            progress->total = 0;
            progress->done = true;
        }
        return 0;
    }

    // 目标骨骼：include_all_bones 时为全部骨骼；否则只写动力学刚体驱动的骨骼，
    // 且跳过「物理后变形」骨骼（这些骨骼要在物理结果之后再由变形计算）。
    std::vector<int> targets;
    if (options.include_all_bones) {
        targets.resize(static_cast<std::size_t>(nb));
        for (int i = 0; i < nb; ++i) targets[static_cast<std::size_t>(i)] = i;
    } else {
        targets = physics.dynamic_bones();
        targets.erase(std::remove_if(targets.begin(), targets.end(),
                                     [&](int bone) {
                                         return bone < 0 || bone >= nb ||
                                                (model.bones[static_cast<std::size_t>(bone)].flags &
                                                 BF_AFTER_PHYSICS) != 0;
                                     }),
                      targets.end());
    }

    const int total = end - begin + 1;
    if (progress) {
        progress->frame = begin;
        progress->total = total;
        progress->done = false;
    }

    // 从确定的初始状态开始：累加器必须清零，否则上一段播放的余量会改变首批子步。
    physics.reset_accumulator();

    MorphWeights weights;
    MorphWeights static_weights;
    if (options.warmup) {
        // 预热用起始帧的动画权重，避免开烘焙第一帧就因形态差异抖动。
        evaluate_clip(source, static_cast<double>(begin), pose, &static_weights);
        pose.solve_ik();
        physics.warmup_steps = options.warmup_steps > 0 ? options.warmup_steps : 0;
        physics.reset(pose, &static_weights, options.warmup_steps > 0);
        physics.reset_accumulator();
    }

    const std::array<VmdCurve, 4> curve = linear_curve();
    int written = 0;
    for (int frame = begin; frame <= end; ++frame) {
        if (progress) {
            if (progress->cancel_requested) return written;
            progress->frame = frame;
        }

        evaluate_clip(source, static_cast<double>(frame), pose, &weights);
        pose.solve_ik();
        // 固定推进 1/30 秒；内部按 1/120 累加，与调用方帧率无关。
        physics.advance(pose, kBakeFrameSeconds, &weights);

        for (int bone : targets) {
            AnimBoneKey key;
            key.frame = frame;
            key.pos = pose.trans[static_cast<std::size_t>(bone)];
            key.rot = pose.rot[static_cast<std::size_t>(bone)];
            key.curve = curve;
            AnimBoneTrack& track =
                output.ensure_bone_track(bone, model.bones[static_cast<std::size_t>(bone)].name);
            output.set_bone_key(track_index_of(output, track), key);
            ++written;
        }
    }

    if (progress) {
        progress->frame = end;
        progress->done = true;
    }
    return written;
}

void advance_physics_to(Physics& physics, Pose& pose, const MorphWeights* weights,
                        double from_frame, double to_frame) {
    const double dt = (to_frame - from_frame) / 30.0;
    if (dt <= 0.0) return;
    physics.advance(pose, dt, weights);
}

}  // namespace pmx
