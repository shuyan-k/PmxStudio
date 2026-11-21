// 数值对拍/回归测试：PMX round-trip、姿态求值、IK、蒙皮、物理稳定步进。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "core/bone_category.hpp"
#include "core/flesh.hpp"
#include "core/fs.hpp"
#include "core/math.hpp"
#include "core/material_ext.hpp"
#include "core/morph.hpp"
#include "core/pmx.hpp"
#include "core/pose.hpp"
#include "core/skinning.hpp"
#include "core/utf8_args.hpp"
#include "core/vmd.hpp"
#include "core/vpd.hpp"
#ifdef PMX_ENABLE_PHYSICS
#include "physics/physics.hpp"
#endif

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

double max_abs_diff(const std::vector<pmx::Vec3>& a, const std::vector<pmx::Vec3>& b) {
    if (a.size() != b.size()) return 1e9;
    double m = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        m = std::max(m, static_cast<double>(std::fabs(a[i].x - b[i].x)));
        m = std::max(m, static_cast<double>(std::fabs(a[i].y - b[i].y)));
        m = std::max(m, static_cast<double>(std::fabs(a[i].z - b[i].z)));
    }
    return m;
}

bool finite_mat(const pmx::Mat4& m) {
    for (float v : m.m)
        if (!std::isfinite(v)) return false;
    return true;
}

// ---------------------------------------------------------------- PMX 读写
void test_roundtrip(const std::string& path, const std::filesystem::path& out) {
    std::printf("[1] PMX 读取与 round-trip\n");
    pmx::Model a = pmx::load_pmx(path);
    check(a.vertices.size() > 0, "顶点数 > 0");
    check(a.indices.size() % 3 == 0, "索引按三角形对齐");
    check(a.n_faces() > 0, "面数 > 0");

    a.name = "roundtrip";
    pmx::save_pmx(a, out.string());
    check(std::filesystem::exists(out), "保存文件存在");
    pmx::Model b = pmx::load_pmx(out.string());

    check(a.name == b.name, "模型名一致");
    check(a.vertices.size() == b.vertices.size(), "顶点数一致");
    check(a.indices == b.indices, "索引一致");
    check(a.bones.size() == b.bones.size(), "骨骼数一致");
    check(a.materials.size() == b.materials.size(), "材质数一致");
    check(a.morphs.size() == b.morphs.size(), "形态数一致");
    check(a.rigid.size() == b.rigid.size(), "刚体数一致");
    check(a.joints.size() == b.joints.size(), "关节数一致");
    check(a.textures == b.textures, "贴图表一致");
    check_near(max_abs_diff(a.vertices.pos, b.vertices.pos), 0.0, 1e-6, "顶点坐标");
    check_near(max_abs_diff(a.vertices.nrm, b.vertices.nrm), 0.0, 1e-6, "顶点法线");

    double wmax = 0.0;
    for (std::size_t i = 0; i < a.vertices.weights.size(); ++i) {
        for (int k = 0; k < 4; ++k)
            wmax = std::max(wmax, std::fabs(static_cast<double>(
                                       a.vertices.weights[i][k] - b.vertices.weights[i][k])));
        for (int k = 0; k < 4; ++k)
            check(a.vertices.bones[i][k] == b.vertices.bones[i][k], "权重骨骼索引一致");
        check(a.vertices.wtype[i] == b.vertices.wtype[i], "权重类型一致");
    }
    check_near(wmax, 0.0, 1e-6, "蒙皮权重");

    for (std::size_t i = 0; i < a.bones.size(); ++i) {
        check(a.bones[i].name == b.bones[i].name, "骨骼名一致");
        check(a.bones[i].flags == b.bones[i].flags, "骨骼标志一致");
        check(a.bones[i].parent == b.bones[i].parent, "骨骼父级一致");
    }
    for (std::size_t i = 0; i < a.materials.size(); ++i) {
        check_near(a.materials[i].diffuse.w, b.materials[i].diffuse.w, 1e-6, "材质 alpha");
        check(a.materials[i].face_count == b.materials[i].face_count, "材质面数一致");
    }
    for (std::size_t i = 0; i < a.rigid.size(); ++i) {
        check(a.rigid[i].shape == b.rigid[i].shape, "刚体形状一致");
        check_near(a.rigid[i].size.y, b.rigid[i].size.y, 1e-6, "刚体尺寸");
    }
    const auto ra = pmx::material_ranges(a);
    const auto rb = pmx::material_ranges(b);
    check(ra.size() == rb.size(), "材质区间数一致");
    std::printf("  顶点 %zu，面 %zu，骨骼 %zu，材质 %zu\n", a.vertices.size(), a.n_faces(),
                a.bones.size(), a.materials.size());
}

// ---------------------------------------------------------------- 姿态 / IK
void test_pose(const pmx::Model& model) {
    std::printf("[2] 姿态求值与 IK\n");
    pmx::Pose pose(model);
    pose.reset();
    // world() 返回内部缓冲的引用，比较前必须拷贝快照。
    const std::vector<pmx::Mat4> W0 = pose.world();
    check(W0.size() == static_cast<std::size_t>(pose.n), "世界矩阵数量等于骨骼数");
    bool all_finite = true;
    for (const pmx::Mat4& m : W0) all_finite &= finite_mat(m);
    check(all_finite, "静止世界矩阵无 NaN");

    // 静止姿态下，世界平移即骨骼静止位置
    double pos_err = 0.0;
    for (int i = 0; i < pose.n; ++i) {
        pos_err = std::max(pos_err, static_cast<double>(std::fabs(W0[static_cast<std::size_t>(i)].at(0, 3) - pose.rest_pos[static_cast<std::size_t>(i)].x)));
        pos_err = std::max(pos_err, static_cast<double>(std::fabs(W0[static_cast<std::size_t>(i)].at(1, 3) - pose.rest_pos[static_cast<std::size_t>(i)].y)));
        pos_err = std::max(pos_err, static_cast<double>(std::fabs(W0[static_cast<std::size_t>(i)].at(2, 3) - pose.rest_pos[static_cast<std::size_t>(i)].z)));
    }
    check_near(pos_err, 0.0, 2e-5, "静止世界平移 = 骨骼静止位置");

    if (!model.bones.empty()) {
        // 选一根真正带权重的骨骼，旋转才可能影响顶点。
        int bone = 0;
        for (const auto& b : model.vertices.bones)
            if (b[0] >= 0 && b[0] < pose.n) {
                bone = b[0];
                break;
            }
        pose.invalidate_world();
        pose.rotate_local(bone, pmx::euler_to_quat(0.3f, 0.2f, 0.1f));
        pose.solve_ik();
        const std::vector<pmx::Mat4> W1 = pose.world();
        check(finite_mat(W1[static_cast<std::size_t>(bone)]), "旋转后世界矩阵有效");
        check(W1[static_cast<std::size_t>(bone)].at(0, 0) !=
                  W0[static_cast<std::size_t>(bone)].at(0, 0) ||
              W1[static_cast<std::size_t>(bone)].at(1, 1) !=
                  W0[static_cast<std::size_t>(bone)].at(1, 1),
              "旋转生效");
        pose.reset();
        pose.world();
    }

    // 旋转骨骼只改变末端朝向，骨骼自身的原点（head）必须保持不动。
    // 这是手动摆姿的前提：local_matrix = T(rest_off + trans) * R，平移量由父级决定，
    // 与自身的 R 无关。这里不调用 solve_ik，单独验证姿态求值的这一性质。
    int rot_checked = 0;
    for (int b = 0; b < pose.n && rot_checked < 8; ++b) {
        pose.reset();
        pose.world();
        const pmx::Mat4 mb = pose.world()[static_cast<std::size_t>(b)];
        const pmx::Vec3 head0{mb.at(0, 3), mb.at(1, 3), mb.at(2, 3)};
        pose.rotate_world(b, pmx::Vec3{0.0f, 0.0f, 1.0f}, 0.4f);
        const std::vector<pmx::Mat4> wa = pose.world();
        const pmx::Vec3 head1{wa[static_cast<std::size_t>(b)].at(0, 3),
                              wa[static_cast<std::size_t>(b)].at(1, 3),
                              wa[static_cast<std::size_t>(b)].at(2, 3)};
        check_near(length(head1 - head0), 0.0, 1e-5, "旋转后骨骼原点不动");
        ++rot_checked;
    }
    pose.reset();
    pose.world();

    // IK 收敛：链末端应贴近目标（若模型带 IK）
    int ik_count = 0;
    for (const pmx::Bone& b : model.bones)
        if (b.ik) ++ik_count;
    std::printf("  骨骼 %d，IK 链 %d\n", pose.n, ik_count);
}

// ---------------------------------------------------------------- 骨骼形态
void test_bone_morph(const std::filesystem::path& dir) {
    std::printf("[2b] 骨骼形态（四元数旋转按权重插值）\n");
    pmx::Model model;
    model.name = "bone-morph";
    model.bones.resize(2);
    model.bones[0].name = "root";
    model.bones[0].parent = -1;
    model.bones[1].name = "child";
    model.bones[1].parent = 0;
    model.bones[1].pos = {0.0f, 1.0f, 0.0f};

    pmx::Morph morph;
    morph.name = "骨骼旋转";
    morph.type = 2;
    pmx::MorphOffset offset;
    offset.bone = 1;
    offset.move = {0.0f, 1.0f, 0.0f};
    // 绕 Z 轴 90° 的四元数。
    const float half = pmx::kPi / 4.0f;
    offset.rot = {0.0f, 0.0f, std::sin(half), std::cos(half)};
    morph.offsets.push_back(offset);
    model.morphs.push_back(morph);

    pmx::Pose pose(model);
    pmx::MorphWeights half_weight{{0, 0.5f}};
    pose.apply(&half_weight);
    // apply() 把形态叠加结果写进 rot / trans，base_* 仅保存未叠加的基础姿态。
    const pmx::Quat q = pose.rot[1];
    const float angle = 2.0f * std::acos(std::min(1.0f, std::fabs(q.w)));
    check_near(angle, pmx::kPi / 4.0f, 1e-4, "权重 0.5 时旋转 45°");
    // 位移是相对静止位置的偏移：0 + 0.5 × 1 = 0.5。
    check_near(pose.trans[1].y, 0.5f, 1e-5, "骨骼形态位移按权重叠加");

    pmx::MorphWeights zero_weight{{0, 0.0f}};
    pose.apply(&zero_weight);
    check_near(pose.rot[1].w, 1.0f, 1e-6, "权重 0 时保持原姿态");

    // PMX 里骨骼形态旋转存 4 个浮点（四元数），读写必须保持一致。
    const std::string path = (dir / "pmxstudio_bone_morph.pmx").string();
    pmx::save_pmx(model, path);
    const pmx::Model loaded = pmx::load_pmx(path);
    check(loaded.morphs.size() == 1 && loaded.morphs[0].offsets.size() == 1,
          "骨骼形态偏移读写");
    if (!loaded.morphs.empty() && !loaded.morphs[0].offsets.empty()) {
        const pmx::MorphOffset& back = loaded.morphs[0].offsets[0];
        check_near(back.rot.z, offset.rot.z, 1e-6, "骨骼形态四元数 z 分量 round-trip");
        check_near(back.rot.w, offset.rot.w, 1e-6, "骨骼形态四元数 w 分量 round-trip");
    }
}

// ---------------------------------------------------------------- 蒙皮
void test_bone_category() {
    using pmx::BoneCategory;
    check(pmx::classify_bone("左後髪1-1", "") == BoneCategory::Hair, "頭髮分類");
    check(pmx::classify_bone("刘海", "") == BoneCategory::Hair, "刘海分類");
    check(pmx::classify_bone("左手袖1-1", "") == BoneCategory::Clothing, "袖子優先於四肢");
    check(pmx::classify_bone("右耳環1-1", "") == BoneCategory::Accessory, "耳環優先於體幹");
    check(pmx::classify_bone("左裙星鏈1-3", "") == BoneCategory::Clothing, "裙鏈分類");
    check(pmx::classify_bone("领带1-1", "") == BoneCategory::Clothing, "领带分類");
    check(pmx::classify_bone("右手捩1", "") == BoneCategory::Limbs, "手腕捩骨分類");
    check(pmx::classify_bone("DownTeeth", "") == BoneCategory::Torso, "英文大小写分類");
    check(pmx::classify_bone("左足ＩＫ", "") == BoneCategory::Limbs, "足 IK 分類");
    check(pmx::classify_bone("左ひざ", "") == BoneCategory::Limbs, "膝分類");
    check(pmx::classify_bone("上半身2", "") == BoneCategory::Torso, "上半身分類");
    check(pmx::classify_bone("右目先", "") == BoneCategory::Torso, "眼部歸體幹");
    check(pmx::classify_bone("left elbow", "") == BoneCategory::Limbs, "英文肘分類");
    check(pmx::classify_bone("armor", "") == BoneCategory::Clothing, "armor 不誤判四肢");
    check(pmx::classify_bone("weapon_R", "") == BoneCategory::Prop, "英文武器分類");
    check(pmx::classify_bone("", "sword") == BoneCategory::Prop, "僅英文名也能分類");
    check(pmx::classify_bone("unknown_thing", "") == BoneCategory::Other, "未知分類");
    check(pmx::classify_bone("", "") == BoneCategory::Other, "空名分類");
}

void test_orthographic() {
    // 行主序：平移在最后一列，w 行必须是 (0,0,0,1)。写成 m[12..14] 会让阴影
    // 投影被错误透视除法，斜向光下地面阴影几乎消失。
    const pmx::Mat4 p = pmx::orthographic(-4.0f, 4.0f, -2.0f, 2.0f, 1.0f, 11.0f);
    check_near(p.at(3, 0), 0.0, 1e-6, "正交矩阵 w.x 为 0");
    check_near(p.at(3, 1), 0.0, 1e-6, "正交矩阵 w.y 为 0");
    check_near(p.at(3, 2), 0.0, 1e-6, "正交矩阵 w.z 为 0");
    check_near(p.at(3, 3), 1.0, 1e-6, "正交矩阵 w.w 为 1");
    const pmx::Vec3 hi = pmx::transform_point(p, {4.0f, 2.0f, -1.0f});
    check_near(hi.x, 1.0, 1e-5, "正交矩阵右上角 x");
    check_near(hi.y, 1.0, 1e-5, "正交矩阵右上角 y");
    check_near(hi.z, -1.0, 1e-5, "正交矩阵近平面 z");
    const pmx::Vec3 lo = pmx::transform_point(p, {-4.0f, -2.0f, -11.0f});
    check_near(lo.x, -1.0, 1e-5, "正交矩阵左下角 x");
    check_near(lo.y, -1.0, 1e-5, "正交矩阵左下角 y");
    check_near(lo.z, 1.0, 1e-5, "正交矩阵远平面 z");
}

void test_material_ext_candidates() {
    std::printf("[材质扩展] 贴图配对约定\n");
    const pmx::MaterialExtCandidates c = pmx::material_ext_candidates("/model/tex/body.PNG");
    check(!c.normal.empty() && !c.orm.empty() && !c.metallic.empty(), "材质扩展候选非空");
    const std::string dir = pmx::utf8_string(pmx::utf8_path("/model/tex"));
    check(c.normal.front() == pmx::utf8_string(pmx::utf8_path(dir) / "body_normal.png"),
          "法线贴图按贴图名派生");
    check(c.orm.front() == pmx::utf8_string(pmx::utf8_path(dir) / "body_orm.png"),
          "ORM 贴图按贴图名派生");
    check(c.metallic.front() == pmx::utf8_string(pmx::utf8_path(dir) / "body_metallic.png"),
          "金属度贴图按贴图名派生");
    const auto has = [](const std::vector<std::string>& list, const std::string& name) {
        return std::find(list.begin(), list.end(), name) != list.end();
    };
    check(has(c.normal, pmx::utf8_string(pmx::utf8_path(dir) / "body_n.png")),
          "支持 _n 法线后缀");
    check(has(c.orm, pmx::utf8_string(pmx::utf8_path(dir) / "body_orm.jpg")),
          "候选包含其他图片格式");
    check(pmx::material_ext_candidates("").normal.empty(), "空路径没有候选");
    check(pmx::material_ext_candidates("/m/tex/body").normal.front() ==
              pmx::utf8_string(pmx::utf8_path("/m/tex") / "body_normal.png"),
          "无扩展名的漫反射贴图也能派生");
}

void test_generate_normal_map() {
    std::printf("[材质扩展] 自动生成法线贴图\n");
    std::vector<unsigned char> flat(8 * 8 * 4, 128);
    for (int i = 0; i < 8 * 8; ++i) flat[i * 4 + 3] = 255;
    std::vector<unsigned char> n = pmx::generate_normal_map(flat.data(), 8, 8, 1.0f);
    check(n.size() == 8 * 8 * 4, "生成法线图尺寸一致");
    bool neutral = true;
    for (std::size_t i = 0; i < n.size(); i += 4) {
        if (n[i] != 128 || n[i + 1] != 128 || n[i + 2] != 255 || n[i + 3] != 255)
            neutral = false;
    }
    check(neutral, "纯色贴图生成平面法线");

    std::vector<unsigned char> ramp(8 * 8 * 4);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * 8 + x) * 4;
            const unsigned char v = static_cast<unsigned char>(x * 36);
            ramp[i] = ramp[i + 1] = ramp[i + 2] = v;
            ramp[i + 3] = 255;
        }
    }
    n = pmx::generate_normal_map(ramp.data(), 8, 8, 1.0f);
    const std::size_t c = (static_cast<std::size_t>(4) * 8 + 4) * 4;
    check(n[c] < 120, "亮度向右升高时法线偏向 -u");
    check(n[c + 1] >= 126 && n[c + 1] <= 130, "水平坡度不倾斜 v");
    check(n[c + 2] > 240, "法线 Z 分量保持向前");

    n = pmx::generate_normal_map(ramp.data(), 8, 8, 0.0f);
    check(n[c] == 128 && n[c + 1] == 128, "强度 0 关闭凹凸");
    check(pmx::generate_normal_map(flat.data(), 2, 8, 1.0f).empty(), "尺寸过小不生成");
}

void test_skinning(const pmx::Model& model) {
    std::printf("[3] CPU 参考蒙皮\n");
    pmx::SkinDeformer deform(model);
    pmx::Pose pose(model);
    pose.reset();
    const auto& world = pose.world();
    std::vector<pmx::Vec3> out_pos, out_nrm;
    deform.deform(model.vertices.pos, model.vertices.nrm, world, out_pos, out_nrm);

    check(out_pos.size() == model.vertices.size(), "输出顶点数一致");
    check_near(max_abs_diff(model.vertices.pos, out_pos), 0.0, 2e-5,
               "静止姿态蒙皮 = 原顶点坐标");
    bool nrm_ok = true;
    for (const pmx::Vec3& n : out_nrm)
        nrm_ok &= std::isfinite(n.x) && std::isfinite(n.y) && std::isfinite(n.z);
    check(nrm_ok, "法线无 NaN");

    if (!model.bones.empty()) {
        int bone = 0;
        for (const auto& b : model.vertices.bones)
            if (b[0] >= 0 && b[0] < pose.n) {
                bone = b[0];
                break;
            }
        pose.rotate_local(bone, pmx::euler_to_quat(0.25f, 0.0f, 0.0f));
        pose.invalidate_world();
        const std::vector<pmx::Mat4> w1 = pose.world();
        deform.deform(model.vertices.pos, model.vertices.nrm, w1, out_pos, out_nrm);
        check(max_abs_diff(model.vertices.pos, out_pos) > 1e-4, "旋转后蒙皮产生位移");

        // 局部重算与全量重算一致：dirty 集合要覆盖世界矩阵真正变化的所有骨骼，
        // 即被旋转骨骼自身及其全部后代（子骨骼的世界矩阵会随父级一起改变）。
        std::vector<pmx::Vec3> sub_pos = model.vertices.pos, sub_nrm = model.vertices.nrm;
        std::vector<int> dirty{bone};
        std::vector<char> visited(model.bones.size(), 0);
        visited[static_cast<std::size_t>(bone)] = 1;
        for (std::size_t at = 0; at < dirty.size(); ++at) {
            const int parent = dirty[at];
            for (std::size_t k = 0; k < model.bones.size(); ++k) {
                if (model.bones[k].parent != parent || visited[k]) continue;
                visited[k] = 1;
                dirty.push_back(static_cast<int>(k));
            }
        }
        deform.deform_subset(model.vertices.pos, model.vertices.nrm, w1, dirty, sub_pos, sub_nrm);
        check_near(max_abs_diff(sub_pos, out_pos), 0.0, 2e-4, "局部重算与全量重算一致");
    }
}

// ---------------------------------------------------------------- 物理
void test_physics(pmx::Model& model) {
#ifdef PMX_ENABLE_PHYSICS
    std::printf("[4] Bullet 物理\n");
    pmx::Physics physics;
    if (!physics.build(model)) {
        std::printf("  跳过：%s\n", physics.error.c_str());
        return;
    }
    check(true, "物理世界构建");
    pmx::Pose pose(model);
    pose.reset();
    physics.enabled = true;
    physics.reset(pose, nullptr, true);
    const auto& W = pose.world();
    bool finite = true;
    for (const pmx::Mat4& m : W) finite &= finite_mat(m);
    check(finite, "物理复位后姿态有效");

    for (int i = 0; i < 120; ++i) {
        physics.step(pose, 1.0f / 60.0f);
        pose.apply(nullptr);
        pose.world();
    }
    finite = true;
    for (const pmx::Mat4& m : pose.world()) finite &= finite_mat(m);
    check(finite, "60×2 步后姿态无 NaN");
    check(std::isfinite(physics.last_max_delta), "last_max_delta 有限");
    const std::vector<int>& bones = physics.dynamic_bones();
    std::printf("  刚体 %zu，动态绑定骨骼 %zu，最大位移 %.6f\n", model.rigid.size(),
                bones.size(), static_cast<double>(physics.last_max_delta));
#else
    std::printf("[4] 物理：编译时已关闭（PMX_ENABLE_PHYSICS=OFF）\n");
#endif
}

// ---------------------------------------------------------------- VPD 导出
void test_vpd(const pmx::Model& model, const std::filesystem::path& dir) {
    std::printf("[5] VPD 导出与回读\n");
    if (model.bones.empty()) {
        check(false, "模型含骨骼");
        return;
    }
    pmx::Pose pose(model);
    pose.reset();
    const int bone = 0;
    pose.base_rot[static_cast<std::size_t>(bone)] = pmx::euler_to_quat(0.2f, -0.3f, 0.1f);
    pose.base_trans[static_cast<std::size_t>(bone)] = pmx::Vec3{0.5f, 1.0f, -0.25f};
    pmx::MorphWeights weights;
    const bool has_morph = !model.morphs.empty();
    if (has_morph) weights.emplace_back(0, 0.75f);

    const std::string text = pmx::build_vpd_text(model, pose, &weights);
    check(text.rfind("Vocaloid Pose Data file", 0) == 0, "VPD 头部正确");
    check(text.find("Bone0{") != std::string::npos, "含骨骼块");
    if (has_morph) check(text.find("Morph0{") != std::string::npos, "含形态块");

    const pmx::VpdFile parsed = pmx::parse_vpd_text(text, "inline", "utf-8");
    check(parsed.bones.size() == 1, "内存回读骨骼数 = 1");
    if (!parsed.bones.empty()) {
        check_near(parsed.bones[0].pos.x, 0.5, 1e-5, "位移 x 回读");
        check_near(parsed.bones[0].pos.z, -0.25, 1e-5, "位移 z 回读");
        check_near(parsed.bones[0].rot.w, pose.base_rot[static_cast<std::size_t>(bone)].w,
                   1e-5, "旋转 w 回读");
    }
    if (has_morph) {
        check(parsed.morphs.size() == 1, "内存回读形态数 = 1");
        if (!parsed.morphs.empty())
            check_near(parsed.morphs[0].weight, 0.75, 1e-5, "形态权重回读");
    }

    const std::string path = (dir / "pmxstudio_export.vpd").string();
    const pmx::VpdExport result = pmx::save_vpd(path, model, pose, &weights);
    check(std::filesystem::exists(path), "VPD 文件写出");
    check(result.bones == 1, "导出骨骼计数");
    check(result.morphs == (has_morph ? 1 : 0), "导出形态计数");
    check(result.encoding == "cp932" || result.encoding == "utf-8", "导出编码标记有效");

    const pmx::VpdFile disk = pmx::load_vpd(path);
    check(disk.bones.size() == 1, "磁盘回读骨骼数 = 1");
    pmx::Pose pose2(model);
    pose2.reset();
    pmx::MorphWeights weights2;
    pmx::apply_vpd(model, pose2, disk, &weights2);
    check_near(pose2.base_trans[static_cast<std::size_t>(bone)].x, 0.5, 2e-4,
               "应用回读 VPD 的位移");
    if (has_morph)
        check_near(weights2.empty() ? 0.0f : weights2[0].second, 0.75, 2e-4,
                   "应用回读 VPD 的形态权重");

    // 静止姿态不应导出任何骨骼块
    pmx::Pose idle(model);
    idle.reset();
    const std::string idle_text = pmx::build_vpd_text(model, idle);
    check(idle_text.find("Bone0{") == std::string::npos, "静止姿态默认不导出骨骼");
    const std::string all_text = pmx::build_vpd_text(model, idle, nullptr, "", true);
    check(all_text.find("Bone0{") != std::string::npos, "include_all 导出全部骨骼");
}

// ---------------------------------------------------------------- VMD 导出与采样
void test_vmd(const pmx::Model& model, const std::filesystem::path& dir) {
    std::printf("[7] VMD 导出、解析与采样\n");
    if (model.bones.empty()) {
        check(false, "模型含骨骼");
        return;
    }
    const int bone = 0;
    pmx::Pose pose(model);
    pose.reset();
    pose.base_rot[static_cast<std::size_t>(bone)] = pmx::euler_to_quat(0.1f, 0.2f, 0.3f);
    pose.base_trans[static_cast<std::size_t>(bone)] = pmx::Vec3{0.4f, 0.5f, 0.6f};
    pmx::MorphWeights weights;
    const bool has_morph = !model.morphs.empty();
    if (has_morph) weights.emplace_back(0, 0.6f);

    const std::string path = (dir / "pmxstudio_export.vmd").string();
    const pmx::VmdExport result = pmx::save_vmd(path, model, pose, &weights);
    check(std::filesystem::exists(path), "VMD 文件写出");
    check(result.bones == 1, "导出骨骼轨道数 = 1");
    check(result.morphs == (has_morph ? 1 : 0), "导出形态轨道数");

    const pmx::VmdMotion motion = pmx::load_vmd(path);
    check(motion.version == "0002", "VMD 版本 0002");
    check(motion.bone_tracks.size() == 1, "回读骨骼轨道数 = 1");
    if (!motion.bone_tracks.empty()) {
        check(motion.bone_tracks[0].keys.size() == 1, "回读关键帧数 = 1");
        check(motion.bone_tracks[0].name == model.bones[static_cast<std::size_t>(bone)].name,
              "回读骨骼名");
        check_near(motion.bone_tracks[0].keys[0].pos.x, 0.4, 1e-5, "回读位移 x");
        check_near(motion.bone_tracks[0].keys[0].rot.w,
                   pose.base_rot[static_cast<std::size_t>(bone)].w, 1e-5, "回读旋转 w");
    }
    if (has_morph) {
        check(motion.morph_tracks.size() == 1, "回读形态轨道数 = 1");
        if (!motion.morph_tracks.empty())
            check_near(motion.morph_tracks[0].keys[0].weight, 0.6, 1e-5, "回读形态权重");
    }

    const pmx::VmdBinding binding = pmx::bind_vmd(model, motion);
    check(binding.bone_matched == 1, "绑定到模型骨骼");
    if (has_morph) check(binding.morph_matched == 1, "绑定到模型形态");
    pmx::Pose target(model);
    pmx::MorphWeights out;
    pmx::evaluate_vmd(motion, binding, 0.0, target, &out);
    check_near(target.base_trans[static_cast<std::size_t>(bone)].x, 0.4, 1e-5, "采样位移 x");
    check_near(target.base_rot[static_cast<std::size_t>(bone)].w,
               pose.base_rot[static_cast<std::size_t>(bone)].w, 1e-5, "采样旋转 w");
    if (has_morph) {
        check(out.size() == 1, "采样形态数 = 1");
        if (!out.empty()) check_near(out[0].second, 0.6, 1e-5, "采样形态权重");
    }
    if (model.bones.size() > 1)
        check(target.base_trans[1].x == 0.0f && target.base_trans[1].y == 0.0f,
              "未绑定骨骼保持静止");

    // 两关键帧：线性缓动下第 15 帧应为前后值的平均。
    pmx::VmdMotion synth;
    synth.model_name = "synth";
    pmx::VmdBoneTrack track;
    track.name = model.bones[static_cast<std::size_t>(bone)].name;
    track.keys.push_back(pmx::VmdBoneKey{0, pmx::Vec3{}, pmx::identity_quat(), {}});
    pmx::VmdBoneKey end_key;
    end_key.frame = 30;
    end_key.pos = pmx::Vec3{1.0f, 0.0f, 0.0f};
    end_key.rot = pmx::euler_to_quat(0.0f, pmx::kPi, 0.0f);
    track.keys.push_back(end_key);
    synth.bone_tracks.push_back(track);
    if (has_morph) {
        pmx::VmdMorphTrack morph_track;
        morph_track.name = model.morphs[0].name;
        morph_track.keys.push_back(pmx::VmdMorphKey{0, 0.0f});
        morph_track.keys.push_back(pmx::VmdMorphKey{30, 1.0f});
        synth.morph_tracks.push_back(morph_track);
    }
    synth.max_frame = 30;
    const pmx::VmdBinding synth_binding = pmx::bind_vmd(model, synth);
    check(synth_binding.bone_matched == 1, "合成动作绑定骨骼");
    pmx::Pose mid(model);
    pmx::MorphWeights mid_weights;
    pmx::evaluate_vmd(synth, synth_binding, 15.0, mid, &mid_weights);
    check_near(mid.base_trans[static_cast<std::size_t>(bone)].x, 0.5, 1e-5, "线性插值位移中点");
    check_near(mid.base_rot[static_cast<std::size_t>(bone)].w, std::sqrt(0.5), 1e-4,
               "四元数半程插值");
    if (has_morph && !mid_weights.empty())
        check_near(mid_weights[0].second, 0.5, 1e-5, "形态线性插值");

    // 未匹配名称进入 missing 列表。
    pmx::VmdMotion unknown;
    pmx::VmdBoneTrack unknown_track;
    unknown_track.name = "不存在的骨骼";
    unknown_track.keys.push_back(pmx::VmdBoneKey{0, pmx::Vec3{}, pmx::identity_quat(), {}});
    unknown.bone_tracks.push_back(unknown_track);
    const pmx::VmdBinding unknown_binding = pmx::bind_vmd(model, unknown);
    check(unknown_binding.bone_matched == 0, "未知骨骼不绑定");
    check(unknown_binding.missing_bones.size() == 1, "未知骨骼记入未匹配");

    // 静止姿态全量导出：应包含模型全部骨骼。
    pmx::Pose idle(model);
    idle.reset();
    const std::string all_path = (dir / "pmxstudio_export_all.vmd").string();
    const pmx::VmdExport all = pmx::save_vmd(all_path, model, idle, nullptr, "", true);
    check(all.bones == static_cast<int>(model.bones.size()), "全量导出包含全部骨骼");
}

void test_utf8_paths(const pmx::Model& model, const std::filesystem::path& dir) {
    std::printf("[6] 中文路径读写\n");
    const std::string sub_dir =
        pmx::utf8_string(dir) + "/" + pmx::utf8_string(pmx::utf8_path(std::string("中文目录")));
    std::filesystem::create_directories(pmx::utf8_path(sub_dir));
    const std::string path =
        sub_dir + "/" + pmx::utf8_string(pmx::utf8_path(std::string("模型 副本.pmx")));

    pmx::Model copy = model;
    pmx::save_pmx(copy, path);
    check(pmx::path_exists(path), "中文路径写出 PMX");
    const pmx::Model loaded = pmx::load_pmx(path);
    check(loaded.vertices.size() == model.vertices.size(), "中文路径读回顶点数");
    check(loaded.bones.size() == model.bones.size(), "中文路径读回骨骼数");

    // 纹理路径解析要保留 UTF-8 目录，不能被窄字符编码截断。
    pmx::Model probe = model;
    probe.path = sub_dir + "/probe.pmx";
    probe.textures = {"tex\\a.png"};
    std::string resolved = probe.resolve_texture(0);
    for (char& c : resolved)
        if (c == '\\') c = '/';
    check(resolved.find("中文目录") != std::string::npos, "纹理路径保留中文目录");
    check(resolved.find("/tex/a.png") != std::string::npos, "纹理解析替换反斜杠");
}

// ---------------------------------------------------------------- 肉体变形
void test_flesh() {
    std::printf("[9] 接触式肉体变形\n");
    pmx::Model m;
    const int grid = 5;
    const float spacing = 0.25f;
    // 两张面对面、相距 0.005 的 5×5 网格，模拟压在一起的两块表面。
    auto add_sheet = [&](float z, float nz) {
        const int base = static_cast<int>(m.vertices.pos.size());
        for (int y = 0; y < grid; ++y) {
            for (int x = 0; x < grid; ++x) {
                m.vertices.pos.push_back({x * spacing, y * spacing, z});
                m.vertices.nrm.push_back({0.0f, 0.0f, nz});
                m.vertices.uv.push_back({0.0f, 0.0f});
                m.vertices.wtype.push_back(pmx::WT_BDEF1);
                m.vertices.bones.push_back({0, 0, 0, 0});
                m.vertices.weights.push_back({1.0f, 0.0f, 0.0f, 0.0f});
            }
        }
        for (int y = 0; y + 1 < grid; ++y) {
            for (int x = 0; x + 1 < grid; ++x) {
                const int i0 = base + y * grid + x;
                const int i1 = i0 + 1;
                const int i2 = i0 + grid;
                const int i3 = i2 + 1;
                m.indices.insert(m.indices.end(),
                                 {static_cast<std::uint32_t>(i0), static_cast<std::uint32_t>(i1),
                                  static_cast<std::uint32_t>(i2), static_cast<std::uint32_t>(i1),
                                  static_cast<std::uint32_t>(i3), static_cast<std::uint32_t>(i2)});
            }
        }
    };
    add_sheet(0.0f, 1.0f);
    add_sheet(0.005f, -1.0f);

    pmx::FleshSolver solver;
    solver.reset(m);
    check(solver.valid(), "焊接拓扑有效");
    check_near(solver.height(), 1.0, 1e-5, "身高取竖直包围盒");

    pmx::FleshParams params;
    params.range = 0.1f;  // 身高 1.0 → 接触距离 0.1
    params.dent = 0.6f;
    params.bulge = 0.0f;

    auto pos = m.vertices.pos;
    auto nrm = m.vertices.nrm;
    solver.apply(pos, nrm, params);
    check(pos[0].z < -1e-3f, "法线 +Z 的表面被压入");
    check(pos[static_cast<std::size_t>(grid * grid)].z > 0.005f + 1e-3f,
          "法线 -Z 的表面被压入");

    bool normals_ok = true;
    for (const pmx::Vec3& n : nrm) {
        const float len = pmx::length(n);
        if (!std::isfinite(len) || std::fabs(len - 1.0f) > 1e-3f) normals_ok = false;
    }
    check(normals_ok, "形变后法线归一化");

    // 硬度 1 / 区域硬度 1 都不应该产生位移。
    pmx::FleshParams hard = params;
    hard.firmness = 1.0f;
    auto stiff = m.vertices.pos;
    auto stiff_n = m.vertices.nrm;
    solver.apply(stiff, stiff_n, hard);
    check(max_abs_diff(stiff, m.vertices.pos) < 1e-6, "整体硬度 1 时不形变");

    pmx::FleshParams region = params;
    region.region_bone = 0;
    region.region_firmness = 1.0f;
    auto rigid_region = m.vertices.pos;
    auto rigid_n = m.vertices.nrm;
    solver.apply(rigid_region, rigid_n, region);
    check(max_abs_diff(rigid_region, m.vertices.pos) < 1e-6, "区域硬度 1 时不形变");

    // 默认参数（接触距离 1.2% 身高）在这两块相距 0.5% 的面上不应产生任何形变。
    pmx::FleshParams defaults;
    auto rest = m.vertices.pos;
    auto rest_n = m.vertices.nrm;
    solver.apply(rest, rest_n, defaults);
    check(max_abs_diff(rest, m.vertices.pos) < 1e-6, "默认参数静止姿态无形变");

    // 只是靠近、并未压到表面：即使间隙小于接触距离也不应该形变。
    // 否则衣物、头发和并拢的肢体在静止时都会被判为接触，表现为全身膨胀。
    pmx::Model apart = m;
    for (std::size_t i = static_cast<std::size_t>(grid * grid);
         i < apart.vertices.pos.size(); ++i)
        apart.vertices.pos[i].z += 0.045f;  // 两片间距 0.05
    pmx::FleshSolver apart_solver;
    apart_solver.reset(apart);
    auto apart_pos = apart.vertices.pos;
    auto apart_n = apart.vertices.nrm;
    apart_solver.apply(apart_pos, apart_n, params);
    check(max_abs_diff(apart_pos, apart.vertices.pos) < 1e-6,
          "仅邻近未压入表面时不形变");

    // Corner Taper：只应该回收隆起，不应放大位移。
    pmx::FleshParams bumped = params;
    bumped.bulge = 1.0f;
    bumped.taper = 0.0f;
    bumped.threads = 1;
    auto open = m.vertices.pos;
    auto open_n = m.vertices.nrm;
    solver.apply(open, open_n, bumped);
    pmx::FleshParams tapered = bumped;
    tapered.taper = 1.0f;
    auto closed = m.vertices.pos;
    auto closed_n = m.vertices.nrm;
    solver.apply(closed, closed_n, tapered);
    check(max_abs_diff(open, closed) > 1e-4, "Corner Taper 改变了隆起分布");
    // 折痕端部的隆起回收后，最大外翻量不应该变大。
    float max_open = 0.0f;
    float max_closed = 0.0f;
    for (std::size_t i = 0; i < m.vertices.pos.size(); ++i) {
        const pmx::Vec3 d0 = open[i] - m.vertices.pos[i];
        const pmx::Vec3 d1 = closed[i] - m.vertices.pos[i];
        max_open = std::max(max_open, d0.z * m.vertices.nrm[i].z);
        max_closed = std::max(max_closed, d1.z * m.vertices.nrm[i].z);
    }
    check(max_closed <= max_open + 1e-5f, "Corner Taper 不会放大外翻隆起");

    // 多线程结果必须与单线程一致（线程局部数组按 max 合并）。
    auto mt = m.vertices.pos;
    auto mt_n = m.vertices.nrm;
    pmx::FleshParams threaded = bumped;
    threaded.threads = 4;
    threaded.taper = 0.55f;
    solver.apply(mt, mt_n, threaded);
    auto st = m.vertices.pos;
    auto st_n = m.vertices.nrm;
    pmx::FleshParams single = threaded;
    single.threads = 1;
    solver.apply(st, st_n, single);
    check(max_abs_diff(mt, st) < 1e-6, "肉体求解多线程结果一致");

    // 外部碰撞体：球心放在两张面之间，靠近球心的顶点被推到球面上。
    pmx::FleshParams collision = params;
    collision.dent = 0.0f;
    collision.bulge = 0.0f;
    collision.taper = 0.0f;
    collision.threads = 1;
    pmx::FleshColliderShape ball;
    ball.a = {0.5f, 0.5f, 0.0025f};
    ball.radius = 0.15f;
    auto pushed = m.vertices.pos;
    auto pushed_n = m.vertices.nrm;
    solver.apply(pushed, pushed_n, collision, {ball});
    // 索引 12 是下表面的中心顶点 (0.5, 0.5, 0)。
    const std::size_t center = static_cast<std::size_t>(2 * grid + 2);
    check(pushed[center].z < -0.1f, "球碰撞体把顶点推到表面");
    check(std::fabs(pushed[0].z) < 1e-4f, "远离碰撞体的顶点不受影响");

    // 碰撞体硬度 1：完全不推。
    pmx::FleshColliderShape hard_ball = ball;
    hard_ball.firmness = 1.0f;
    auto held = m.vertices.pos;
    auto held_n = m.vertices.nrm;
    solver.apply(held, held_n, collision, {hard_ball});
    check(max_abs_diff(held, m.vertices.pos) < 1e-6, "碰撞体硬度 1 时不形变");

    // 胶囊碰撞体：轴线经过下表面，靠近轴线的顶点被推开。
    pmx::FleshColliderShape capsule;
    capsule.a = {0.25f, 0.0f, 0.0025f};
    capsule.b = {0.75f, 0.0f, 0.0025f};
    capsule.radius = 0.1f;
    capsule.capsule = true;
    auto cap = m.vertices.pos;
    auto cap_n = m.vertices.nrm;
    solver.apply(cap, cap_n, collision, {capsule});
    const std::size_t on_axis = static_cast<std::size_t>(2);
    check(cap[on_axis].z < -1e-3f, "胶囊碰撞体推开轴线附近的顶点");
    check(std::fabs(cap[static_cast<std::size_t>(grid * grid - 1)].z) < 1e-4f,
          "胶囊碰撞体不影响远端顶点");

    // Smooth Pops：第一帧记录位移，第二帧只走一半。
    std::vector<pmx::Vec3> skinned = m.vertices.pos;
    std::vector<pmx::Vec3> smooth_pos(skinned.size());
    std::vector<pmx::Vec3> prev;
    for (std::size_t i = 0; i < skinned.size(); ++i)
        smooth_pos[i] = skinned[i] + pmx::Vec3{0.0f, 0.0f, 0.2f};
    pmx::smooth_flesh_pops(skinned, smooth_pos, prev, 0.5f);
    check(std::fabs(smooth_pos[0].z - 0.2f) < 1e-5f, "Smooth Pops 首帧保留原位移");
    for (std::size_t i = 0; i < skinned.size(); ++i)
        smooth_pos[i] = skinned[i] + pmx::Vec3{0.0f, 0.0f, 0.4f};
    pmx::smooth_flesh_pops(skinned, smooth_pos, prev, 0.5f);
    check(std::fabs(smooth_pos[0].z - 0.3f) < 1e-5f, "Smooth Pops 第二帧向新位移收敛");
}

}  // namespace

int main(int argc, char** argv) {
    // 崩溃时也要保留已打印的进度，便于定位失败项。
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // Windows 的 argv 是 ANSI 编码，中文路径参数需要先还原成 UTF-8。
    const std::vector<std::string> args = pmx::utf8_args(argc, argv);
    const std::string path = args.size() > 1 ? args[1] : PMX_TEST_MODEL;
    if (!std::filesystem::exists(pmx::utf8_path(path))) {
        std::fprintf(stderr, "找不到测试模型：%s\n", path.c_str());
        return 2;
    }
    pmx::Model model;
    try {
        model = pmx::load_pmx(path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "解析失败：%s\n", e.what());
        return 2;
    }

    const std::filesystem::path out =
        std::filesystem::temp_directory_path() / "pmxstudio_roundtrip.pmx";
    try {
        test_roundtrip(path, out);
        test_pose(model);
        test_bone_morph(out.parent_path());
        test_bone_category();
        test_orthographic();
        test_material_ext_candidates();
        test_generate_normal_map();
        test_skinning(model);
        test_flesh();
        test_physics(model);
        test_vpd(model, out.parent_path());
        test_utf8_paths(model, out.parent_path());
        test_vmd(model, out.parent_path());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "异常：%s\n", e.what());
        return 1;
    }

    std::printf("\n共 %d 项检查，失败 %d 项\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
