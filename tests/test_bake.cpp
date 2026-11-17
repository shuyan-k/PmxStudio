// 烘焙确定性回归：同一模型、同一初始状态下连续烘焙两次，逐帧比较关键帧。
// 覆盖的是「固定步长 + 从起始帧重放」这条确定性路径，而不是实时演算。
#include <cstdio>
#include <exception>
#include <string>
#include <utility>

#include "core/anim.hpp"
#include "core/bake.hpp"
#include "core/pmx.hpp"
#include "core/pose.hpp"

#ifdef PMX_ENABLE_PHYSICS
#include "physics/physics.hpp"
#endif

namespace {

#ifdef PMX_ENABLE_PHYSICS

pmx::AnimClip make_clip(const pmx::Model& model) {
    pmx::AnimClip clip;
    clip.name = "bake";
    // 60 帧的空轨道：不含关键帧，只用来绑定骨骼并让片段非空；
    // 烘焙范围由 BakeOptions::end_frame 显式给出。
    pmx::AnimBoneTrack track;
    track.bone = model.bones.empty() ? -1 : 0;
    track.name = model.bones.empty() ? std::string() : model.bones[0].name;
    clip.bones.push_back(std::move(track));
    return clip;
}

bool same_key(const pmx::AnimBoneKey& a, const pmx::AnimBoneKey& b) {
    return a.frame == b.frame && a.pos.x == b.pos.x && a.pos.y == b.pos.y &&
           a.pos.z == b.pos.z && a.rot.x == b.rot.x && a.rot.y == b.rot.y &&
           a.rot.z == b.rot.z && a.rot.w == b.rot.w;
}

bool bake_once(const pmx::Model& model, const pmx::AnimClip& source, pmx::AnimClip& output,
               pmx::Physics& physics, pmx::Pose& pose, const pmx::BakeOptions& options,
               int& keys) {
    std::string error;
    pmx::BakeProgress progress;
    keys = pmx::bake_physics(source, output, model, pose, physics, nullptr, options, &progress,
                             &error);
    if (keys < 0) {
        std::fprintf(stderr, "烘焙失败：%s\n", error.c_str());
        return false;
    }
    if (!progress.done) {
        std::fprintf(stderr, "烘焙未正常结束\n");
        return false;
    }
    return true;
}

int run(const pmx::Model& model) {
    pmx::BakeOptions options;
    options.begin_frame = 0;
    options.end_frame = 60;
    options.warmup = true;
    options.warmup_steps = 1;

    const pmx::AnimClip source1 = make_clip(model);
    pmx::AnimClip output1;
    pmx::Pose pose1(model);
    pose1.reset();
    pmx::Physics physics1;
    if (!physics1.build(model)) {
        std::printf("skip: 物理世界构建失败（%s）\n", physics1.error.c_str());
        return 0;
    }
    physics1.enabled = true;
    int keys1 = 0;
    if (!bake_once(model, source1, output1, physics1, pose1, options, keys1)) return 1;

    // 第二次从独立的 Bullet 世界开始，验证结果只取决于输入而不是上一次的残留状态。
    const pmx::AnimClip source2 = make_clip(model);
    pmx::AnimClip output2;
    pmx::Pose pose2(model);
    pose2.reset();
    pmx::Physics physics2;
    if (!physics2.build(model)) {
        std::printf("skip: 物理世界构建失败（%s）\n", physics2.error.c_str());
        return 0;
    }
    physics2.enabled = true;
    int keys2 = 0;
    if (!bake_once(model, source2, output2, physics2, pose2, options, keys2)) return 1;

    bool ok = keys1 == keys2;
    if (!ok) std::fprintf(stderr, "关键帧总数不同：%d != %d\n", keys1, keys2);

    if (output1.bones.size() != output2.bones.size()) {
        std::fprintf(stderr, "轨道数不同：%zu != %zu\n", output1.bones.size(),
                     output2.bones.size());
        return 1;
    }

    for (const pmx::AnimBoneTrack& a : output1.bones) {
        const int index = output2.find_bone_track(a.bone, a.name);
        if (index < 0) {
            std::fprintf(stderr, "第二次烘焙缺少骨骼轨道：%s\n", a.name.c_str());
            ok = false;
            continue;
        }
        const pmx::AnimBoneTrack& b = output2.bones[static_cast<std::size_t>(index)];
        if (a.keys.size() != b.keys.size()) {
            std::fprintf(stderr, "轨道 %s 关键帧数不同：%zu != %zu\n", a.name.c_str(),
                         a.keys.size(), b.keys.size());
            ok = false;
            continue;
        }
        for (std::size_t i = 0; i < a.keys.size(); ++i) {
            if (!same_key(a.keys[i], b.keys[i])) {
                std::fprintf(stderr, "轨道 %s 第 %zu 个关键帧不一致（frame %d）\n",
                             a.name.c_str(), i, a.keys[i].frame);
                ok = false;
                break;
            }
        }
    }

    std::printf("刚体 %zu，动态骨骼 %zu，写入 %d 个关键帧，两次烘焙%s\n", model.rigid.size(),
                physics1.dynamic_bones().size(), keys1, ok ? "逐位一致" : "不一致");
    return ok ? 0 : 1;
}

#endif  // PMX_ENABLE_PHYSICS

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef PMX_ENABLE_PHYSICS
#ifdef PMX_TEST_MODEL
    const std::string default_path = PMX_TEST_MODEL;
#else
    const std::string default_path = "tests/test_model.pmx";
#endif
    const std::string path = argc > 1 ? argv[1] : default_path;
    pmx::Model model;
    try {
        model = pmx::load_pmx(path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "解析失败：%s\n", e.what());
        return 2;
    }
    if (model.rigid.empty()) {
        std::printf("skip: 模型没有刚体\n");
        return 0;
    }
    return run(model);
#else
    (void)argc;
    (void)argv;
    std::printf("skip: 编译时已关闭物理（PMX_ENABLE_PHYSICS=OFF）\n");
    return 0;
#endif
}
