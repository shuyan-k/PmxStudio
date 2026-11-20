#include "gpu/flesh_gpu.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_set>

#include "core/pmx.hpp"

namespace pmx {
namespace {

// 与蒙皮/网格着色器共用 binding 0..5，避免超出 GL 4.6 的最低 8 个 SSBO 绑定点。
constexpr GLuint kBindStatic = 0;
constexpr GLuint kBindDynamic = 1;
constexpr GLuint kBindGrid = 2;
constexpr GLuint kBindPos = 3;
constexpr GLuint kBindNrm = 4;
constexpr GLuint kBindCollider = 5;

// 碰撞体数量上限：UI 不会用到这么多，固定容量让缓冲只分配一次。
constexpr int kMaxColliders = 256;

int weld_coord(float v) {
    return static_cast<int>(std::clamp(v, -100000.0f, 100000.0f) * 10000.0f);
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

std::uint32_t next_pow2(std::uint32_t v) {
    std::uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

// ---------------------------------------------------------------- GLSL 公共前缀
// 所有偏移/容量都以 uniform 传入：静态与动态缓冲的布局在 CPU 侧一份，
// 着色器只按名字取，避免两边手写索引错位。
const char* const kFleshCommon = R"GLSL(#version 460 core
layout(local_size_x = 64) in;

layout(std430, binding = 0) readonly buffer TopoBlock { uint topo[]; };
layout(std430, binding = 1) buffer DynBlock { uint dyn[]; };
layout(std430, binding = 2) buffer GridBlock { uint grid[]; };
layout(std430, binding = 3) buffer PosBlock { vec4 gpos[]; };
layout(std430, binding = 4) buffer NrmBlock { vec4 gnrm[]; };
layout(std430, binding = 5) readonly buffer ColBlock { vec4 cols[]; };

uniform int uN;
uniform int uCn;
uniform int uT;

uniform int uCanon;
uniform int uDupOff;
uniform int uDupVert;
uniform int uRepBone;
uniform int uRepWeight;
uniform int uRepCount;
uniform int uNbOff;
uniform int uNbVert;
uniform int uVtriOff;
uniform int uVtriId;
uniform int uTri;

uniform int uWeldPos;
uniform int uWeldNrm;
uniform int uTriNrm;
uniform int uFirm;
uniform int uTaper;
uniform int uBulge;
uniform int uDelta;
uniform int uContact;
uniform int uDent;
uniform int uPrev;
uniform int uSkinSnap;

uniform int uTriHead;
uniform int uTriHeadCount;
uniform int uContactHead;
uniform int uContactHeadCount;
uniform int uCounter;
uniform int uTriPool;
uniform int uTriCap;
uniform int uContactPool;
uniform int uContactCap;

bool finite3(vec3 v) { return !any(isnan(v)) && !any(isinf(v)); }

vec3 load3(int off, uint i) {
    int b = off + int(i) * 4;
    return vec3(uintBitsToFloat(dyn[b]), uintBitsToFloat(dyn[b + 1]), uintBitsToFloat(dyn[b + 2]));
}

void store3(int off, uint i, vec3 v) {
    int b = off + int(i) * 4;
    dyn[b] = floatBitsToUint(v.x);
    dyn[b + 1] = floatBitsToUint(v.y);
    dyn[b + 2] = floatBitsToUint(v.z);
}

float load1(int off, uint i) { return uintBitsToFloat(dyn[off + int(i)]); }
void store1(int off, uint i, float v) { dyn[off + int(i)] = floatBitsToUint(v); }

int cell_coord(float v) { return int(clamp(floor(v), -1000000.0, 1000000.0)); }

// 与 CPU FleshSolver 完全一致的空间哈希键：三个 21 位坐标拼成 63 位，
// 这里拆成两个 uint 存放；桶号只做散列，命中仍按 lo/hi 校验。
void pack_cell(int x, int y, int z, out uint lo, out uint hi) {
    uint ux = uint(x + (1 << 20)) & 0x1FFFFFu;
    uint uy = uint(y + (1 << 20)) & 0x1FFFFFu;
    uint uz = uint(z + (1 << 20)) & 0x1FFFFFu;
    lo = uz | ((uy & 0x7FFu) << 21);
    hi = (uy >> 11) | (ux << 10);
}

uint cell_hash(uint lo, uint hi, int mask) {
    uint h = lo * 0x9E3779B1u ^ (hi * 0x85EBCA77u + 0x165667B1u);
    h ^= h >> 15;
    h *= 0x2545F491u;
    h ^= h >> 13;
    return h & uint(mask);
}

// 无锁 push-front：入口内容先写完整，再用 CAS 发布桶头。
// 读到旧桶头说明发布失败，按最新值重试；入口槽位由原子计数器唯一分配。
void push_entry(int head_off, int pool, int cap, int counter_index, int bucket, uint id,
                uint lo, uint hi) {
    uint slot = atomicAdd(grid[counter_index], 1u);
    if (slot >= uint(cap)) return;
    int base = pool + int(slot) * 4;
    uint expected = grid[head_off + bucket];
    uint prev;
    do {
        prev = expected;
        grid[base] = id;
        grid[base + 1] = prev;
        grid[base + 2] = lo;
        grid[base + 3] = hi;
        memoryBarrierBuffer();
        expected = atomicCompSwap(grid[head_off + bucket], prev, slot + 1u);
    } while (expected != prev);
}
)GLSL";

// ---------------------------------------------------------------- 各阶段

// 蒙皮结果快照：Smooth Pops 需要形变前的坐标做基准（只在开启平滑时执行）。
const char* const kSkinSnapshot = R"GLSL(
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= uint(uN)) return;
    store3(uSkinSnap, i, gpos[i].xyz);
}
)GLSL";

// 焊接顶点准备：重复顶点取平均、法线归一化、区域硬度，并清空逐帧中间量。
const char* const kWeldPrepare = R"GLSL(
uniform float uFirmness;
uniform int uRegionBone;
uniform float uRegionFirmness;

void main() {
    uint c = gl_GlobalInvocationID.x;
    if (c >= uint(uCn)) return;
    uint begin = topo[uDupOff + int(c)];
    uint end = topo[uDupOff + int(c) + 1];

    vec3 p = vec3(0.0);
    vec3 n = vec3(0.0);
    uint count = 0u;
    for (uint k = begin; k < end; ++k) {
        uint vi = topo[uDupVert + int(k)];
        vec3 pp = gpos[vi].xyz;
        if (finite3(pp)) {
            p += pp;
            ++count;
        }
        vec3 nn = gnrm[vi].xyz;
        if (finite3(nn)) n += nn;
    }
    if (count > 0u) p /= float(count);
    float nl = length(n);
    if (nl > 1e-8) n /= nl;
    store3(uWeldPos, c, p);
    store3(uWeldNrm, c, n);

    float region = 0.0;
    if (uRegionBone >= 0) {
        int cnt = int(topo[uRepCount + int(c)]);
        for (int k = 0; k < cnt; ++k) {
            int b = int(topo[uRepBone + int(c) * 4 + k]);
            if (b == uRegionBone) region += uintBitsToFloat(topo[uRepWeight + int(c) * 4 + k]);
        }
    }
    store1(uFirm, c, clamp(uFirmness + uRegionFirmness * clamp(region, 0.0, 1.0), 0.0, 1.0));

    dyn[uContact + int(c)] = 0u;
    dyn[uDent + int(c)] = 0u;
    store1(uTaper, c, 0.0);
    store1(uBulge, c, 0.0);
    store3(uDelta, c, vec3(0.0));
}
)GLSL";

// 三角形入桶：同时算三角形法线。退化三角形写法线 0 且不铺格子。
const char* const kTriGrid = R"GLSL(
uniform float uRange;
uniform int uTriMask;

void main() {
    uint t = gl_GlobalInvocationID.x;
    if (t >= uint(uT)) return;
    uint ta = topo[uTri + int(t) * 3];
    uint tb = topo[uTri + int(t) * 3 + 1];
    uint tc = topo[uTri + int(t) * 3 + 2];
    vec3 a = load3(uWeldPos, ta);
    vec3 b = load3(uWeldPos, tb);
    vec3 c = load3(uWeldPos, tc);
    vec3 n = vec3(0.0);
    if (finite3(a) && finite3(b) && finite3(c)) {
        vec3 cr = cross(b - a, c - a);
        float n2 = dot(cr, cr);
        if (n2 >= 1e-16) n = cr * inversesqrt(n2);
    }
    store3(uTriNrm, t, n);
    if (dot(n, n) < 0.25) return;

    float inv = 1.0 / uRange;
    int x0 = cell_coord(min(a.x, min(b.x, c.x)) * inv);
    int x1 = cell_coord(max(a.x, max(b.x, c.x)) * inv);
    int y0 = cell_coord(min(a.y, min(b.y, c.y)) * inv);
    int y1 = cell_coord(max(a.y, max(b.y, c.y)) * inv);
    int z0 = cell_coord(min(a.z, min(b.z, c.z)) * inv);
    int z1 = cell_coord(max(a.z, max(b.z, c.z)) * inv);
    // 病态的超大三角形不铺格子，与 CPU 的 200000 单元上限一致。
    float cells = float(x1 - x0 + 1) * float(y1 - y0 + 1) * float(z1 - z0 + 1);
    if (cells > 200000.0) return;

    for (int x = x0; x <= x1; ++x) {
        for (int y = y0; y <= y1; ++y) {
            for (int z = z0; z <= z1; ++z) {
                uint lo;
                uint hi;
                pack_cell(x, y, z, lo, hi);
                int bucket = int(cell_hash(lo, hi, uTriMask));
                push_entry(uTriHead, uTriPool, int(uTriCap), uCounter, bucket, t, lo, hi);
            }
        }
    }
}
)GLSL";

// 自接触扫描：顶点对邻近三角形求最近点，凹陷/接触强度用原子 max 合并。
const char* const kContact = R"GLSL(
uniform float uRange;
uniform float uDentAmt;
uniform int uTriMask;

vec3 closest_on_triangle(vec3 p, vec3 a, vec3 b, vec3 c, out vec3 bary) {
    vec3 q;
    vec3 ab = b - a;
    vec3 ac = c - a;
    vec3 ap = p - a;
    float d1 = dot(ab, ap);
    float d2 = dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0) {
        q = a;
    } else {
        vec3 bp = p - b;
        float d3 = dot(ab, bp);
        float d4 = dot(ac, bp);
        if (d3 >= 0.0 && d4 <= d3) {
            q = b;
        } else {
            float vc = d1 * d4 - d3 * d2;
            if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
                q = a + ab * (d1 / (d1 - d3));
            } else {
                vec3 cp = p - c;
                float d5 = dot(ab, cp);
                float d6 = dot(ac, cp);
                if (d6 >= 0.0 && d5 <= d6) {
                    q = c;
                } else {
                    float vb = d5 * d2 - d1 * d6;
                    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
                        q = a + ac * (d2 / (d2 - d6));
                    } else {
                        float va = d3 * d6 - d5 * d4;
                        if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
                            q = b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
                        } else {
                            float denom = 1.0 / (va + vb + vc);
                            float v = vb * denom;
                            float w = vc * denom;
                            q = a + ab * v + ac * w;
                        }
                    }
                }
            }
        }
    }
    float wa = length(cross(c - b, q - b));
    float wb = length(cross(a - c, q - c));
    float wc = length(cross(b - a, q - a));
    float sum = wa + wb + wc;
    if (sum > 1e-12) bary = vec3(wa, wb, wc) / sum;
    else bary = vec3(1.0, 0.0, 0.0);
    return q;
}

bool shares_edge(uint v, uint other) {
    int begin = int(topo[uNbOff + int(v)]);
    int end = int(topo[uNbOff + int(v) + 1]);
    for (int k = begin; k < end; ++k) {
        if (topo[uNbVert + k] == other) return true;
    }
    return false;
}

void atomic_max_positive(int off, uint i, float v) {
    // 负数 / NaN 的位模式在无符号比较里会压过正数，必须先挡住。
    if (!(v > 0.0)) return;
    atomicMax(dyn[off + int(i)], floatBitsToUint(v));
}

void main() {
    uint v = gl_GlobalInvocationID.x;
    if (v >= uint(uCn)) return;
    vec3 p = load3(uWeldPos, v);
    vec3 wn = load3(uWeldNrm, v);
    if (!finite3(p) || length(wn) < 1e-6) return;
    float firm_v = load1(uFirm, v);
    float r2 = uRange * uRange;
    int cx = cell_coord(p.x / uRange);
    int cy = cell_coord(p.y / uRange);
    int cz = cell_coord(p.z / uRange);

    for (int dx = -1; dx <= 1; ++dx) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dz = -1; dz <= 1; ++dz) {
                uint lo;
                uint hi;
                pack_cell(cx + dx, cy + dy, cz + dz, lo, hi);
                int bucket = int(cell_hash(lo, hi, uTriMask));
                uint node = grid[uTriHead + bucket];
                uint guard = 0u;
                // 上限取池容量：任何桶的链长都不可能超过总条目数，
                // 既能保证与 CPU 的遍历结果一致，又能防止异常数据死循环。
                while (node != 0u && guard < uint(uTriCap)) {
                    int base = uTriPool + int(node - 1u) * 4;
                    uint tid = grid[base];
                    uint next = grid[base + 1];
                    bool same_cell = grid[base + 2] == lo && grid[base + 3] == hi;
                    node = next;
                    ++guard;
                    if (!same_cell || tid >= uint(uT)) continue;

                    uint ta = topo[uTri + int(tid) * 3];
                    uint tb = topo[uTri + int(tid) * 3 + 1];
                    uint tc = topo[uTri + int(tid) * 3 + 2];
                    if (ta == v || tb == v || tc == v) continue;
                    // 先做最便宜的朝向筛选：绝大多数候选三角形在这一步就被剔除，
                    // 共边检查（线性扫描邻居表）只对真正可能接触的三角形执行。
                    vec3 tn = load3(uTriNrm, tid);
                    if (dot(tn, tn) < 0.25) continue;
                    if (dot(wn, tn) > -0.2) continue;
                    if (shares_edge(v, ta) || shares_edge(v, tb) || shares_edge(v, tc)) continue;

                    vec3 bary;
                    vec3 closest =
                        closest_on_triangle(p, load3(uWeldPos, ta), load3(uWeldPos, tb),
                                            load3(uWeldPos, tc), bary);
                    vec3 d = p - closest;
                    float d2 = dot(d, d);
                    if (d2 >= r2) continue;
                    // 仅有间隙不算挤压：只有真正压到表面上的顶点才计入，
                    // 否则衣物、头发和并拢的肢体在静止时也会被判定为接触，
                    // 表现为全身膨胀/融化。允许一个很小的贴合带吸收浮点误差。
                    float side = dot(d, tn);
                    float gap_band = uRange * 0.1;
                    if (side > gap_band) continue;
                    float gate = clamp((gap_band - side) / gap_band, 0.0, 1.0);
                    float strength = (1.0 - sqrt(d2) / uRange) * gate;
                    if (strength <= 0.0) continue;

                    atomic_max_positive(uContact, v, strength);
                    float depth = strength * uRange * 0.5 * uDentAmt;
                    atomic_max_positive(uDent, v, depth * (1.0 - firm_v));
                    atomic_max_positive(uContact, ta, strength * bary.x);
                    atomic_max_positive(uContact, tb, strength * bary.y);
                    atomic_max_positive(uContact, tc, strength * bary.z);
                    atomic_max_positive(uDent, ta, depth * (1.0 - load1(uFirm, ta)) * bary.x);
                    atomic_max_positive(uDent, tb, depth * (1.0 - load1(uFirm, tb)) * bary.y);
                    atomic_max_positive(uDent, tc, depth * (1.0 - load1(uFirm, tc)) * bary.z);
                }
            }
        }
    }
}
)GLSL";

// 接触点入桶：只有真实接触（>0.02）的顶点参与隆起 / Taper 的邻域查询。
const char* const kContactGrid = R"GLSL(
uniform float uInvBulge;
uniform int uContactMask;

void main() {
    uint v = gl_GlobalInvocationID.x;
    if (v >= uint(uCn)) return;
    if (!(load1(uContact, v) > 0.02)) return;
    vec3 p = load3(uWeldPos, v);
    int cx = cell_coord(p.x * uInvBulge);
    int cy = cell_coord(p.y * uInvBulge);
    int cz = cell_coord(p.z * uInvBulge);
    uint lo;
    uint hi;
    pack_cell(cx, cy, cz, lo, hi);
    int bucket = int(cell_hash(lo, hi, uContactMask));
    push_entry(uContactHead, uContactPool, int(uContactCap), uCounter + 1, bucket, v, lo, hi);
}
)GLSL";

// Corner Taper：折痕端部朝单侧受压，按方向各向异性回收隆起。
const char* const kTaper = R"GLSL(
uniform float uBulgeRadius;
uniform float uTaperAmount;
uniform float uTaperReach;
uniform float uTaperFalloff;
uniform int uContactMask;

void main() {
    uint v = gl_GlobalInvocationID.x;
    if (v >= uint(uCn)) return;
    if (!(uTaperAmount > 0.0)) return;
    if (!(load1(uContact, v) > 0.02)) return;

    float inv_bulge = 1.0 / max(uBulgeRadius, 1e-6);
    float taper_radius = max(uBulgeRadius * clamp(uTaperReach, 0.05, 3.0), 1e-5);
    float inv_taper = 1.0 / taper_radius;
    vec3 p = load3(uWeldPos, v);
    int cx = cell_coord(p.x * inv_bulge);
    int cy = cell_coord(p.y * inv_bulge);
    int cz = cell_coord(p.z * inv_bulge);

    vec3 dir = vec3(0.0);
    float total = 0.0;
    for (int dx = -1; dx <= 1; ++dx) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dz = -1; dz <= 1; ++dz) {
                uint lo;
                uint hi;
                pack_cell(cx + dx, cy + dy, cz + dz, lo, hi);
                int bucket = int(cell_hash(lo, hi, uContactMask));
                uint node = grid[uContactHead + bucket];
                uint guard = 0u;
                while (node != 0u && guard < uint(uContactCap)) {
                    int base = uContactPool + int(node - 1u) * 4;
                    uint c = grid[base];
                    uint next = grid[base + 1];
                    bool same_cell = grid[base + 2] == lo && grid[base + 3] == hi;
                    node = next;
                    ++guard;
                    if (!same_cell || c == v) continue;
                    vec3 d = load3(uWeldPos, c) - p;
                    float d2 = dot(d, d);
                    if (d2 < 1e-12 || d2 >= taper_radius * taper_radius) continue;
                    float dist = sqrt(d2);
                    float w = load1(uContact, c) * (1.0 - dist * inv_taper);
                    if (w <= 0.0) continue;
                    dir += d * (w / dist);
                    total += w;
                }
            }
        }
    }
    float aniso = total > 1e-6 ? length(dir) / total : 1.0;
    float t = clamp((aniso - 0.55) / 0.45, 0.0, 1.0);
    store1(uTaper, v, clamp(uTaperAmount, 0.0, 1.0) * pow(t, clamp(uTaperFalloff, 0.1, 4.0)));
}
)GLSL";

// 隆起：接触点把周围表面向外推，形成挤压外翻。
const char* const kBulge = R"GLSL(
uniform float uBulgeAmplitude;
uniform float uBulgeRadius;
uniform int uContactMask;

void main() {
    uint v = gl_GlobalInvocationID.x;
    if (v >= uint(uCn)) return;
    if (!(uBulgeAmplitude > 0.0)) return;
    float soft = 1.0 - load1(uFirm, v);
    float rim = 1.0 - clamp(load1(uContact, v), 0.0, 1.0);
    vec3 p = load3(uWeldPos, v);
    if (soft <= 0.01 || rim <= 0.0 || !finite3(p)) return;

    float inv_bulge = 1.0 / max(uBulgeRadius, 1e-6);
    int cx = cell_coord(p.x * inv_bulge);
    int cy = cell_coord(p.y * inv_bulge);
    int cz = cell_coord(p.z * inv_bulge);
    float best = 0.0;
    for (int dx = -1; dx <= 1; ++dx) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dz = -1; dz <= 1; ++dz) {
                uint lo;
                uint hi;
                pack_cell(cx + dx, cy + dy, cz + dz, lo, hi);
                int bucket = int(cell_hash(lo, hi, uContactMask));
                uint node = grid[uContactHead + bucket];
                uint guard = 0u;
                while (node != 0u && guard < uint(uContactCap)) {
                    int base = uContactPool + int(node - 1u) * 4;
                    uint c = grid[base];
                    uint next = grid[base + 1];
                    bool same_cell = grid[base + 2] == lo && grid[base + 3] == hi;
                    node = next;
                    ++guard;
                    if (!same_cell) continue;
                    vec3 d = load3(uWeldPos, c) - p;
                    float d2 = dot(d, d);
                    if (d2 >= uBulgeRadius * uBulgeRadius) continue;
                    float t = 1.0 - sqrt(d2) / uBulgeRadius;
                    if (t <= 0.0) continue;
                    float src = load1(uContact, c) * (1.0 - load1(uTaper, c));
                    best = max(best, src * t * t);
                }
            }
        }
    }
    store1(uBulge, v, uBulgeAmplitude * best * rim * soft);
}
)GLSL";

// 位移合成：法线方向的凹陷/隆起 + 外部碰撞体推出，并立即写回焊接位置，
// 供重算法线使用（与 CPU 的 wp += d 一致）。
const char* const kSolve = R"GLSL(
uniform float uRange;
uniform float uBulgeRadius;
uniform int uColliderCount;

vec3 closest_on_segment(vec3 p, vec3 a, vec3 b) {
    vec3 ab = b - a;
    float len2 = dot(ab, ab);
    if (len2 < 1e-12) return a;
    float t = clamp(dot(p - a, ab) / len2, 0.0, 1.0);
    return a + ab * t;
}

void main() {
    uint c = gl_GlobalInvocationID.x;
    if (c >= uint(uCn)) return;
    vec3 p = load3(uWeldPos, c);
    if (!finite3(p)) return;
    float move = load1(uBulge, c) - load1(uDent, c);
    vec3 total = load3(uWeldNrm, c) * move;

    for (int i = 0; i < uColliderCount; ++i) {
        vec4 c0 = cols[i * 2];
        vec4 c1 = cols[i * 2 + 1];
        vec3 a = c0.xyz;
        vec3 b = c1.xyz;
        float radius = max(c0.w, 1e-5);
        vec3 closest = distance(a, b) > 1e-6 ? closest_on_segment(p, a, b) : a;
        vec3 d = p - closest;
        float dist = length(d);
        if (dist < 1e-6 || dist > radius + uBulgeRadius) continue;
        float soft = 1.0 - clamp(c1.w, 0.0, 1.0);
        if (soft <= 0.0) continue;
        vec3 dir = d * (1.0 / dist);
        if (dist < radius) {
            total += dir * ((radius - dist) * soft);
        } else {
            float t = 1.0 - (dist - radius) / uBulgeRadius;
            total += dir * (uRange * 0.35 * t * t * soft);
        }
    }

    if (total.x == 0.0 && total.y == 0.0 && total.z == 0.0) return;
    store3(uDelta, c, total);
    store3(uWeldPos, c, p + total);
}
)GLSL";

// 形变后重算法线：每个焊接顶点累加相邻三角形法线。
// 邻接表按三角形顺序构建，累加顺序与 CPU 一致。
const char* const kWeldNormal = R"GLSL(
void main() {
    uint v = gl_GlobalInvocationID.x;
    if (v >= uint(uCn)) return;
    int begin = int(topo[uVtriOff + int(v)]);
    int end = int(topo[uVtriOff + int(v) + 1]);
    vec3 acc = vec3(0.0);
    for (int k = begin; k < end; ++k) {
        uint t = topo[uVtriId + k];
        uint ta = topo[uTri + int(t) * 3];
        uint tb = topo[uTri + int(t) * 3 + 1];
        uint tc = topo[uTri + int(t) * 3 + 2];
        vec3 fn = cross(load3(uWeldPos, tb) - load3(uWeldPos, ta),
                        load3(uWeldPos, tc) - load3(uWeldPos, ta));
        if (!finite3(fn) || dot(fn, fn) < 1e-16) continue;
        acc += fn;
    }
    if (length(acc) > 1e-8) store3(uWeldNrm, v, normalize(acc));
}
)GLSL";

// 回写原始顶点：只有真正移动过的焊接点才改位置与法线，避免磨平硬边。
const char* const kFinalize = R"GLSL(
uniform float uMovedEps;

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= uint(uN)) return;
    uint c = topo[uCanon + int(i)];
    vec3 d = load3(uDelta, c);
    if (length(d) < uMovedEps) return;
    gpos[i].xyz += d;
    gnrm[i].xyz = load3(uWeldNrm, c);
}
)GLSL";

// Smooth Pops：连续帧之间对肉体位移做指数混合；不连续时只重置历史。
const char* const kSmooth = R"GLSL(
uniform float uKeep;
uniform int uReset;

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= uint(uN)) return;
    vec3 cur = gpos[i].xyz;
    vec3 snap = load3(uSkinSnap, i);
    if (uReset != 0) {
        store3(uPrev, i, cur - snap);
        return;
    }
    if (!(uKeep > 0.0)) return;
    vec3 pv = load3(uPrev, i);
    pv = pv * uKeep + (cur - snap) * (1.0 - uKeep);
    store3(uPrev, i, pv);
    gpos[i].xyz = snap + pv;
}
)GLSL";

}  // namespace

GpuFleshSolver::~GpuFleshSolver() { release(); }

bool GpuFleshSolver::init(std::string* error) {
    if (ready_) return true;
    auto build = [&](ShaderProgram& program, const char* body, const char* name) {
        std::string source = std::string(kFleshCommon) + body;
        std::string err;
        if (!program.build_compute(source.c_str(), &err)) {
            error_ = std::string("GPU 肉体（") + name + "）：" + err;
            if (error) *error = error_;
            return false;
        }
        return true;
    };
    if (!build(skin_snapshot_, kSkinSnapshot, "快照") ||
        !build(weld_prepare_, kWeldPrepare, "焊接准备") ||
        !build(tri_grid_, kTriGrid, "三角形网格") ||
        !build(contact_, kContact, "自接触") || !build(contact_grid_, kContactGrid, "接触网格") ||
        !build(taper_, kTaper, "Corner Taper") || !build(bulge_, kBulge, "隆起") ||
        !build(solve_, kSolve, "位移") || !build(weld_normal_, kWeldNormal, "法线") ||
        !build(finalize_, kFinalize, "回写") || !build(smooth_, kSmooth, "Smooth Pops")) {
        return false;
    }
    ready_ = true;
    error_.clear();
    return true;
}

void GpuFleshSolver::release() {
    delete_buffers();
    ready_ = false;
}

void GpuFleshSolver::delete_buffers() {
    GLuint buffers[] = {static_ssbo_, dynamic_ssbo_, grid_ssbo_, collider_ssbo_};
    for (GLuint b : buffers) {
        if (b) glDeleteBuffers(1, &b);
    }
    static_ssbo_ = 0;
    dynamic_ssbo_ = 0;
    grid_ssbo_ = 0;
    collider_ssbo_ = 0;
    collider_capacity_ = 0;
    static_words_ = 0;
    dynamic_words_ = 0;
    grid_words_ = 0;
    vertex_count_ = 0;
    weld_count_ = 0;
    tri_count_ = 0;
    static_data_.clear();
    prev_valid_ = false;
    prev_frame_ = -1e9f;
    prev_key_ = 0;
}

void GpuFleshSolver::clear() { delete_buffers(); }

bool GpuFleshSolver::reset(const Model& model, std::string* error) {
    clear();
    if (!ready_) {
        if (error) *error = error_.empty() ? "GPU 肉体计算着色器不可用" : error_;
        return false;
    }
    return build_static(model, error);
}

bool GpuFleshSolver::build_static(const Model& model, std::string* error) {
    const int n = static_cast<int>(model.vertices.size());
    vertex_count_ = n;
    if (n <= 0) {
        if (error) *error = "模型没有顶点";
        return false;
    }

    // 身高决定接触距离，与 CPU FleshSolver::reset 完全一致。
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

    // 焊接：1e-4 量化后的静止坐标相同即视为同一点，重复顶点共享形变与法线。
    std::map<std::array<int, 3>, std::uint32_t> weld;
    std::vector<std::uint32_t> canon(static_cast<std::size_t>(n), 0);
    std::vector<std::uint32_t> rep;
    rep.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const Vec3 p = model.vertices.pos[static_cast<std::size_t>(i)];
        const std::array<int, 3> key{weld_coord(p.x), weld_coord(p.y), weld_coord(p.z)};
        auto [it, inserted] = weld.emplace(key, static_cast<std::uint32_t>(rep.size()));
        if (inserted) rep.push_back(static_cast<std::uint32_t>(i));
        canon[static_cast<std::size_t>(i)] = it->second;
    }
    const int cn = static_cast<int>(rep.size());
    weld_count_ = cn;

    // 焊接后的三角形：退化三角形不参与接触。
    std::vector<std::array<std::uint32_t, 3>> tris;
    tris.reserve(model.indices.size() / 3);
    const std::size_t index_count = model.indices.size();
    for (std::size_t t = 0; t + 2 < index_count; t += 3) {
        const std::uint32_t i0 = model.indices[t];
        const std::uint32_t i1 = model.indices[t + 1];
        const std::uint32_t i2 = model.indices[t + 2];
        if (i0 >= static_cast<std::uint32_t>(n) || i1 >= static_cast<std::uint32_t>(n) ||
            i2 >= static_cast<std::uint32_t>(n))
            continue;
        const std::uint32_t a = canon[i0];
        const std::uint32_t b = canon[i1];
        const std::uint32_t c = canon[i2];
        if (a == b || b == c || a == c) continue;
        tris.push_back({a, b, c});
    }
    const int tri_count = static_cast<int>(tris.size());
    tri_count_ = tri_count;

    // 重复顶点 CSR：每个焊接点按原始索引顺序列出它的所有副本。
    std::vector<std::uint32_t> dup_off(static_cast<std::size_t>(cn) + 1, 0);
    for (int i = 0; i < n; ++i) ++dup_off[canon[static_cast<std::size_t>(i)] + 1];
    for (int c = 0; c < cn; ++c) dup_off[static_cast<std::size_t>(c) + 1] +=
        dup_off[static_cast<std::size_t>(c)];
    std::vector<std::uint32_t> dup_vert(static_cast<std::size_t>(n), 0);
    std::vector<std::uint32_t> cursor = dup_off;
    for (int i = 0; i < n; ++i) {
        dup_vert[cursor[canon[static_cast<std::size_t>(i)]]++] = static_cast<std::uint32_t>(i);
    }

    // 每个焊接点取代表顶点的骨骼权重（区域硬度用）。
    std::vector<std::uint32_t> rep_bone(static_cast<std::size_t>(cn) * 4, 0);
    std::vector<std::uint32_t> rep_weight(static_cast<std::size_t>(cn) * 4, 0);
    std::vector<std::uint32_t> rep_count(static_cast<std::size_t>(cn), 0);
    for (int c = 0; c < cn; ++c) {
        const std::size_t vi = static_cast<std::size_t>(rep[static_cast<std::size_t>(c)]);
        const int count =
            std::clamp(influence_count(model.vertices.wtype[vi]), 1, 4);
        rep_count[static_cast<std::size_t>(c)] = static_cast<std::uint32_t>(count);
        for (int k = 0; k < 4; ++k) {
            const std::size_t o = static_cast<std::size_t>(c) * 4 + static_cast<std::size_t>(k);
            rep_bone[o] = static_cast<std::uint32_t>(model.vertices.bones[vi][static_cast<std::size_t>(k)]);
            float w = model.vertices.weights[vi][static_cast<std::size_t>(k)];
            std::uint32_t bits = 0;
            std::memcpy(&bits, &w, sizeof(bits));
            rep_weight[o] = bits;
        }
    }

    // 共边邻居（无重复）与顶点相邻三角形 CSR。
    std::vector<std::unordered_set<std::uint32_t>> adj(static_cast<std::size_t>(cn));
    std::vector<std::vector<std::uint32_t>> vtri(static_cast<std::size_t>(cn));
    std::unordered_set<std::uint64_t> edge_seen;
    edge_seen.reserve(tris.size() * 3);
    auto add_edge = [&](std::uint32_t a, std::uint32_t b) {
        const std::uint64_t key =
            (static_cast<std::uint64_t>(a) << 32) | static_cast<std::uint64_t>(b);
        if (edge_seen.insert(key).second) {
            adj[static_cast<std::size_t>(a)].insert(b);
            adj[static_cast<std::size_t>(b)].insert(a);
        }
    };
    for (int t = 0; t < tri_count; ++t) {
        const std::array<std::uint32_t, 3>& tri = tris[static_cast<std::size_t>(t)];
        add_edge(tri[0], tri[1]);
        add_edge(tri[1], tri[2]);
        add_edge(tri[2], tri[0]);
        for (std::uint32_t v : tri) {
            if (v < static_cast<std::uint32_t>(cn))
                vtri[static_cast<std::size_t>(v)].push_back(static_cast<std::uint32_t>(t));
        }
    }
    std::vector<std::uint32_t> nb_off(static_cast<std::size_t>(cn) + 1, 0);
    for (int c = 0; c < cn; ++c) {
        nb_off[static_cast<std::size_t>(c) + 1] =
            nb_off[static_cast<std::size_t>(c)] +
            static_cast<std::uint32_t>(adj[static_cast<std::size_t>(c)].size());
    }
    std::vector<std::uint32_t> nb_vert(nb_off[static_cast<std::size_t>(cn)], 0);
    for (int c = 0; c < cn; ++c) {
        std::vector<std::uint32_t> list(adj[static_cast<std::size_t>(c)].begin(),
                                        adj[static_cast<std::size_t>(c)].end());
        std::sort(list.begin(), list.end());
        std::copy(list.begin(), list.end(),
                  nb_vert.begin() + static_cast<std::ptrdiff_t>(nb_off[static_cast<std::size_t>(c)]));
    }
    std::vector<std::uint32_t> vtri_off(static_cast<std::size_t>(cn) + 1, 0);
    for (int c = 0; c < cn; ++c) {
        vtri_off[static_cast<std::size_t>(c) + 1] =
            vtri_off[static_cast<std::size_t>(c)] +
            static_cast<std::uint32_t>(vtri[static_cast<std::size_t>(c)].size());
    }
    std::vector<std::uint32_t> vtri_id(vtri_off[static_cast<std::size_t>(cn)], 0);
    for (int c = 0; c < cn; ++c) {
        std::copy(vtri[static_cast<std::size_t>(c)].begin(),
                  vtri[static_cast<std::size_t>(c)].end(),
                  vtri_id.begin() +
                      static_cast<std::ptrdiff_t>(vtri_off[static_cast<std::size_t>(c)]));
    }

    // ---- 静态缓冲布局
    o_canon_ = 0;
    o_dup_off_ = o_canon_ + static_cast<std::uint32_t>(n);
    o_dup_vert_ = o_dup_off_ + static_cast<std::uint32_t>(cn) + 1;
    o_rep_bone_ = o_dup_vert_ + static_cast<std::uint32_t>(n);
    o_rep_weight_ = o_rep_bone_ + static_cast<std::uint32_t>(cn) * 4;
    o_rep_count_ = o_rep_weight_ + static_cast<std::uint32_t>(cn) * 4;
    o_nb_off_ = o_rep_count_ + static_cast<std::uint32_t>(cn);
    o_nb_vert_ = o_nb_off_ + static_cast<std::uint32_t>(cn) + 1;
    o_vtri_off_ = o_nb_vert_ + static_cast<std::uint32_t>(nb_vert.size());
    o_vtri_id_ = o_vtri_off_ + static_cast<std::uint32_t>(cn) + 1;
    o_tri_ = o_vtri_id_ + static_cast<std::uint32_t>(vtri_id.size());
    static_words_ = o_tri_ + static_cast<std::uint32_t>(tri_count) * 3;

    static_data_.assign(static_words_, 0);
    std::copy(canon.begin(), canon.end(), static_data_.begin() + o_canon_);
    std::copy(dup_off.begin(), dup_off.end(), static_data_.begin() + o_dup_off_);
    std::copy(dup_vert.begin(), dup_vert.end(), static_data_.begin() + o_dup_vert_);
    std::copy(rep_bone.begin(), rep_bone.end(), static_data_.begin() + o_rep_bone_);
    std::copy(rep_weight.begin(), rep_weight.end(), static_data_.begin() + o_rep_weight_);
    std::copy(rep_count.begin(), rep_count.end(), static_data_.begin() + o_rep_count_);
    std::copy(nb_off.begin(), nb_off.end(), static_data_.begin() + o_nb_off_);
    std::copy(nb_vert.begin(), nb_vert.end(), static_data_.begin() + o_nb_vert_);
    std::copy(vtri_off.begin(), vtri_off.end(), static_data_.begin() + o_vtri_off_);
    std::copy(vtri_id.begin(), vtri_id.end(), static_data_.begin() + o_vtri_id_);
    for (int t = 0; t < tri_count; ++t) {
        const std::array<std::uint32_t, 3>& tri = tris[static_cast<std::size_t>(t)];
        static_data_[o_tri_ + static_cast<std::uint32_t>(t) * 3] = tri[0];
        static_data_[o_tri_ + static_cast<std::uint32_t>(t) * 3 + 1] = tri[1];
        static_data_[o_tri_ + static_cast<std::uint32_t>(t) * 3 + 2] = tri[2];
    }

    // ---- 动态缓冲布局（float 以位模式存放）
    const std::uint32_t un = static_cast<std::uint32_t>(n);
    const std::uint32_t ucn = static_cast<std::uint32_t>(cn);
    const std::uint32_t ut = static_cast<std::uint32_t>(tri_count);
    o_weld_pos_ = 0;
    o_weld_nrm_ = o_weld_pos_ + ucn * 4;
    o_tri_nrm_ = o_weld_nrm_ + ucn * 4;
    o_firm_ = o_tri_nrm_ + ut * 4;
    o_taper_ = o_firm_ + ucn;
    o_bulge_ = o_taper_ + ucn;
    o_delta_ = o_bulge_ + ucn;
    o_contact_ = o_delta_ + ucn * 4;
    o_dent_ = o_contact_ + ucn;
    o_prev_ = o_dent_ + ucn;
    o_skin_snap_ = o_prev_ + un * 4;
    dynamic_words_ = o_skin_snap_ + un * 4;

    // ---- 空间哈希布局
    tri_head_off_ = 0;
    tri_head_count_ = next_pow2(std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(std::min<std::size_t>(static_cast<std::size_t>(tri_count) * 2,
                                                        1u << 21)),
        1u << 15, 1u << 21));
    contact_head_off_ = tri_head_off_ + tri_head_count_;
    contact_head_count_ = next_pow2(std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(std::min<std::size_t>(static_cast<std::size_t>(cn) * 2,
                                                        1u << 20)),
        1u << 13, 1u << 20));
    counter_off_ = contact_head_off_ + contact_head_count_;
    tri_pool_off_ = counter_off_ + 2;
    tri_pool_cap_ = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(std::min<std::size_t>(static_cast<std::size_t>(tri_count) * 8,
                                                        1u << 22)),
        1u << 16, 1u << 22);
    contact_pool_off_ = tri_pool_off_ + tri_pool_cap_ * 4;
    contact_pool_cap_ = std::max<std::uint32_t>(ucn, 1u);
    grid_words_ = contact_pool_off_ + contact_pool_cap_ * 4;

    // ---- 上传
    glGenBuffers(1, &static_ssbo_);
    glBindBuffer(glc::GL_SHADER_STORAGE_BUFFER, static_ssbo_);
    glBufferData(glc::GL_SHADER_STORAGE_BUFFER,
                 static_cast<GLsizeiptr>(static_data_.size() * sizeof(std::uint32_t)),
                 static_data_.data(), glc::GL_STATIC_DRAW);

    glGenBuffers(1, &dynamic_ssbo_);
    glBindBuffer(glc::GL_SHADER_STORAGE_BUFFER, dynamic_ssbo_);
    glBufferData(glc::GL_SHADER_STORAGE_BUFFER,
                 static_cast<GLsizeiptr>(static_cast<std::size_t>(dynamic_words_) * 4), nullptr,
                 glc::GL_DYNAMIC_DRAW);
    glClearBufferData(glc::GL_SHADER_STORAGE_BUFFER, glc::GL_R32UI, glc::GL_RED_INTEGER,
                      glc::GL_UNSIGNED_INT, nullptr);

    glGenBuffers(1, &grid_ssbo_);
    glBindBuffer(glc::GL_SHADER_STORAGE_BUFFER, grid_ssbo_);
    glBufferData(glc::GL_SHADER_STORAGE_BUFFER,
                 static_cast<GLsizeiptr>(static_cast<std::size_t>(grid_words_) * 4), nullptr,
                 glc::GL_DYNAMIC_DRAW);
    glClearBufferData(glc::GL_SHADER_STORAGE_BUFFER, glc::GL_R32UI, glc::GL_RED_INTEGER,
                      glc::GL_UNSIGNED_INT, nullptr);

    collider_capacity_ = kMaxColliders;
    glGenBuffers(1, &collider_ssbo_);
    glBindBuffer(glc::GL_SHADER_STORAGE_BUFFER, collider_ssbo_);
    glBufferData(glc::GL_SHADER_STORAGE_BUFFER,
                 static_cast<GLsizeiptr>(static_cast<std::size_t>(collider_capacity_) * 8 *
                                         sizeof(float)),
                 nullptr, glc::GL_DYNAMIC_DRAW);
    glBindBuffer(glc::GL_SHADER_STORAGE_BUFFER, 0);

    set_constant_uniforms();
    return true;
}

void GpuFleshSolver::set_constant_uniforms() {
    ShaderProgram* programs[] = {&skin_snapshot_, &weld_prepare_, &tri_grid_,  &contact_,
                                 &contact_grid_,  &taper_,        &bulge_,     &solve_,
                                 &weld_normal_,   &finalize_,     &smooth_};
    for (ShaderProgram* p : programs) {
        if (!p->id()) continue;
        p->use();
        p->set1i("uN", vertex_count_);
        p->set1i("uCn", weld_count_);
        p->set1i("uT", tri_count_);
        p->set1i("uCanon", static_cast<int>(o_canon_));
        p->set1i("uDupOff", static_cast<int>(o_dup_off_));
        p->set1i("uDupVert", static_cast<int>(o_dup_vert_));
        p->set1i("uRepBone", static_cast<int>(o_rep_bone_));
        p->set1i("uRepWeight", static_cast<int>(o_rep_weight_));
        p->set1i("uRepCount", static_cast<int>(o_rep_count_));
        p->set1i("uNbOff", static_cast<int>(o_nb_off_));
        p->set1i("uNbVert", static_cast<int>(o_nb_vert_));
        p->set1i("uVtriOff", static_cast<int>(o_vtri_off_));
        p->set1i("uVtriId", static_cast<int>(o_vtri_id_));
        p->set1i("uTri", static_cast<int>(o_tri_));
        p->set1i("uWeldPos", static_cast<int>(o_weld_pos_));
        p->set1i("uWeldNrm", static_cast<int>(o_weld_nrm_));
        p->set1i("uTriNrm", static_cast<int>(o_tri_nrm_));
        p->set1i("uFirm", static_cast<int>(o_firm_));
        p->set1i("uTaper", static_cast<int>(o_taper_));
        p->set1i("uBulge", static_cast<int>(o_bulge_));
        p->set1i("uDelta", static_cast<int>(o_delta_));
        p->set1i("uContact", static_cast<int>(o_contact_));
        p->set1i("uDent", static_cast<int>(o_dent_));
        p->set1i("uPrev", static_cast<int>(o_prev_));
        p->set1i("uSkinSnap", static_cast<int>(o_skin_snap_));
        p->set1i("uTriHead", static_cast<int>(tri_head_off_));
        p->set1i("uTriHeadCount", static_cast<int>(tri_head_count_));
        p->set1i("uTriMask", static_cast<int>(tri_head_count_ - 1));
        p->set1i("uContactHead", static_cast<int>(contact_head_off_));
        p->set1i("uContactHeadCount", static_cast<int>(contact_head_count_));
        p->set1i("uContactMask", static_cast<int>(contact_head_count_ - 1));
        p->set1i("uCounter", static_cast<int>(counter_off_));
        p->set1i("uTriPool", static_cast<int>(tri_pool_off_));
        p->set1i("uTriCap", static_cast<int>(tri_pool_cap_));
        p->set1i("uContactPool", static_cast<int>(contact_pool_off_));
        p->set1i("uContactCap", static_cast<int>(contact_pool_cap_));
    }
}

void GpuFleshSolver::upload_colliders(const std::vector<FleshColliderShape>& colliders) {
    const int count = std::min(static_cast<int>(colliders.size()), collider_capacity_);
    if (count <= 0) return;
    std::vector<float> data(static_cast<std::size_t>(count) * 8, 0.0f);
    for (int i = 0; i < count; ++i) {
        const FleshColliderShape& c = colliders[static_cast<std::size_t>(i)];
        const std::size_t o = static_cast<std::size_t>(i) * 8;
        data[o + 0] = c.a.x;
        data[o + 1] = c.a.y;
        data[o + 2] = c.a.z;
        data[o + 3] = std::max(c.radius, 1e-5f);
        data[o + 4] = c.b.x;
        data[o + 5] = c.b.y;
        data[o + 6] = c.b.z;
        data[o + 7] = std::clamp(c.firmness, 0.0f, 1.0f);
    }
    glBindBuffer(glc::GL_SHADER_STORAGE_BUFFER, collider_ssbo_);
    glBufferSubData(glc::GL_SHADER_STORAGE_BUFFER, 0,
                    static_cast<GLsizeiptr>(data.size() * sizeof(float)), data.data());
}

bool GpuFleshSolver::solve(const GpuFleshParams& params,
                           const std::vector<FleshColliderShape>& colliders, std::uint64_t key,
                           GLuint out_pos_ssbo, GLuint out_nrm_ssbo, std::string* error) {
    dispatch_count_ = 0;
    if (!ready_ || !has_model() || !out_pos_ssbo || !out_nrm_ssbo) {
        if (error) *error = error_.empty() ? "GPU 肉体求解器不可用" : error_;
        return false;
    }

    const float range = std::max(1e-5f, height_ * std::clamp(params.range, 0.001f, 0.08f));
    const float width = std::clamp(params.width, 0.5f, 12.0f);
    const float bulge_radius = range * width;
    const float inv_bulge = 1.0f / std::max(bulge_radius, 1e-6f);
    const float dent_amt = std::clamp(params.dent, 0.0f, 1.0f);
    const float amplitude = range * std::clamp(params.bulge, 0.0f, 2.0f) * 0.75f;
    const float moved_eps = range * 1e-4f;

    const bool smooth_on = params.smooth > 0.0f;
    bool contiguous = false;
    if (smooth_on) {
        contiguous = prev_valid_ &&
                     std::fabs(params.frame - (prev_frame_ + 1.0f)) < 0.75f && key == prev_key_;
    }
    const float keep = std::clamp(std::clamp(params.smooth, 0.0f, 0.9f) * 0.8f, 0.0f, 0.9f);

    // 桶头 + 计数器清零（入口池不清：桶头清零后旧入口不可达）。
    glBindBuffer(glc::GL_SHADER_STORAGE_BUFFER, grid_ssbo_);
    glClearBufferSubData(glc::GL_SHADER_STORAGE_BUFFER, glc::GL_R32UI, 0,
                         static_cast<GLsizeiptr>(static_cast<std::size_t>(counter_off_ + 2) *
                                                 sizeof(std::uint32_t)),
                         glc::GL_RED_INTEGER, glc::GL_UNSIGNED_INT, nullptr);

    upload_colliders(colliders);
    const int collider_count = std::min(static_cast<int>(colliders.size()), collider_capacity_);

    glBindBufferBase(glc::GL_SHADER_STORAGE_BUFFER, kBindStatic, static_ssbo_);
    glBindBufferBase(glc::GL_SHADER_STORAGE_BUFFER, kBindDynamic, dynamic_ssbo_);
    glBindBufferBase(glc::GL_SHADER_STORAGE_BUFFER, kBindGrid, grid_ssbo_);
    glBindBufferBase(glc::GL_SHADER_STORAGE_BUFFER, kBindPos, out_pos_ssbo);
    glBindBufferBase(glc::GL_SHADER_STORAGE_BUFFER, kBindNrm, out_nrm_ssbo);
    glBindBufferBase(glc::GL_SHADER_STORAGE_BUFFER, kBindCollider, collider_ssbo_);

    auto dispatch = [&](ShaderProgram& program, int count) {
        if (count <= 0) return;
        const GLuint groups = static_cast<GLuint>((count + 63) / 64);
        glDispatchCompute(groups, 1, 1);
        glMemoryBarrier(glc::GL_SHADER_STORAGE_BARRIER_BIT |
                        glc::GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);
        ++dispatch_count_;
    };

    if (smooth_on) {
        skin_snapshot_.use();
        dispatch(skin_snapshot_, vertex_count_);
    }

    weld_prepare_.use();
    weld_prepare_.set1f("uFirmness", params.firmness);
    weld_prepare_.set1i("uRegionBone", params.region_bone);
    weld_prepare_.set1f("uRegionFirmness", params.region_firmness);
    dispatch(weld_prepare_, weld_count_);

    tri_grid_.use();
    tri_grid_.set1f("uRange", range);
    dispatch(tri_grid_, tri_count_);

    contact_.use();
    contact_.set1f("uRange", range);
    contact_.set1f("uDentAmt", dent_amt);
    dispatch(contact_, weld_count_);

    contact_grid_.use();
    contact_grid_.set1f("uInvBulge", inv_bulge);
    dispatch(contact_grid_, weld_count_);

    taper_.use();
    taper_.set1f("uBulgeRadius", bulge_radius);
    taper_.set1f("uTaperAmount", params.taper);
    taper_.set1f("uTaperReach", params.taper_reach);
    taper_.set1f("uTaperFalloff", params.taper_falloff);
    dispatch(taper_, weld_count_);

    bulge_.use();
    bulge_.set1f("uBulgeAmplitude", amplitude);
    bulge_.set1f("uBulgeRadius", bulge_radius);
    dispatch(bulge_, weld_count_);

    solve_.use();
    solve_.set1f("uRange", range);
    solve_.set1f("uBulgeRadius", bulge_radius);
    solve_.set1i("uColliderCount", collider_count);
    dispatch(solve_, weld_count_);

    weld_normal_.use();
    dispatch(weld_normal_, weld_count_);

    finalize_.use();
    finalize_.set1f("uMovedEps", moved_eps);
    dispatch(finalize_, vertex_count_);

    if (smooth_on) {
        smooth_.use();
        smooth_.set1f("uKeep", keep);
        smooth_.set1i("uReset", contiguous ? 0 : 1);
        dispatch(smooth_, vertex_count_);
        prev_valid_ = true;
        prev_frame_ = params.frame;
        prev_key_ = key;
    }
    return true;
}

}  // namespace pmx
