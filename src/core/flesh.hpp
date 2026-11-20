// 接触式肉体变形：自接触检测 + 凹陷/隆起 + 区域软硬 + 折痕端回收（Corner Taper）
// + 外部碰撞体。逐帧、无状态：只依赖当前蒙皮后的顶点位置，不做模拟，因此拖动时间轴、
// 跳帧渲染都得到一致结果（Smooth Pops 的时间平滑放在渲染器/烘焙侧）。
#pragma once

#include <array>
#include <cstdint>
#include <unordered_set>
#include <vector>

#include "core/math.hpp"

namespace pmx {

struct Model;

// 由骨骼姿态换算到世界空间的碰撞体：球（a 为球心）或胶囊（a→b）。
struct FleshColliderShape {
    Vec3 a{0.0f, 0.0f, 0.0f};
    Vec3 b{0.0f, 0.0f, 0.0f};
    float radius = 0.1f;
    // 0 最软，1 完全不让肉体形变。
    float firmness = 0.0f;
    bool capsule = false;
};

struct FleshParams {
    // 自接触检测距离，占模型身高的比例。
    float range = 0.012f;
    // 凹陷深度：接触余量的比例，1 表示两侧各压入一半、刚好合拢。
    float dent = 0.6f;
    // 隆起强度；宽度是接触距离的倍数（隆起影响半径）。
    float bulge = 1.0f;
    float width = 3.0f;
    // 整体硬度：1 时完全不形变。
    float firmness = 0.0f;
    // 区域硬度：该骨骼影响的顶点额外变硬（-1 表示关闭）。
    int region_bone = -1;
    float region_firmness = 0.7f;
    // Corner Taper：折痕闭合端回收隆起。reach 是隆起半径的倍数，
    // falloff 控制从端部向折痕中段过渡的曲线（越大越集中在端点）。
    float taper = 0.55f;
    float taper_reach = 0.70f;
    float taper_falloff = 1.0f;
    // 0 = 自动（按顶点数启用多线程），1 = 强制单线程（调试/测试）。
    int threads = 0;
};

// Smooth Pops：把当前肉体位移与上一帧做指数混合，抑制接触状态翻转造成的单帧跳变。
// skinned 为未形变坐标，pos 为形变结果（原地修改），prev 保存上一帧位移。
void smooth_flesh_pops(const std::vector<Vec3>& skinned, std::vector<Vec3>& pos,
                       std::vector<Vec3>& prev, float blend);

class FleshSolver {
public:
    // 结构变化（加载模型 / 编辑顶点）后重建焊接拓扑。
    void reset(const Model& model);
    void clear();
    bool valid() const { return cn_ > 0; }
    float height() const { return height_; }

    // pos/nrm 为蒙皮后的顶点；原地写入形变结果，并在形变区域重算法线。
    void apply(std::vector<Vec3>& pos, std::vector<Vec3>& nrm, const FleshParams& params,
               const std::vector<FleshColliderShape>& colliders = {}) const;

private:
    int n_ = 0;
    int cn_ = 0;
    float height_ = 1.0f;
    // 原始顶点 -> 焊接后的顶点；重复顶点（UV 缝/材质边界）共享同一个焊接点。
    std::vector<int> canon_;
    std::vector<int> rep_;  // 焊接点对应的第一个原始顶点，用于读取骨骼权重
    std::vector<std::uint8_t> counts_;
    std::vector<std::array<int, 4>> bones_;
    std::vector<std::array<float, 4>> weights_;
    std::vector<std::array<int, 3>> tris_;
    // 共享边的焊接顶点对：相邻面在折痕处会靠得很近，不能当作自接触。
    std::unordered_set<std::uint64_t> edges_;
};

}  // namespace pmx
