// 多模型场景的摆放：把一个 PMX 模型整体缩放 / 旋转 / 平移到场景里。
#pragma once

#include "core/pmx.hpp"

namespace pmx {

// 原地变换模型：先按 scale 缩放，再绕 Y 轴旋转 yaw_degrees（角度制），最后平移。
// 网格顶点、SDEF 控制点、骨骼、刚体 / 关节、软体尺度与形态偏移都会同步变换，
// 保证蒙皮与物理在变换后仍然一致。只改内存副本，不写回文件。
void transform_model(Model& model, Vec3 translation, float yaw_degrees, float scale);

}  // namespace pmx
