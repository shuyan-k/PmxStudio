#include <algorithm>
#include <cmath>
#include <string>

#include "app/viewer.hpp"
#include "core/math.hpp"
#include "core/pmx.hpp"
#include "core/skinning.hpp"

namespace {

constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;

// 附件在静止姿态下的世界变换：先旋转、再平移（缩放已烘焙进 accessory 副本，
// Pose 没有缩放通道，缩放必须落在几何与骨骼上）。
pmx::Mat4 accessory_matrix(const Viewer& v) {
    const pmx::Quat q = pmx::euler_to_quat(v.accessory_rot[0] * kDegToRad,
                                          v.accessory_rot[1] * kDegToRad,
                                          v.accessory_rot[2] * kDegToRad);
    return pmx::from_quat_pos(q, pmx::Vec3{v.accessory_pos[0], v.accessory_pos[1],
                                          v.accessory_pos[2]});
}

// 附件顶点直接落在世界坐标：蒙皮矩阵是 W * T(-rest)，静止时 W = T(rest)，
// 因此世界坐标即静止坐标，骨骼运动后由矩阵带出去。
void write_accessory_vertices(Viewer& v) {
    const std::size_t base = v.accessory_vertex_base;
    const std::size_t na = v.accessory.vertices.pos.size();
    if (base == 0 || na == 0) return;
    if (v.render_model.vertices.size() != base + na) return;

    const pmx::Mat4 xf = accessory_matrix(v);
    pmx::VertexData& dst = v.render_model.vertices;
    const pmx::VertexData& src = v.accessory.vertices;
    for (std::size_t i = 0; i < na; ++i) {
        dst.pos[base + i] = pmx::transform_point(xf, src.pos[i]);
        pmx::Vec3 n = pmx::transform_dir(xf, src.nrm[i]);
        const float len = pmx::length(n);
        dst.nrm[base + i] = len > 1e-8f ? n * (1.0f / len) : n;
    }
}

}  // namespace

int Viewer::default_accessory_bone() const {
    if (model.bones.empty()) return -1;
    // 优先精确的「頭 / head」，其次包含这些字的骨骼（如「頭.先」），实在没有就用 0 号。
    for (const char* want : {"頭", "头", "head", "Head"}) {
        for (std::size_t i = 0; i < model.bones.size(); ++i) {
            if (model.bones[i].name == want || model.bones[i].name_en == want)
                return static_cast<int>(i);
        }
    }
    for (std::size_t i = 0; i < model.bones.size(); ++i) {
        const std::string& n = model.bones[i].name;
        if (n.find("頭") != std::string::npos || n.find("头") != std::string::npos)
            return static_cast<int>(i);
    }
    return 0;
}

void Viewer::rebuild_render_model() {
    accessory_vertex_base = 0;
    if (!accessory_loaded || accessory.vertices.pos.size() == 0 || model.vertices.size() == 0) {
        render_model = pmx::Model{};
        return;
    }

    render_model = model;
    const std::size_t nv = model.vertices.size();
    const std::size_t na = accessory.vertices.pos.size();
    accessory_vertex_base = nv;

    // 附件顶点全部 100% 绑定到锚点骨骼（BDEF1）。
    const int anchor =
        (accessory_bone >= 0 && accessory_bone < static_cast<int>(model.bones.size()))
            ? accessory_bone
            : 0;

    pmx::VertexData& dst = render_model.vertices;
    dst.pos.resize(nv + na);
    dst.nrm.resize(nv + na);
    dst.uv.resize(nv + na);
    dst.edge.resize(nv + na);
    dst.wtype.resize(nv + na);
    dst.bones.resize(nv + na);
    dst.weights.resize(nv + na);
    dst.sdef.resize(nv + na);
    const std::size_t uv_stride = static_cast<std::size_t>(std::max(model.add_uv_count, 0)) * 4;
    dst.add_uv.resize((nv + na) * uv_stride, 0.0f);

    // 附加 UV：两边通道数不同时按较小的复制，缺的补 0。
    const int copy_uv =
        std::min(model.add_uv_count, std::max(accessory.add_uv_count, 0));
    const std::size_t src_uv_stride =
        static_cast<std::size_t>(std::max(accessory.add_uv_count, 0)) * 4;

    const pmx::VertexData& src = accessory.vertices;
    for (std::size_t i = 0; i < na; ++i) {
        if (i < src.uv.size()) dst.uv[nv + i] = src.uv[i];
        dst.edge[nv + i] = i < src.edge.size() ? src.edge[i] : 1.0f;
        dst.wtype[nv + i] = pmx::WT_BDEF1;
        dst.bones[nv + i] = {anchor, 0, 0, 0};
        dst.weights[nv + i] = {1.0f, 0.0f, 0.0f, 0.0f};
        dst.sdef[nv + i] = {};
        for (int c = 0; c < copy_uv; ++c) {
            for (int k = 0; k < 4; ++k) {
                dst.add_uv[(nv + i) * uv_stride + static_cast<std::size_t>(c) * 4 +
                           static_cast<std::size_t>(k)] =
                    src.add_uv[i * src_uv_stride + static_cast<std::size_t>(c) * 4 +
                               static_cast<std::size_t>(k)];
            }
        }
    }

    const std::size_t ib = render_model.indices.size();
    render_model.indices.resize(ib + accessory.indices.size());
    for (std::size_t i = 0; i < accessory.indices.size(); ++i) {
        render_model.indices[ib + i] =
            accessory.indices[i] + static_cast<std::uint32_t>(nv);
    }

    // 纹理统一换成绝对路径：附件与主模型可能不在同一目录。
    const int tex_base = static_cast<int>(render_model.textures.size());
    for (std::size_t t = 0; t < accessory.textures.size(); ++t) {
        render_model.textures.push_back(accessory.resolve_texture(static_cast<int>(t)));
    }

    for (const pmx::Material& src_mat : accessory.materials) {
        pmx::Material mt = src_mat;
        if (mt.texture >= 0) mt.texture += tex_base;
        if (mt.sphere >= 0) mt.sphere += tex_base;
        // toon_shared == 0 时 toon 是纹理索引，否则是内置 toon（0..9）。
        if (mt.toon_shared == 0 && mt.toon >= 0) mt.toon += tex_base;
        render_model.materials.push_back(mt);
    }

    write_accessory_vertices(*this);
}

void Viewer::apply_accessory_structure() {
    rebuild_render_model();
    accessory_reset = true;
    if (!pose) return;
    std::string err;
    if (!renderer.upload_model(active_model(), &err)) {
        error = err;
        return;
    }
    apply_morphs();
}

void Viewer::rebuild_accessory_scaled() {
    accessory_pose.reset();
    accessory_skin.reset();
#ifdef PMX_ENABLE_PHYSICS
    accessory_physics.reset();
#endif
    accessory_physics_active = false;
    accessory_skin_pos.clear();
    accessory_skin_nrm.clear();
    accessory_reset = true;
    if (!accessory_loaded) return;

    // 缩放必须烘焙进几何与骨骼：Pose / Physics 都没有缩放通道。
    accessory = accessory_base;
    const float s = std::clamp(accessory_scale, 0.01f, 100.0f);
    accessory_scale = s;
    if (std::fabs(s - 1.0f) > 1e-6f) {
        for (pmx::Vec3& p : accessory.vertices.pos) p = p * s;
        for (auto& sd : accessory.vertices.sdef)
            for (float& v : sd) v *= s;  // C / R0 / R1 都是长度量
        for (pmx::Bone& b : accessory.bones) {
            b.pos = b.pos * s;
            b.tail_offset = b.tail_offset * s;
        }
        for (pmx::RigidBody& r : accessory.rigid) {
            r.pos = r.pos * s;
            r.size = r.size * s;
        }
        for (pmx::Joint& j : accessory.joints) {
            j.pos = j.pos * s;
            j.min_loc = j.min_loc * s;
            j.max_loc = j.max_loc * s;
            j.spring_loc = j.spring_loc * s;
        }
        for (pmx::Morph& mo : accessory.morphs) {
            if (mo.type != 1) continue;  // 只缩放顶点形态的位移
            for (pmx::MorphOffset& off : mo.offsets) {
                off.offset.x *= s;
                off.offset.y *= s;
                off.offset.z *= s;
            }
        }
    }

    if (accessory.bones.empty() || accessory.vertices.pos.empty()) return;
    accessory_pose = std::make_unique<pmx::Pose>(accessory);
    accessory_skin = std::make_unique<pmx::SkinDeformer>(accessory);
    accessory_skin_pos.assign(accessory.vertices.pos.size(), pmx::Vec3{});
    accessory_skin_nrm.assign(accessory.vertices.pos.size(), pmx::Vec3{});
#ifdef PMX_ENABLE_PHYSICS
    // 附件自带刚体时建独立物理世界：跟随锚点骨骼在主场空间里演算，
    // 才能正确响应头部运动和重力。地面不需要，避免垂饰掉到 y=0。
    if (!accessory.rigid.empty() && physics_ptr) {
        auto phys = std::make_unique<pmx::Physics>();
        phys->gravity = physics_ptr->gravity;
        phys->iterations = physics_ptr->iterations;
        phys->warmup_steps = physics_ptr->warmup_steps;
        phys->ground = false;
        if (phys->build(accessory)) accessory_physics = std::move(phys);
    }
#endif
}

void Viewer::update_accessory_frame(float dt) {
    accessory_physics_active = false;
    if (!accessory_loaded || !pose || !accessory_pose || !accessory_skin) return;
    const std::size_t base = accessory_vertex_base;
    const std::size_t na = accessory.vertices.pos.size();
    if (base == 0 || na == 0 || render_model.vertices.size() != base + na) return;

    // 锚点骨骼相对静止的刚性增量：渲染模型把附件段绑在锚点骨骼上，
    // 写回顶点时要先除掉它，GPU/CPU 蒙皮会再乘回来。
    pmx::Mat4 delta = pmx::identity4();
    const std::vector<pmx::Mat4>& main_world = pose->world();
    if (accessory_bone >= 0 && accessory_bone < static_cast<int>(model.bones.size()) &&
        accessory_bone < static_cast<int>(main_world.size())) {
        const pmx::Vec3 rest = model.bones[static_cast<std::size_t>(accessory_bone)].pos;
        const pmx::Mat4 rest_m = pmx::from_quat_pos(pmx::identity_quat(), rest);
        delta = pmx::mul(main_world[static_cast<std::size_t>(accessory_bone)],
                         pmx::inverse_rigid(rest_m));
    }

    // 附件根骨骼落到 delta * 摆放矩阵上，其余骨骼保留自己的局部姿态（物理会写入）。
    const pmx::Mat4 attach = pmx::mul(delta, accessory_matrix(*this));
    for (int i = 0; i < accessory_pose->n; ++i) {
        const std::size_t si = static_cast<std::size_t>(i);
        if (accessory_pose->parent[si] >= 0) continue;
        const pmx::Vec3 rest = accessory.bones[si].pos;
        const pmx::Mat4 w = pmx::mul(attach, pmx::from_quat_pos(pmx::identity_quat(), rest));
        accessory_pose->base_rot[si] = pmx::mat3_to_quat(pmx::upper3(w));
        accessory_pose->base_trans[si] = pmx::Vec3{w.at(0, 3) - rest.x, w.at(1, 3) - rest.y,
                                                   w.at(2, 3) - rest.z};
    }
    // 上一帧物理写在 ov 里的覆盖不能留到本帧：关物理/复位后要回到动画姿态，
    // 物理演算自己会重新写入覆盖。
    accessory_pose->clear_overrides();
    accessory_pose->apply();
    if (preview_mode) accessory_pose->solve_ik();
    accessory_pose->world();

#ifdef PMX_ENABLE_PHYSICS
    const bool simulate = preview_mode && physics_enabled && accessory_physics != nullptr;
    if (simulate) {
        // 与主模型物理保持同一组参数（重力 / 迭代次数跟随面板）。
        if (physics_ptr)
            accessory_physics->set_params(physics_ptr->gravity, physics_ptr->iterations);
        accessory_physics->enabled = true;
        if (accessory_reset) {
            accessory_physics->reset(*accessory_pose, nullptr, true);
            accessory_reset = false;
        } else if (dt > 0.0f) {
            accessory_physics->advance(*accessory_pose, dt);
        }
        accessory_pose->world();
        accessory_physics_active = true;
    } else {
        accessory_reset = true;
    }
#endif

    // 附件自带骨骼蒙皮（世界坐标已含 delta 与摆放），再换算回锚点静止空间。
    accessory_skin->deform(accessory.vertices.pos, accessory.vertices.nrm,
                           accessory_pose->world(), accessory_skin_pos, accessory_skin_nrm);
    const pmx::Mat4 inv_delta = pmx::inverse_rigid(delta);
    const pmx::Mat3 inv_rot = pmx::upper3(inv_delta);
    pmx::VertexData& dst = render_model.vertices;
    for (std::size_t i = 0; i < na; ++i) {
        dst.pos[base + i] = pmx::transform_point(inv_delta, accessory_skin_pos[i]);
        const pmx::Vec3 n = pmx::mul(inv_rot, accessory_skin_nrm[i]);
        const float len = pmx::length(n);
        dst.nrm[base + i] = len > 1e-8f ? n * (1.0f / len) : n;
    }
    renderer.update_vertex_range(render_model, base);
}

bool Viewer::load_accessory(const std::string& path, std::string* err) {
    if (!pose || model.bones.empty()) {
        if (err) *err = "尚未加载模型或模型没有骨骼";
        return false;
    }
    pmx::Model acc;
    try {
        acc = pmx::load_pmx(path);
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
    if (acc.vertices.size() == 0 || acc.indices.empty()) {
        if (err) *err = "附件模型没有可渲染的几何";
        return false;
    }

    accessory_base = std::move(acc);
    accessory_path = path;
    accessory_loaded = true;
    accessory_bone = default_accessory_bone();
    accessory_rot[0] = accessory_rot[1] = accessory_rot[2] = 0.0f;
    accessory_scale = 1.0f;
    // 默认把附件原点放到锚点骨骼处，加载后就能在头部附近看到。
    if (accessory_bone >= 0) {
        const pmx::Vec3 p = model.bones[static_cast<std::size_t>(accessory_bone)].pos;
        accessory_pos[0] = p.x;
        accessory_pos[1] = p.y;
        accessory_pos[2] = p.z;
    } else {
        accessory_pos[0] = accessory_pos[1] = accessory_pos[2] = 0.0f;
    }

    rebuild_accessory_scaled();
    apply_accessory_structure();
    status = "已加载附件 " + path;
    return true;
}

void Viewer::clear_accessory() {
    const bool had = accessory_loaded;
    accessory = pmx::Model{};
    accessory_base = pmx::Model{};
    accessory_pose.reset();
    accessory_skin.reset();
#ifdef PMX_ENABLE_PHYSICS
    accessory_physics.reset();
#endif
    accessory_skin_pos.clear();
    accessory_skin_nrm.clear();
    accessory_reset = false;
    accessory_physics_active = false;
    render_model = pmx::Model{};
    accessory_path.clear();
    accessory_loaded = false;
    accessory_bone = -1;
    accessory_vertex_base = 0;

    if (!had || !pose) return;
    std::string err;
    if (!renderer.upload_model(model, &err)) {
        error = err;
        return;
    }
    apply_morphs();
    status = "已移除附件";
}

void Viewer::update_accessory_transform() {
    if (!accessory_loaded) return;
    // 结构过期（主模型刚改过几何）：走完整重建与上传。
    if (accessory_vertex_base == 0 ||
        render_model.vertices.size() !=
            model.vertices.size() + accessory.vertices.pos.size()) {
        apply_accessory_structure();
        return;
    }
    accessory_reset = true;
    write_accessory_vertices(*this);
    // 主模型的形态结果不变，只把附件段写回 VBO（贴图与缓冲都不重建）。
    renderer.update_vertex_range(active_model(), accessory_vertex_base);
}
