// 动画数据层 / VMD 相机轨回归测试：AnimClip <-> VmdMotion 往返、
// save_vmd_motion 二进制往返、采样端点与中间值。
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "core/anim.hpp"
#include "core/vmd.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "  [失败] %s\n", what);
    }
}

void check_near(double got, double want, double tol, const char* what) {
    ++g_checks;
    if (!(std::fabs(got - want) <= tol)) {
        ++g_failures;
        std::fprintf(stderr, "  [失败] %s: %.9f != %.9f (tol %.2e)\n", what, got, want, tol);
    }
}

void check_vec(pmx::Vec3 got, pmx::Vec3 want, double tol, const char* what) {
    check_near(got.x, want.x, tol, what);
    check_near(got.y, want.y, tol, what);
    check_near(got.z, want.z, tol, what);
}

void check_quat(pmx::Quat got, pmx::Quat want, double tol, const char* what) {
    const double d = static_cast<double>(got.x) * want.x + static_cast<double>(got.y) * want.y +
                     static_cast<double>(got.z) * want.z + static_cast<double>(got.w) * want.w;
    const double sign = d < 0.0 ? -1.0 : 1.0;
    check_near(got.x, want.x * sign, tol, what);
    check_near(got.y, want.y * sign, tol, what);
    check_near(got.z, want.z * sign, tol, what);
    check_near(got.w, want.w * sign, tol, what);
}

pmx::VmdMotion make_motion() {
    pmx::VmdMotion motion;
    motion.model_name = "anim_test";

    pmx::VmdBoneTrack center;
    center.name = "center";
    center.keys.push_back(pmx::VmdBoneKey{0, pmx::Vec3{0.0f, 0.0f, 0.0f},
                                          pmx::identity_quat(), {}});
    center.keys.push_back(pmx::VmdBoneKey{
        10, pmx::Vec3{1.5f, -2.0f, 0.25f},
        pmx::axis_angle_quat({0.0f, 1.0f, 0.0f}, 0.3f), {}});
    center.keys.push_back(pmx::VmdBoneKey{
        20, pmx::Vec3{3.0f, 0.0f, 0.5f},
        pmx::axis_angle_quat({0.0f, 0.0f, 1.0f}, 0.7f), {}});
    motion.bone_tracks.push_back(center);

    pmx::VmdBoneTrack leg;
    leg.name = "leg_L";
    leg.keys.push_back(pmx::VmdBoneKey{0, pmx::Vec3{0.0f, 0.0f, 0.0f},
                                       pmx::identity_quat(), {}});
    leg.keys.push_back(pmx::VmdBoneKey{
        15, pmx::Vec3{0.0f, -1.25f, 0.5f},
        pmx::axis_angle_quat({1.0f, 0.0f, 0.0f}, -0.45f), {}});
    leg.keys.push_back(pmx::VmdBoneKey{
        30, pmx::Vec3{0.0f, -2.5f, 1.0f},
        pmx::axis_angle_quat({1.0f, 0.0f, 0.0f}, -0.9f), {}});
    motion.bone_tracks.push_back(leg);

    pmx::VmdMorphTrack smile;
    smile.name = "smile";
    smile.keys.push_back(pmx::VmdMorphKey{0, 0.0f});
    smile.keys.push_back(pmx::VmdMorphKey{10, 0.75f});
    smile.keys.push_back(pmx::VmdMorphKey{20, 1.0f});
    motion.morph_tracks.push_back(smile);

    pmx::VmdCameraKey cam0;
    cam0.frame = 0;
    cam0.distance = 45.0f;
    cam0.target = pmx::Vec3{0.0f, 10.0f, 0.0f};
    cam0.rot = pmx::Vec3{0.1f, 0.2f, 0.3f};
    cam0.fov = 30;
    cam0.perspective = true;
    motion.camera_track.push_back(cam0);

    pmx::VmdCameraKey cam1;
    cam1.frame = 24;
    cam1.distance = 50.5f;
    cam1.target = pmx::Vec3{1.0f, 11.0f, -2.0f};
    cam1.rot = pmx::Vec3{0.15f, -0.25f, 0.5f};
    cam1.fov = 40;
    cam1.perspective = false;
    motion.camera_track.push_back(cam1);

    return motion;
}

pmx::VmdBinding make_binding(const pmx::VmdMotion& motion) {
    pmx::VmdBinding binding;
    binding.bone_target = {0, 1};
    binding.morph_target = {0};
    binding.bone_matched = 2;
    binding.morph_matched = 1;
    (void)motion;
    return binding;
}

void check_motion(const pmx::VmdMotion& motion, const char* tag) {
    char label[128];

    std::snprintf(label, sizeof(label), "%s: 骨骼轨数", tag);
    check(motion.bone_tracks.size() == 2, label);
    std::snprintf(label, sizeof(label), "%s: 形态轨数", tag);
    check(motion.morph_tracks.size() == 1, label);
    std::snprintf(label, sizeof(label), "%s: 相机帧数", tag);
    check(motion.camera_track.size() == 2, label);
    std::snprintf(label, sizeof(label), "%s: camera_keys", tag);
    check(motion.camera_keys == 2, label);

    if (motion.bone_tracks.size() == 2) {
        const pmx::VmdBoneTrack& center = motion.bone_tracks[0];
        std::snprintf(label, sizeof(label), "%s: center 关键帧数", tag);
        check(center.name == "center" && center.keys.size() == 3, label);
        if (center.keys.size() == 3) {
            std::snprintf(label, sizeof(label), "%s: center 帧号", tag);
            check(center.keys[0].frame == 0 && center.keys[1].frame == 10 &&
                      center.keys[2].frame == 20,
                  label);
            check_vec(center.keys[1].pos, pmx::Vec3{1.5f, -2.0f, 0.25f}, 1e-5, "center 位移");
            check_quat(center.keys[2].rot,
                       pmx::axis_angle_quat({0.0f, 0.0f, 1.0f}, 0.7f), 1e-5, "center 旋转");
        }
        const pmx::VmdBoneTrack& leg = motion.bone_tracks[1];
        std::snprintf(label, sizeof(label), "%s: leg 关键帧数", tag);
        check(leg.name == "leg_L" && leg.keys.size() == 3, label);
        if (leg.keys.size() == 3) {
            check_vec(leg.keys[1].pos, pmx::Vec3{0.0f, -1.25f, 0.5f}, 1e-5, "leg 位移");
            check_quat(leg.keys[2].rot,
                       pmx::axis_angle_quat({1.0f, 0.0f, 0.0f}, -0.9f), 1e-5, "leg 旋转");
        }
    }

    if (!motion.morph_tracks.empty() && motion.morph_tracks[0].keys.size() == 3) {
        std::snprintf(label, sizeof(label), "%s: 形态权重", tag);
        check_near(motion.morph_tracks[0].keys[1].weight, 0.75, 1e-5, label);
        check_near(motion.morph_tracks[0].keys[2].weight, 1.0, 1e-5, label);
    } else {
        std::snprintf(label, sizeof(label), "%s: 形态关键帧数", tag);
        check(false, label);
    }

    if (motion.camera_track.size() == 2) {
        const pmx::VmdCameraKey& a = motion.camera_track[0];
        const pmx::VmdCameraKey& b = motion.camera_track[1];
        std::snprintf(label, sizeof(label), "%s: 相机帧号", tag);
        check(a.frame == 0 && b.frame == 24, label);
        check_near(a.distance, 45.0, 1e-5, "相机距离 0");
        check_near(b.distance, 50.5, 1e-5, "相机距离 1");
        check_vec(a.target, pmx::Vec3{0.0f, 10.0f, 0.0f}, 1e-5, "相机注视点 0");
        check_vec(b.target, pmx::Vec3{1.0f, 11.0f, -2.0f}, 1e-5, "相机注视点 1");
        check_vec(a.rot, pmx::Vec3{0.1f, 0.2f, 0.3f}, 1e-5, "相机欧拉角 0");
        check_vec(b.rot, pmx::Vec3{0.15f, -0.25f, 0.5f}, 1e-5, "相机欧拉角 1");
        std::snprintf(label, sizeof(label), "%s: 相机 FOV / 透视", tag);
        check(a.fov == 30 && a.perspective && b.fov == 40 && !b.perspective, label);
    }
}

void test_roundtrip() {
    const pmx::VmdMotion motion = make_motion();
    const pmx::VmdBinding binding = make_binding(motion);

    const pmx::AnimClip clip = pmx::from_vmd(motion, binding);
    check(clip.bones.size() == 2 && clip.bones[0].bone == 0 && clip.bones[1].bone == 1,
          "from_vmd 骨骼绑定索引");
    check(clip.morphs.size() == 1 && clip.morphs[0].morph == 0, "from_vmd 形态绑定索引");
    check(clip.camera.size() == 2, "from_vmd 相机帧数");

    const pmx::VmdMotion motion2 = pmx::to_vmd(clip);
    check_motion(motion2, "第一次往返");

    const pmx::VmdBinding binding2 = make_binding(motion2);
    const pmx::AnimClip clip2 = pmx::from_vmd(motion2, binding2);
    const pmx::VmdMotion motion3 = pmx::to_vmd(clip2);
    check_motion(motion3, "第二次往返");
}

void test_binary() {
    const pmx::VmdMotion motion = make_motion();
    const pmx::VmdBinding binding = make_binding(motion);
    const pmx::AnimClip clip = pmx::from_vmd(motion, binding);
    const pmx::VmdMotion motion2 = pmx::to_vmd(clip);

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "pmxstudio_anim_test.vmd";
    const std::string path_str = path.string();
    const pmx::VmdExport exported = pmx::save_vmd_motion(path_str, motion2);
    check(exported.bones == 6 && exported.morphs == 3, "save_vmd_motion 帧计数");

    const pmx::VmdMotion loaded = pmx::load_vmd(path_str);
    check_motion(loaded, "二进制往返");
    check(loaded.model_name == "anim_test", "二进制往返: 模型名");
    check(loaded.camera_keys == 2, "二进制往返: declared camera_keys");

    // 相机插值表：默认线性曲线读回为 0/0/1/1（写入 0/0/127/127）。
    if (loaded.camera_track.size() == 2) {
        const pmx::VmdCurve& c = loaded.camera_track[0].curve[0];
        check_near(c.x1, 0.0, 1e-5, "相机插值 x1");
        check_near(c.y1, 0.0, 1e-5, "相机插值 y1");
        check_near(c.x2, 1.0, 1e-5, "相机插值 x2");
        check_near(c.y2, 1.0, 1e-5, "相机插值 y2");
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

void test_sampling() {
    pmx::AnimBoneTrack track;
    track.name = "probe";
    track.bone = 0;
    track.keys.push_back(pmx::AnimBoneKey{0, pmx::Vec3{0.0f, 0.0f, 0.0f},
                                          pmx::identity_quat(), {}});
    track.keys.push_back(pmx::AnimBoneKey{20, pmx::Vec3{10.0f, 0.0f, 0.0f},
                                          pmx::axis_angle_quat({0.0f, 1.0f, 0.0f}, 1.0f),
                                          {}});

    pmx::Vec3 pos;
    pmx::Quat rot;
    check(pmx::sample_bone_track(track, 10.0, pos, rot), "sample_bone_track 返回 true");
    check_near(pos.x, 5.0, 1e-5, "中间帧位移在两端之间（x=5）");
    check(pos.x > 0.0f && pos.x < 10.0f, "中间帧位移严格位于两端之间");
    const double qlen = std::sqrt(static_cast<double>(rot.x) * rot.x +
                                  static_cast<double>(rot.y) * rot.y +
                                  static_cast<double>(rot.z) * rot.z +
                                  static_cast<double>(rot.w) * rot.w);
    check_near(qlen, 1.0, 1e-5, "中间帧旋转为单位四元数");

    pmx::sample_bone_track(track, -5.0, pos, rot);
    check_near(pos.x, 0.0, 1e-5, "越界取首帧");
    pmx::sample_bone_track(track, 100.0, pos, rot);
    check_near(pos.x, 10.0, 1e-5, "越界取末帧");

    pmx::AnimMorphTrack morph;
    morph.name = "m";
    morph.morph = 0;
    morph.keys.push_back(pmx::AnimMorphKey{0, 0.0f});
    morph.keys.push_back(pmx::AnimMorphKey{10, 1.0f});
    check_near(pmx::sample_morph_track(morph, 5.0), 0.5, 1e-5, "形态线性插值");
    check_near(pmx::sample_morph_track(morph, 50.0), 1.0, 1e-5, "形态越界取末帧");

    std::vector<pmx::AnimCameraKey> cameras;
    pmx::AnimCameraKey a;
    a.frame = 0;
    a.distance = 10.0f;
    a.target = pmx::Vec3{0.0f, 0.0f, 0.0f};
    a.rot = pmx::Vec3{0.0f, 0.0f, 0.0f};
    a.fov = 30;
    cameras.push_back(a);
    pmx::AnimCameraKey b = a;
    b.frame = 10;
    b.distance = 20.0f;
    b.target = pmx::Vec3{10.0f, 0.0f, 0.0f};
    b.rot = pmx::Vec3{0.0f, 1.0f, 0.0f};
    b.fov = 40;
    cameras.push_back(b);

    pmx::AnimCameraKey out;
    check(pmx::sample_camera_track(cameras, 5.0, out), "sample_camera_track 返回 true");
    check_near(out.distance, 15.0, 1e-5, "相机距离插值");
    check_near(out.target.x, 5.0, 1e-5, "相机注视点插值");
    check_near(out.rot.y, 0.5, 1e-5, "相机欧拉角插值");
    check(out.fov == 35, "相机 FOV 插值");
    check(!pmx::sample_camera_track({}, 0.0, out), "空相机轨返回 false");
}

}  // namespace

int main() {
    test_roundtrip();
    test_binary();
    test_sampling();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d / %d 项检查失败\n", g_failures, g_checks);
        return 1;
    }
    std::printf("anim: %d 项检查全部通过\n", g_checks);
    return 0;
}
