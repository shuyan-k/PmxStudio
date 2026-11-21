#include "core/model_transform.hpp"

#include <algorithm>
#include <cmath>

namespace pmx {

void transform_model(Model& model, Vec3 translation, float yaw_degrees, float scale) {
    if (!(scale > 1e-6f)) scale = 1.0f;
    const Quat yaw = axis_angle_quat({0.0f, 1.0f, 0.0f}, yaw_degrees * kPi / 180.0f);
    const Mat3 rot = quat_to_mat3(yaw);

    auto point = [&](Vec3 p) { return mul(rot, p * scale) + translation; };
    auto direction = [&](Vec3 p) { return mul(rot, p); };

    VertexData& v = model.vertices;
    for (std::size_t i = 0; i < v.pos.size(); ++i) {
        v.pos[i] = point(v.pos[i]);
        v.nrm[i] = normalize(direction(v.nrm[i]));
        // SDEF：C / R0 / R1 都是模型空间里的点，跟随整体变换。
        std::array<float, 9>& sd = v.sdef[i];
        for (int k = 0; k < 3; ++k) {
            const Vec3 p = point({sd[k * 3 + 0], sd[k * 3 + 1], sd[k * 3 + 2]});
            sd[k * 3 + 0] = p.x;
            sd[k * 3 + 1] = p.y;
            sd[k * 3 + 2] = p.z;
        }
    }

    for (Bone& bone : model.bones) {
        bone.pos = point(bone.pos);
        bone.tail_offset = direction(bone.tail_offset) * scale;
        bone.axis = direction(bone.axis);
        bone.local_x = direction(bone.local_x);
        bone.local_z = direction(bone.local_z);
    }

    for (RigidBody& body : model.rigid) {
        body.pos = point(body.pos);
        body.rot.y += yaw_degrees;
        body.size = body.size * scale;
    }

    for (Joint& joint : model.joints) {
        joint.pos = point(joint.pos);
        joint.rot.y += yaw_degrees;
        joint.min_loc = joint.min_loc * scale;
        joint.max_loc = joint.max_loc * scale;
        joint.spring_loc = joint.spring_loc * scale;
    }

    for (SoftBody& soft : model.soft_bodies) {
        soft.collision_margin *= scale;
        soft.b_link_dist = static_cast<int>(std::lround(soft.b_link_dist * scale));
    }

    for (Morph& morph : model.morphs) {
        // 1 顶点 / 2 骨骼 / 9 冲量偏移带方向或位置，其余与空间无关。
        if (morph.type == 1) {
            // 形态里存的是顶点位移（不是位置），只缩放 + 旋转，不平移。
            for (MorphOffset& offset : morph.offsets) {
                const Vec3 p = direction({offset.offset.x, offset.offset.y, offset.offset.z}) *
                               scale;
                offset.offset.x = p.x;
                offset.offset.y = p.y;
                offset.offset.z = p.z;
            }
        } else if (morph.type == 2) {
            // 骨骼局部旋转增量随模型一起旋转：q' = R * q * R^-1。
            const Quat inv_yaw{-yaw.x, -yaw.y, -yaw.z, yaw.w};
            for (MorphOffset& offset : morph.offsets) {
                offset.move = direction(offset.move) * scale;
                const Quat local{offset.rot.x, offset.rot.y, offset.rot.z, offset.rot.w};
                const Quat rotated = quat_mul(yaw, quat_mul(local, inv_yaw));
                offset.rot = {rotated.x, rotated.y, rotated.z, rotated.w};
            }
        } else if (morph.type == 9) {
            for (MorphOffset& offset : morph.offsets) {
                offset.velocity = direction(offset.velocity);
                offset.torque = direction(offset.torque);
            }
        }
    }
}

}  // namespace pmx
