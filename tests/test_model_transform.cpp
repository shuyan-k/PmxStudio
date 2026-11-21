// 多模型摆放变换：缩放 / 绕 Y 旋转 / 平移是否正确作用到网格、骨骼与物理数据。
#include <array>
#include <cmath>
#include <cstdio>

#include "core/math.hpp"
#include "core/model_transform.hpp"
#include "core/pmx.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

void check_near(float got, float want, float tol, const char* what) {
    ++g_checks;
    if (!(std::fabs(got - want) <= tol)) {
        ++g_failures;
        std::fprintf(stderr, "  [失败] %s: %.6f != %.6f (tol %.2e)\n", what, got, want,
                     tol);
    }
}

void check_vec(pmx::Vec3 got, pmx::Vec3 want, const char* what) {
    check_near(got.x, want.x, 1e-4f, what);
    check_near(got.y, want.y, 1e-4f, what);
    check_near(got.z, want.z, 1e-4f, what);
}

pmx::Model make_model() {
    pmx::Model m;
    m.name = "transform_test";
    m.vertices.pos = {{1.0f, 0.0f, 0.0f}, {0.0f, 2.0f, 0.0f}};
    m.vertices.nrm = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    m.vertices.sdef.assign(2, std::array<float, 9>{});
    // SDEF：中心 / R0 / R1 三个点。
    m.vertices.sdef[0] = {1.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f};

    pmx::Bone bone;
    bone.pos = {1.0f, 0.0f, 0.0f};
    bone.tail_offset = {0.0f, 1.0f, 0.0f};
    bone.axis = {1.0f, 0.0f, 0.0f};
    m.bones.push_back(bone);

    pmx::RigidBody body;
    body.pos = {1.0f, 3.0f, 0.0f};
    body.rot = {0.0f, 10.0f, 0.0f};
    body.size = {0.5f, 1.0f, 0.25f};
    m.rigid.push_back(body);

    pmx::Joint joint;
    joint.pos = {2.0f, 0.0f, 0.0f};
    joint.rot = {5.0f, 20.0f, 0.0f};
    joint.min_loc = {-1.0f, -2.0f, -3.0f};
    m.joints.push_back(joint);

    pmx::Morph morph;
    morph.type = 1;  // 顶点偏移
    pmx::MorphOffset offset{};
    offset.offset = {1.0f, 0.0f, 0.0f, 0.0f};
    morph.offsets.push_back(offset);
    m.morphs.push_back(morph);
    return m;
}

}  // namespace

int main() {
    std::printf("[1] 缩放 + 绕 Y 旋转 90° + 平移\n");
    pmx::Model m = make_model();
    pmx::transform_model(m, {10.0f, 0.0f, 0.0f}, 90.0f, 2.0f);
    check_vec(m.vertices.pos[0], {10.0f, 0.0f, -2.0f}, "顶点 0");
    check_vec(m.vertices.pos[1], {10.0f, 4.0f, 0.0f}, "顶点 1");
    check_vec(m.vertices.nrm[0], {0.0f, 0.0f, -1.0f}, "法线 0 随旋转且保持单位长度");
    check_vec(m.vertices.nrm[1], {0.0f, 1.0f, 0.0f}, "法线 1");
    // SDEF 三个点都按同样的变换走。
    check_vec({m.vertices.sdef[0][0], m.vertices.sdef[0][1], m.vertices.sdef[0][2]},
              {10.0f, 0.0f, -2.0f}, "SDEF 中心");
    check_vec({m.vertices.sdef[0][3], m.vertices.sdef[0][4], m.vertices.sdef[0][5]},
              {10.0f, 0.0f, -4.0f}, "SDEF R0");

    check_vec(m.bones[0].pos, {10.0f, 0.0f, -2.0f}, "骨骼位置");
    check_vec(m.bones[0].tail_offset, {0.0f, 2.0f, 0.0f}, "骨骼尾向偏移");
    check_vec(m.bones[0].axis, {0.0f, 0.0f, -1.0f}, "骨骼固定轴");

    check_vec(m.rigid[0].pos, {10.0f, 6.0f, -2.0f}, "刚体位置");
    check_near(m.rigid[0].rot.y, 100.0f, 1e-4f, "刚体 Y 欧拉角叠加");
    check_vec(m.rigid[0].size, {1.0f, 2.0f, 0.5f}, "刚体尺寸缩放");

    check_vec(m.joints[0].pos, {10.0f, 0.0f, -4.0f}, "关节位置");
    check_near(m.joints[0].rot.y, 110.0f, 1e-4f, "关节 Y 欧拉角叠加");
    check_vec(m.joints[0].min_loc, {-2.0f, -4.0f, -6.0f}, "关节限位缩放");

    check_vec({m.morphs[0].offsets[0].offset.x, m.morphs[0].offsets[0].offset.y,
               m.morphs[0].offsets[0].offset.z},
              {0.0f, 0.0f, -2.0f}, "顶点形态偏移");

    std::printf("[2] 恒等变换不改变模型\n");
    pmx::Model same = make_model();
    const pmx::Model before = make_model();
    pmx::transform_model(same, {0.0f, 0.0f, 0.0f}, 0.0f, 1.0f);
    check_vec(same.vertices.pos[0], before.vertices.pos[0], "顶点不变");
    check_vec(same.bones[0].axis, before.bones[0].axis, "骨骼轴不变");
    check_near(same.rigid[0].rot.y, before.rigid[0].rot.y, 1e-5f, "刚体旋转不变");

    std::printf("[3] 非法缩放回退为 1\n");
    pmx::Model bad = make_model();
    pmx::transform_model(bad, {0.0f, 0.0f, 0.0f}, 0.0f, 0.0f);
    check_vec(bad.vertices.pos[0], before.vertices.pos[0], "缩放 0 按 1 处理");

    std::printf("\n%d 项检查，%d 项失败\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
