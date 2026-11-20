#include "core/flesh.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <thread>
#include <unordered_map>

#include "core/pmx.hpp"

namespace pmx {
namespace {

constexpr int kCellBias = 1 << 20;

// 空间哈希键：三个 21 位坐标拼成 64 位。
std::uint64_t cell_key(int x, int y, int z) {
    const std::uint64_t ux = static_cast<std::uint64_t>(x + kCellBias) & 0x1FFFFF;
    const std::uint64_t uy = static_cast<std::uint64_t>(y + kCellBias) & 0x1FFFFF;
    const std::uint64_t uz = static_cast<std::uint64_t>(z + kCellBias) & 0x1FFFFF;
    return (ux << 42) | (uy << 21) | uz;
}

std::uint64_t edge_key(int a, int b) {
    if (a > b) std::swap(a, b);
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(a)) << 32) |
           static_cast<std::uint32_t>(b);
}

int influence_count(std::uint8_t wtype) {
    switch (wtype) {
        case WT_BDEF1: return 1;
        case WT_BDEF2: return 2;
        case WT_BDEF4: return 4;
        case WT_SDEF: return 2;
        case WT_QDEF: return 4;
        default: return 1;
    }
}

bool finite(Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// 物理异常时坐标可能非常大；先夹住再转整数，避免浮点转 int 的未定义行为。
int cell_coord(float v) {
    return static_cast<int>(std::clamp(std::floor(v), -1000000.0f, 1000000.0f));
}

// 顶点数较少时多线程的创建/合并开销超过收益；上限取 8 避免在超多核机器上
// 把内存带宽吃满反而变慢。
int thread_count(int requested, int count) {
    if (requested > 0) return std::min(requested, 16);
    if (count < 2048) return 1;
    const unsigned hw = std::thread::hardware_concurrency();
    return std::clamp(static_cast<int>(hw == 0 ? 1 : hw), 1, 8);
}

template <typename Fn>
void parallel_ranges(int count, int threads, Fn&& fn) {
    if (threads <= 1 || count <= 0) {
        fn(0, std::max(0, count));
        return;
    }
    const int n = std::min(threads, std::max(1, count / 512 + 1));
    if (n <= 1) {
        fn(0, count);
        return;
    }
    const int chunk = (count + n - 1) / n;
    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(n - 1));
    for (int t = 1; t < n; ++t) {
        const int begin = t * chunk;
        const int end = std::min(count, begin + chunk);
        if (begin >= end) break;
        pool.emplace_back([&fn, begin, end] { fn(begin, end); });
    }
    fn(0, std::min(count, chunk));
    for (std::thread& th : pool) th.join();
}

// 点到线段 [a,b] 的最近点。
Vec3 closest_on_segment(Vec3 p, Vec3 a, Vec3 b) {
    const Vec3 ab = b - a;
    const float len2 = dot(ab, ab);
    if (len2 < 1e-12f) return a;
    const float t = clampf(dot(p - a, ab) / len2, 0.0f, 1.0f);
    return a + ab * t;
}

int weld_coord(float v) {
    return static_cast<int>(std::clamp(v, -100000.0f, 100000.0f) * 10000.0f);
}

// 点到三角形最近点，同时给出最近点的重心坐标（用于把凹陷分摊到三个顶点）。
void closest_on_triangle(Vec3 p, Vec3 a, Vec3 b, Vec3 c, Vec3& out, float bary[3]) {
    const Vec3 ab = b - a;
    const Vec3 ac = c - a;
    const Vec3 ap = p - a;
    const float d1 = dot(ab, ap);
    const float d2 = dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) {
        out = a;
    } else {
        const Vec3 bp = p - b;
        const float d3 = dot(ab, bp);
        const float d4 = dot(ac, bp);
        if (d3 >= 0.0f && d4 <= d3) {
            out = b;
        } else {
            const float vc = d1 * d4 - d3 * d2;
            if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
                out = a + ab * (d1 / (d1 - d3));
            } else {
                const Vec3 cp = p - c;
                const float d5 = dot(ab, cp);
                const float d6 = dot(ac, cp);
                if (d6 >= 0.0f && d5 <= d6) {
                    out = c;
                } else {
                    const float vb = d5 * d2 - d1 * d6;
                    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
                        out = a + ac * (d2 / (d2 - d6));
                    } else {
                        const float va = d3 * d6 - d5 * d4;
                        if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
                            out = b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
                        } else {
                            const float denom = 1.0f / (va + vb + vc);
                            const float v = vb * denom;
                            const float w = vc * denom;
                            out = a + ab * v + ac * w;
                        }
                    }
                }
            }
        }
    }
    const float wa = length(cross(c - b, out - b));
    const float wb = length(cross(a - c, out - c));
    const float wc = length(cross(b - a, out - a));
    const float sum = wa + wb + wc;
    if (sum > 1e-12f) {
        bary[0] = wa / sum;
        bary[1] = wb / sum;
        bary[2] = wc / sum;
    } else {
        bary[0] = 1.0f;
        bary[1] = 0.0f;
        bary[2] = 0.0f;
    }
}

}  // namespace

void FleshSolver::clear() {
    n_ = 0;
    cn_ = 0;
    height_ = 1.0f;
    canon_.clear();
    rep_.clear();
    counts_.clear();
    bones_.clear();
    weights_.clear();
    tris_.clear();
    edges_.clear();
}

void FleshSolver::reset(const Model& model) {
    clear();
    n_ = static_cast<int>(model.vertices.size());
    if (n_ <= 0) return;

    // 身高决定接触距离的默认尺度；模型没有竖直尺寸时退回包围盒对角线。
    Vec3 lo(1e30f, 1e30f, 1e30f);
    Vec3 hi(-1e30f, -1e30f, -1e30f);
    for (const Vec3& p : model.vertices.pos) {
        if (!finite(p)) continue;
        lo.x = std::min(lo.x, p.x);
        lo.y = std::min(lo.y, p.y);
        lo.z = std::min(lo.z, p.z);
        hi.x = std::max(hi.x, p.x);
        hi.y = std::max(hi.y, p.y);
        hi.z = std::max(hi.z, p.z);
    }
    height_ = std::max({hi.y - lo.y, 0.25f * length(hi - lo), 1e-3f});

    // 按静止位置焊接：UV 缝/材质边界上的重复顶点共享形变与法线。
    std::map<std::array<int, 3>, int> weld;
    canon_.assign(static_cast<std::size_t>(n_), 0);
    rep_.clear();
    rep_.reserve(static_cast<std::size_t>(n_));
    for (int i = 0; i < n_; ++i) {
        const Vec3 p = model.vertices.pos[static_cast<std::size_t>(i)];
        const std::array<int, 3> key{weld_coord(p.x), weld_coord(p.y), weld_coord(p.z)};
        auto [it, inserted] = weld.emplace(key, cn_);
        if (inserted) {
            rep_.push_back(i);
            ++cn_;
        }
        canon_[static_cast<std::size_t>(i)] = it->second;
    }

    counts_.assign(static_cast<std::size_t>(n_), 0);
    bones_.assign(static_cast<std::size_t>(n_), {});
    weights_.assign(static_cast<std::size_t>(n_), {});
    for (int i = 0; i < n_; ++i) {
        const std::size_t si = static_cast<std::size_t>(i);
        const int count = std::clamp(influence_count(model.vertices.wtype[si]), 1, 4);
        counts_[si] = static_cast<std::uint8_t>(count);
        bones_[si] = model.vertices.bones[si];
        weights_[si] = model.vertices.weights[si];
    }

    // 焊接后的三角形与边表。相邻边上的顶点靠近是正常的折痕，不是自接触。
    tris_.clear();
    tris_.reserve(model.indices.size() / 3);
    for (std::size_t t = 0; t + 2 < model.indices.size(); t += 3) {
        const int a = canon_[static_cast<std::size_t>(model.indices[t])];
        const int b = canon_[static_cast<std::size_t>(model.indices[t + 1])];
        const int c = canon_[static_cast<std::size_t>(model.indices[t + 2])];
        if (a == b || b == c || a == c) continue;
        tris_.push_back({a, b, c});
    }
    edges_.clear();
    edges_.reserve(tris_.size() * 3);
    for (const auto& tri : tris_) {
        edges_.insert(edge_key(tri[0], tri[1]));
        edges_.insert(edge_key(tri[1], tri[2]));
        edges_.insert(edge_key(tri[2], tri[0]));
    }
}

void FleshSolver::apply(std::vector<Vec3>& pos, std::vector<Vec3>& nrm,
                        const FleshParams& params,
                        const std::vector<FleshColliderShape>& colliders) const {
    if (!valid() || pos.size() < static_cast<std::size_t>(n_) ||
        nrm.size() < static_cast<std::size_t>(n_))
        return;

    const float range =
        std::max(1e-5f, height_ * clampf(params.range, 0.001f, 0.08f));
    const float width = clampf(params.width, 0.5f, 12.0f);
    const float bulge_radius = range * width;
    const int threads = thread_count(params.threads, cn_);

    // 焊接顶点：重复顶点取平均位置，法线归一化后用于接触方向判断。
    std::vector<Vec3> wp(static_cast<std::size_t>(cn_), {0.0f, 0.0f, 0.0f});
    std::vector<Vec3> wn(static_cast<std::size_t>(cn_), {0.0f, 0.0f, 0.0f});
    std::vector<int> wc(static_cast<std::size_t>(cn_), 0);
    for (int i = 0; i < n_; ++i) {
        const int c = canon_[static_cast<std::size_t>(i)];
        const Vec3 p = pos[static_cast<std::size_t>(i)];
        if (!finite(p)) continue;
        wp[static_cast<std::size_t>(c)] += p;
        const Vec3 n = nrm[static_cast<std::size_t>(i)];
        if (finite(n)) wn[static_cast<std::size_t>(c)] += n;
        ++wc[static_cast<std::size_t>(c)];
    }
    for (int c = 0; c < cn_; ++c) {
        const std::size_t sc = static_cast<std::size_t>(c);
        if (wc[sc] > 1) wp[sc] = wp[sc] / static_cast<float>(wc[sc]);
        if (length(wn[sc]) > 1e-8f) wn[sc] = normalize(wn[sc]);
    }

    // 每个焊接点的硬度：0 最软，1 完全不变形。
    std::vector<float> firm(static_cast<std::size_t>(cn_), 0.0f);
    for (int c = 0; c < cn_; ++c) {
        float region = 0.0f;
        if (params.region_bone >= 0) {
            const int i = rep_[static_cast<std::size_t>(c)];
            const int count = counts_[static_cast<std::size_t>(i)];
            for (int k = 0; k < count; ++k) {
                if (bones_[static_cast<std::size_t>(i)][static_cast<std::size_t>(k)] ==
                    params.region_bone)
                    region += weights_[static_cast<std::size_t>(i)][static_cast<std::size_t>(k)];
            }
        }
        firm[static_cast<std::size_t>(c)] =
            clampf(params.firmness + params.region_firmness * clampf(region, 0.0f, 1.0f), 0.0f,
                   1.0f);
    }

    // 三角形空间哈希：顶点与对方三角形求最近点，能处理稀疏网格的大面接触
    // （只看顶点-顶点会漏掉“面压面”的关节）。
    const float inv_cell = 1.0f / range;
    std::vector<Vec3> tri_normal(tris_.size());
    std::unordered_map<std::uint64_t, std::vector<int>> tri_grid;
    tri_grid.reserve(tris_.size() * 2);
    for (std::size_t t = 0; t < tris_.size(); ++t) {
        const auto& tri = tris_[t];
        const Vec3 a = wp[static_cast<std::size_t>(tri[0])];
        const Vec3 b = wp[static_cast<std::size_t>(tri[1])];
        const Vec3 c = wp[static_cast<std::size_t>(tri[2])];
        if (!finite(a) || !finite(b) || !finite(c)) continue;
        const Vec3 n = cross(b - a, c - a);
        const float n2 = dot(n, n);
        if (n2 < 1e-16f) continue;
        tri_normal[t] = n * (1.0f / std::sqrt(n2));
        const int x0 = cell_coord(std::min({a.x, b.x, c.x}) * inv_cell);
        const int x1 = cell_coord(std::max({a.x, b.x, c.x}) * inv_cell);
        const int y0 = cell_coord(std::min({a.y, b.y, c.y}) * inv_cell);
        const int y1 = cell_coord(std::max({a.y, b.y, c.y}) * inv_cell);
        const int z0 = cell_coord(std::min({a.z, b.z, c.z}) * inv_cell);
        const int z1 = cell_coord(std::max({a.z, b.z, c.z}) * inv_cell);
        // 病态的超大三角形不铺格子，避免一帧插入几十万个单元。
        if (static_cast<long long>(x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) > 200000)
            continue;
        for (int x = x0; x <= x1; ++x)
            for (int y = y0; y <= y1; ++y)
                for (int z = z0; z <= z1; ++z)
                    tri_grid[cell_key(x, y, z)].push_back(static_cast<int>(t));
    }

    std::vector<float> contact(static_cast<std::size_t>(cn_), 0.0f);
    std::vector<float> dent(static_cast<std::size_t>(cn_), 0.0f);
    // 每个顶点独立扫描邻近三角形，只写自己的分量；多线程时用线程局部数组
    // 收集后按 max 合并（max 可交换/结合，结果与单线程完全一致）。
    auto scan_contacts = [&](int begin, int end, float* out_contact, float* out_dent) {
        for (int v = begin; v < end; ++v) {
            const std::size_t sv = static_cast<std::size_t>(v);
            const Vec3 p = wp[sv];
            if (!finite(p) || length(wn[sv]) < 1e-6f) continue;
            const int cx = cell_coord(p.x * inv_cell);
            const int cy = cell_coord(p.y * inv_cell);
            const int cz = cell_coord(p.z * inv_cell);
            for (int dx = -1; dx <= 1; ++dx) {
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dz = -1; dz <= 1; ++dz) {
                        const auto it = tri_grid.find(cell_key(cx + dx, cy + dy, cz + dz));
                        if (it == tri_grid.end()) continue;
                        for (const int ti : it->second) {
                            const auto& tri = tris_[static_cast<std::size_t>(ti)];
                            if (tri[0] == v || tri[1] == v || tri[2] == v) continue;
                            // 共用边的相邻面是正常折痕，不当作自接触。
                            if (edges_.find(edge_key(v, tri[0])) != edges_.end() ||
                                edges_.find(edge_key(v, tri[1])) != edges_.end() ||
                                edges_.find(edge_key(v, tri[2])) != edges_.end())
                                continue;
                            const Vec3 tn = tri_normal[static_cast<std::size_t>(ti)];
                            if (length(tn) < 0.5f) continue;
                            // 只认面对面挤压：顶点法线与三角形法线需大致相反。
                            if (dot(wn[sv], tn) > -0.2f) continue;
                            const Vec3 a = wp[static_cast<std::size_t>(tri[0])];
                            const Vec3 b = wp[static_cast<std::size_t>(tri[1])];
                            const Vec3 c = wp[static_cast<std::size_t>(tri[2])];
                            Vec3 closest;
                            float bary[3];
                            closest_on_triangle(p, a, b, c, closest, bary);
                            const Vec3 d = p - closest;
                            const float d2 = dot(d, d);
                            if (d2 >= range * range) continue;
                            const float strength = 1.0f - std::sqrt(d2) / range;
                            if (strength <= 0.0f) continue;
                            out_contact[sv] = std::max(out_contact[sv], strength);
                            const float depth =
                                strength * range * 0.5f * clampf(params.dent, 0.0f, 1.0f);
                            out_dent[sv] = std::max(out_dent[sv], depth * (1.0f - firm[sv]));
                            for (int k = 0; k < 3; ++k) {
                                const std::size_t sk = static_cast<std::size_t>(tri[k]);
                                out_contact[sk] = std::max(out_contact[sk], strength * bary[k]);
                                out_dent[sk] = std::max(
                                    out_dent[sk], depth * (1.0f - firm[sk]) * bary[k]);
                            }
                        }
                    }
                }
            }
        }
    };
    if (threads <= 1) {
        scan_contacts(0, cn_, contact.data(), dent.data());
    } else {
        const int chunk = (cn_ + threads - 1) / threads;
        std::vector<std::vector<float>> local_contact(static_cast<std::size_t>(threads));
        std::vector<std::vector<float>> local_dent(static_cast<std::size_t>(threads));
        for (int t = 0; t < threads; ++t) {
            local_contact[static_cast<std::size_t>(t)].assign(static_cast<std::size_t>(cn_), 0.0f);
            local_dent[static_cast<std::size_t>(t)].assign(static_cast<std::size_t>(cn_), 0.0f);
        }
        std::vector<std::thread> pool;
        pool.reserve(static_cast<std::size_t>(threads - 1));
        for (int t = 1; t < threads; ++t) {
            const int begin = t * chunk;
            const int end = std::min(cn_, begin + chunk);
            if (begin >= end) break;
            pool.emplace_back([&, t, begin, end] {
                scan_contacts(begin, end, local_contact[static_cast<std::size_t>(t)].data(),
                              local_dent[static_cast<std::size_t>(t)].data());
            });
        }
        scan_contacts(0, std::min(cn_, chunk), local_contact[0].data(), local_dent[0].data());
        for (std::thread& th : pool) th.join();
        for (int t = 0; t < threads; ++t) {
            const float* lc = local_contact[static_cast<std::size_t>(t)].data();
            const float* ld = local_dent[static_cast<std::size_t>(t)].data();
            for (int c = 0; c < cn_; ++c) {
                contact[static_cast<std::size_t>(c)] =
                    std::max(contact[static_cast<std::size_t>(c)], lc[c]);
                dent[static_cast<std::size_t>(c)] =
                    std::max(dent[static_cast<std::size_t>(c)], ld[c]);
            }
        }
    }

    // 接触点空间哈希：隆起、Corner Taper 都从这份邻域取数据。
    std::unordered_map<std::uint64_t, std::vector<int>> contact_grid;
    const float inv_bulge = 1.0f / std::max(bulge_radius, 1e-6f);
    for (int c = 0; c < cn_; ++c) {
        const std::size_t sc = static_cast<std::size_t>(c);
        if (contact[sc] <= 0.02f) continue;
        const Vec3 p = wp[sc];
        contact_grid[cell_key(cell_coord(p.x * inv_bulge), cell_coord(p.y * inv_bulge),
                              cell_coord(p.z * inv_bulge))]
            .push_back(c);
    }

    // Corner Taper：折痕中段两侧的接触方向互相抵消，闭合端只朝一侧，
    // 据此回收端部的隆起，避免折痕尽头鼓出一块。
    std::vector<float> taper(static_cast<std::size_t>(cn_), 0.0f);
    if (params.taper > 0.0f) {
        const float taper_radius =
            std::max(bulge_radius * clampf(params.taper_reach, 0.05f, 3.0f), 1e-5f);
        const float inv_taper = 1.0f / taper_radius;
        const float amount = clampf(params.taper, 0.0f, 1.0f);
        const float falloff = clampf(params.taper_falloff, 0.1f, 4.0f);
        parallel_ranges(cn_, threads, [&](int begin, int end) {
            for (int v = begin; v < end; ++v) {
                const std::size_t sv = static_cast<std::size_t>(v);
                if (contact[sv] <= 0.02f) continue;
                const Vec3 p = wp[sv];
                const int cx = cell_coord(p.x * inv_bulge);
                const int cy = cell_coord(p.y * inv_bulge);
                const int cz = cell_coord(p.z * inv_bulge);
                Vec3 dir(0.0f, 0.0f, 0.0f);
                float total = 0.0f;
                for (int dx = -1; dx <= 1; ++dx) {
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dz = -1; dz <= 1; ++dz) {
                            const auto it =
                                contact_grid.find(cell_key(cx + dx, cy + dy, cz + dz));
                            if (it == contact_grid.end()) continue;
                            for (const int c : it->second) {
                                if (c == v) continue;
                                const Vec3 d = wp[static_cast<std::size_t>(c)] - p;
                                const float dist = length(d);
                                if (dist < 1e-6f || dist >= taper_radius) continue;
                                const float w =
                                    contact[static_cast<std::size_t>(c)] * (1.0f - dist * inv_taper);
                                if (w <= 0.0f) continue;
                                dir += d * (w / dist);
                                total += w;
                            }
                        }
                    }
                }
                // 没有任何接触邻居 = 孤立的折痕端，按最闭合处理。
                const float aniso = total > 1e-6f ? length(dir) / total : 1.0f;
                // 平坦压合区的边缘方向分散（aniso≈0.6），只有明显单侧的
                // 折痕端部才越过 0.55 阈值。
                const float t = clampf((aniso - 0.55f) / 0.45f, 0.0f, 1.0f);
                taper[sv] = amount * std::pow(t, falloff);
            }
        });
    }

    // 隆起：接触点把周围表面向外推，形成 Fleshy 的“挤压外翻”。
    std::vector<float> bulge(static_cast<std::size_t>(cn_), 0.0f);
    const float amplitude = range * clampf(params.bulge, 0.0f, 2.0f) * 0.75f;
    parallel_ranges(cn_, threads, [&](int begin, int end) {
        for (int v = begin; v < end; ++v) {
            const std::size_t sv = static_cast<std::size_t>(v);
            if (amplitude <= 0.0f) continue;
            const float soft = 1.0f - firm[sv];
            const float rim = 1.0f - clampf(contact[sv], 0.0f, 1.0f);
            if (soft <= 0.01f || rim <= 0.0f || !finite(wp[sv])) continue;
            const Vec3 p = wp[sv];
            const int cx = cell_coord(p.x * inv_bulge);
            const int cy = cell_coord(p.y * inv_bulge);
            const int cz = cell_coord(p.z * inv_bulge);
            float best = 0.0f;
            for (int dx = -1; dx <= 1; ++dx) {
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dz = -1; dz <= 1; ++dz) {
                        const auto it = contact_grid.find(cell_key(cx + dx, cy + dy, cz + dz));
                        if (it == contact_grid.end()) continue;
                        for (const int c : it->second) {
                            const float dist = length(wp[static_cast<std::size_t>(c)] - p);
                            const float t = 1.0f - dist / bulge_radius;
                            if (t <= 0.0f) continue;
                            const float src =
                                contact[static_cast<std::size_t>(c)] *
                                (1.0f - taper[static_cast<std::size_t>(c)]);
                            best = std::max(best, src * t * t);
                        }
                    }
                }
            }
            bulge[sv] = amplitude * best * rim * soft;
        }
    });

    // 外部碰撞体：把陷进碰撞体内部的顶点推到表面，碰撞体周围一圈轻微外翻。
    std::vector<Vec3> collider_delta(static_cast<std::size_t>(cn_), {0.0f, 0.0f, 0.0f});
    if (!colliders.empty()) {
        parallel_ranges(cn_, threads, [&](int begin, int end) {
            for (int v = begin; v < end; ++v) {
                const std::size_t sv = static_cast<std::size_t>(v);
                if (!finite(wp[sv])) continue;
                const Vec3 p = wp[sv];
                Vec3 move(0.0f, 0.0f, 0.0f);
                for (const FleshColliderShape& col : colliders) {
                    const float radius = std::max(col.radius, 1e-5f);
                    const Vec3 closest =
                        col.capsule ? closest_on_segment(p, col.a, col.b) : col.a;
                    const Vec3 d = p - closest;
                    const float dist = length(d);
                    if (dist < 1e-6f || dist > radius + bulge_radius) continue;
                    const float soft = 1.0f - clampf(col.firmness, 0.0f, 1.0f);
                    if (soft <= 0.0f) continue;
                    const Vec3 dir = d * (1.0f / dist);
                    if (dist < radius) {
                        // 穿透：推到碰撞体表面，留下贴合的形状。
                        move += dir * ((radius - dist) * soft);
                    } else {
                        // 接触环：越靠近表面越强，抑制硬边。
                        const float t = 1.0f - (dist - radius) / bulge_radius;
                        move += dir * (range * 0.35f * t * t * soft);
                    }
                }
                collider_delta[sv] = move;
            }
        });
    }

    // 应用位移：凹陷沿法线向内，隆起向外，碰撞体按径向推出。
    std::vector<Vec3> delta(static_cast<std::size_t>(cn_), {0.0f, 0.0f, 0.0f});
    for (int c = 0; c < cn_; ++c) {
        const std::size_t sc = static_cast<std::size_t>(c);
        const float move = bulge[sc] - dent[sc];
        Vec3 d = wn[sc] * move + collider_delta[sc];
        if (d.x == 0.0f && d.y == 0.0f && d.z == 0.0f) continue;
        delta[sc] = d;
        wp[sc] += d;
    }

    // 形变区域重算法线；未移动的顶点保留原法线，避免把硬边磨平。
    std::vector<Vec3> deformed(static_cast<std::size_t>(cn_), {0.0f, 0.0f, 0.0f});
    for (const auto& tri : tris_) {
        const std::size_t a = static_cast<std::size_t>(tri[0]);
        const std::size_t b = static_cast<std::size_t>(tri[1]);
        const std::size_t c = static_cast<std::size_t>(tri[2]);
        const Vec3 fn = cross(wp[b] - wp[a], wp[c] - wp[a]);
        if (!finite(fn) || dot(fn, fn) < 1e-16f) continue;
        deformed[a] += fn;
        deformed[b] += fn;
        deformed[c] += fn;
    }
    const float moved_eps = range * 1e-4f;
    for (int c = 0; c < cn_; ++c) {
        const std::size_t sc = static_cast<std::size_t>(c);
        if (length(deformed[sc]) > 1e-8f) {
            wn[sc] = normalize(deformed[sc]);
        }
        if (length(delta[sc]) < moved_eps) delta[sc] = Vec3(0.0f, 0.0f, 0.0f);
    }

    for (int i = 0; i < n_; ++i) {
        const int c = canon_[static_cast<std::size_t>(i)];
        const std::size_t sc = static_cast<std::size_t>(c);
        if (delta[sc].x == 0.0f && delta[sc].y == 0.0f && delta[sc].z == 0.0f) continue;
        pos[static_cast<std::size_t>(i)] += delta[sc];
        nrm[static_cast<std::size_t>(i)] = wn[sc];
    }
}

void smooth_flesh_pops(const std::vector<Vec3>& skinned, std::vector<Vec3>& pos,
                       std::vector<Vec3>& prev, float blend) {
    if (pos.size() != skinned.size()) {
        prev.clear();
        return;
    }
    const float keep = clampf(blend, 0.0f, 0.9f);
    if (prev.size() != pos.size()) {
        prev.resize(pos.size());
        for (std::size_t i = 0; i < pos.size(); ++i) prev[i] = pos[i] - skinned[i];
        return;
    }
    if (keep <= 0.0f) return;
    const float fresh = 1.0f - keep;
    for (std::size_t i = 0; i < pos.size(); ++i) {
        const Vec3 current = pos[i] - skinned[i];
        prev[i] = prev[i] * keep + current * fresh;
        pos[i] = skinned[i] + prev[i];
    }
}

}  // namespace pmx
