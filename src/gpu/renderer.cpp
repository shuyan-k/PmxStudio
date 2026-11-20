#include "gpu/renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <unordered_set>

#include "core/fs.hpp"
#include "core/skinning.hpp"
#include "physics/physics.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace pmx {
namespace {

constexpr int kFloatsPerVertex = 32;  // 8 vec4

// 刚体按组着色，与 Python 版 theme.py 的 GROUP_COLORS 完全一致。
const Vec3 kGroupColors[16] = {
    {0.937f, 0.420f, 0.420f}, {0.941f, 0.702f, 0.357f}, {0.910f, 0.831f, 0.353f},
    {0.369f, 0.788f, 0.541f}, {0.220f, 0.839f, 0.769f}, {0.310f, 0.616f, 1.000f},
    {0.561f, 0.486f, 0.941f}, {0.937f, 0.486f, 0.753f}, {0.753f, 0.518f, 0.341f},
    {0.553f, 0.608f, 0.710f}, {0.439f, 0.784f, 0.937f}, {0.651f, 0.788f, 0.369f},
    {0.898f, 0.561f, 0.353f}, {0.714f, 0.647f, 0.910f}, {0.498f, 0.659f, 0.627f},
    {0.788f, 0.788f, 0.788f},
};
constexpr GLuint kBindingBones = 0;
constexpr GLuint kBindingRest = 1;
constexpr GLuint kBindingVerts = 2;
constexpr GLuint kBindingPos = 3;
constexpr GLuint kBindingNrm = 4;
constexpr GLuint kBindingInstances = 5;

// 静止坐标指纹（FNV-1a）：形态改变时让肉体烘焙缓存失效。
std::uint64_t hash_rest_positions(const std::vector<Vec3>& pos) {
    std::uint64_t h = 1469598103934665603ull;
    for (const Vec3& p : pos) {
        const float values[3] = {p.x, p.y, p.z};
        for (float v : values) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &v, sizeof(bits));
            h ^= bits;
            h *= 1099511628211ull;
        }
    }
    return h;
}

// 关闭物理后端时（-DPMX_ENABLE_PHYSICS=OFF）这些调用退化为空实现。
#ifdef PMX_ENABLE_PHYSICS
inline bool query_body(const Physics* p, int i, Vec3& pos, Quat& q) {
    return p && p->body_world(i, pos, q);
}
inline Mat4 query_bone_offset(const Physics* p, int i) {
    return p ? p->bone_offset(i) : identity4();
}
#else
inline bool query_body(const Physics*, int, Vec3&, Quat&) { return false; }
inline Mat4 query_bone_offset(const Physics*, int) { return identity4(); }
#endif

// ---------------------------------------------------------------- GLSL
const char* kSkinCS = R"(#version 460 core
layout(local_size_x = 64) in;

layout(std430, binding = 0) readonly buffer BoneSkin { mat4 skin[]; };
layout(std430, binding = 1) readonly buffer BoneRest { vec4 rest[]; };
layout(std430, binding = 2) readonly buffer VertIn { vec4 verts[]; };
layout(std430, binding = 3) writeonly buffer PosOut { vec4 outPos[]; };
layout(std430, binding = 4) writeonly buffer NrmOut { vec4 outNrm[]; };

vec4 qmul(vec4 a, vec4 b) {
    return vec4(a.w * b.xyz + b.w * a.xyz + cross(a.xyz, b.xyz),
                a.w * b.w - dot(a.xyz, b.xyz));
}

vec3 rotate_q(vec4 q, vec3 v) {
    vec3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

vec4 quat_from_mat(mat3 m) {
    // GLSL 的 m[列][行]，与常规按 (行,列) 书写的公式索引相反，差分项要交换。
    float tr = m[0][0] + m[1][1] + m[2][2];
    vec4 q;
    if (tr > 0.0) {
        float s = sqrt(tr + 1.0) * 2.0;
        q.w = 0.25 * s;
        q.x = (m[1][2] - m[2][1]) / s;
        q.y = (m[2][0] - m[0][2]) / s;
        q.z = (m[0][1] - m[1][0]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        float s = sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2.0;
        q.w = (m[1][2] - m[2][1]) / s;
        q.x = 0.25 * s;
        q.y = (m[0][1] + m[1][0]) / s;
        q.z = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        float s = sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2.0;
        q.w = (m[2][0] - m[0][2]) / s;
        q.x = (m[0][1] + m[1][0]) / s;
        q.y = 0.25 * s;
        q.z = (m[1][2] + m[2][1]) / s;
    } else {
        float s = sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2.0;
        q.w = (m[0][1] - m[1][0]) / s;
        q.x = (m[0][2] + m[2][0]) / s;
        q.y = (m[1][2] + m[2][1]) / s;
        q.z = 0.25 * s;
    }
    return normalize(q);
}

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= uint(verts.length() / 8u)) return;
    uint base = i * 8u;
    vec3 p0 = verts[base + 0].xyz;
    vec3 n0 = verts[base + 1].xyz;
    vec4 uvwt = verts[base + 2];
    ivec4 bi = ivec4(verts[base + 3]);
    vec4 w = verts[base + 4];
    vec3 sdefC = verts[base + 5].xyz;
    vec3 sdefR0 = verts[base + 6].xyz;
    vec3 sdefR1 = verts[base + 7].xyz;
    int wtype = int(uvwt.w + 0.5);

    int influence = wtype == 0 ? 1 : ((wtype == 1 || wtype == 3) ? 2 : 4);
    vec3 position = vec3(0.0);
    vec3 normal = vec3(0.0);
    bool resolved = false;

    // SDEF：中心/方向按权重混合后沿方向外推，半轴缩放还原体积。
    if (wtype == 3) {
        int b0 = bi.x;
        int b1 = bi.y;
        float w1 = clamp(w.y, 0.0, 1.0);
        float w0 = 1.0 - w1;
        vec3 c0 = (skin[b0] * vec4(sdefC, 1.0)).xyz;
        vec3 c1 = (skin[b1] * vec4(sdefC, 1.0)).xyz;
        vec3 pA = (skin[b0] * vec4(p0, 1.0)).xyz;
        vec3 pB = (skin[b1] * vec4(p0, 1.0)).xyz;
        vec3 blended = c0 * w0 + c1 * w1;
        vec3 direction = (pA - c0) * w0 + (pB - c1) * w1;
        float dir_len = length(direction);
        // PMX 里 R0/R1 存的是绝对坐标（中心 C + 半轴），需先减去 C 再取绝对值。
        vec3 r0v = abs(sdefR0 - sdefC);
        vec3 r1v = abs(sdefR1 - sdefC);
        bool has_r1 = abs(sdefR1.x) > 1e-6 || abs(sdefR1.y) > 1e-6 || abs(sdefR1.z) > 1e-6;
        vec3 axes = has_r1 ? r1v : r0v;
        float amax = max(max(axes.x, axes.y), axes.z);
        float radius = length(p0 - sdefC);
        if (dir_len > 1e-6) {
            mat3 R0 = mat3(skin[b0]);
            vec3 dl = transpose(R0) * direction;
            vec3 d_rest = p0 - sdefC;
            float len_rest = length(d_rest);
            float len_cur = length(dl);
            if (len_rest > 1e-6 && len_cur > 1e-6 && amax > 1e-6) {
                float sum_rest = 0.0;
                float sum_cur = 0.0;
                for (int k = 0; k < 3; ++k) {
                    float a = abs(axes[k]);
                    if (a < 1e-6) a = amax;
                    float tr = (d_rest[k] / len_rest) / a;
                    float tc = (dl[k] / len_cur) / a;
                    sum_rest += tr * tr;
                    sum_cur += tc * tc;
                }
                if (sum_rest > 1e-12 && sum_cur > 1e-12) {
                    radius = len_rest * sqrt(sum_rest / sum_cur);
                    radius = clamp(radius, len_rest * 0.6, len_rest * 1.4);
                }
            }
        }
        if (dir_len > 1e-6 && radius > 1e-6) {
            position = blended + direction * (radius / dir_len);
            resolved = true;
        }
    }

    // BDEF2/BDEF4：双四元数混合，避免大角度弯曲的糖纸效应。
    if (!resolved && (wtype == 1 || wtype == 2)) {
        vec4 qsum = vec4(0.0);
        vec4 dsum = vec4(0.0);
        vec4 reference = vec4(0.0, 0.0, 0.0, 1.0);
        bool has_ref = false;
        float wsum = 0.0;
        for (int k = 0; k < influence; ++k) {
            float wk = w[k];
            if (wk <= 0.0) continue;
            int b = bi[k];
            mat4 M = skin[b];
            vec4 q = quat_from_mat(mat3(M));
            // skin = W * T(-rest)，其平移列已是 t - R*rest，正是双四元数需要的平移。
            vec4 d = 0.5 * qmul(vec4(M[3].xyz, 0.0), q);
            float sgn = 1.0;
            if (!has_ref) {
                reference = q;
                has_ref = true;
            } else if (dot(q, reference) < 0.0) {
                sgn = -1.0;
            }
            qsum += q * wk * sgn;
            dsum += d * wk * sgn;
            wsum += wk;
        }
        float qlen = length(qsum);
        if (wsum > 1e-6 && qlen > 1e-6) {
            qsum /= qlen;
            dsum /= qlen;
            vec4 dconj = vec4(-qsum.xyz, qsum.w);
            vec3 translation = 2.0 * qmul(dsum, dconj).xyz;
            position = rotate_q(qsum, p0) + translation;
            resolved = true;
        }
    }

    for (int k = 0; k < influence; ++k) {
        int b = bi[k];
        if (!resolved) {
            // skin 已包含 T(-rest)，直接作用在静止坐标上。
            position += (skin[b] * vec4(p0, 1.0)).xyz * w[k];
        }
        normal += (mat3(skin[b]) * n0) * w[k];
    }

    float nlen = length(normal);
    outPos[i] = vec4(position, 1.0);
    outNrm[i] = vec4(nlen > 1e-6 ? normal / nlen : n0, 0.0);
}
)";

// 卡通着色管线的 GLSL 统一放在 gpu/toon.hpp，这里只做别名，
// 避免同一段着色器文本在渲染器里再维护一份、两边逐渐漂移。
const char* const kMeshVS = toon::vertex_source;
const char* const kMeshFS = toon::fragment_source;
const char* const kOutlineVS = toon::outline_vertex_source;
const char* const kOutlineFS = toon::outline_fragment_source;

const char* kSolidVS = R"(#version 460 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNrm;
layout(location = 2) in vec4 aColor;
uniform mat4 uMVP;
out vec3 vN;
out vec3 vPos;
out vec4 vC;
void main() {
    vN = aNrm;
    vPos = aPos;
    vC = aColor;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

const char* kSolidFS = R"(#version 460 core
in vec3 vN;
in vec3 vPos;
in vec4 vC;
out vec4 fragColor;
uniform vec3 uLightDir;
uniform vec3 uEye;
void main() {
    vec3 n = normalize(vN);
    if (!gl_FrontFacing) n = -n;
    vec3 light = normalize(uLightDir);
    float ndl = max(dot(n, light), 0.0);
    float hemi = 0.5 + 0.5 * n.y;
    float shade = 0.44 + 0.36 * ndl + 0.20 * hemi;
    vec3 view = normalize(uEye - vPos);
    float rim = pow(1.0 - abs(dot(n, view)), 2.5);
    vec3 col = vC.rgb * shade + vC.rgb * rim * 0.28;
    fragColor = vec4(clamp(col, 0.0, 1.0), vC.a);
}
)";

// 叠加层（骨骼 / 刚体 / 关节）的固定光照方向。辅助层与场景方向光解耦：
// 调整灯光只改变模型与地面的明暗，骨骼的明暗保持稳定，避免「光跟着骨骼走」。
const Vec3 kOverlayLightDir = normalize(Vec3{0.35f, 0.72f, 0.45f});

const char* kInstVS = R"(#version 460 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNrm;
layout(std430, binding = 5) readonly buffer Instances { vec4 inst[]; };
uniform mat4 uMVP;
uniform int uInstanceBase;
out vec3 vN;
out vec3 vPos;
out vec4 vC;
void main() {
    // mat4 占 4 个 vec4 + 1 个颜色；分桶绘制时用 uInstanceBase 指向本组起点。
    uint base = uint(gl_InstanceID + uInstanceBase) * 5u;
    mat4 M = mat4(inst[base + 0], inst[base + 1], inst[base + 2], inst[base + 3]);
    vC = inst[base + 4];
    vec4 world = M * vec4(aPos, 1.0);
    vN = mat3(M) * aNrm;
    vPos = world.xyz;
    gl_Position = uMVP * world;
}
)";

const char* kLineVS = R"(#version 460 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
uniform mat4 uMVP;
out vec4 vC;
void main() {
    vC = aColor;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

const char* kLineFS = R"(#version 460 core
in vec4 vC;
out vec4 fragColor;
uniform float uAlpha;
void main() { fragColor = vec4(vC.rgb, vC.a * uAlpha); }
)";

// ---------------------------------------------------------------- 几何模板
struct TemplateMesh {
    std::vector<float> verts;   // pos3 nrm3 color4
    std::vector<std::uint32_t> idx;
};

void winding_fix(std::vector<float>& verts, std::vector<std::uint32_t>& idx) {
    for (std::size_t t = 0; t + 2 < idx.size(); t += 3) {
        const std::uint32_t a = idx[t], b = idx[t + 1], c = idx[t + 2];
        Vec3 pa{verts[a * 10 + 0], verts[a * 10 + 1], verts[a * 10 + 2]};
        Vec3 pb{verts[b * 10 + 0], verts[b * 10 + 1], verts[b * 10 + 2]};
        Vec3 pc{verts[c * 10 + 0], verts[c * 10 + 1], verts[c * 10 + 2]};
        Vec3 naive{verts[a * 10 + 3], verts[a * 10 + 4], verts[a * 10 + 5]};
        const Vec3 face = cross(pb - pa, pc - pa);
        if (length(face) < 1e-10f) {
            idx[t] = idx[t + 1] = idx[t + 2] = 0;
            continue;
        }
        if (dot(face, naive) < 0.0f) std::swap(idx[t + 1], idx[t + 2]);
    }
}

TemplateMesh make_box(float hx, float hy, float hz) {
    TemplateMesh m;
    const Vec3 n[6] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
    const float quad[6][12] = {
        {-hx, -hy, hz, hx, -hy, hz, hx, hy, hz, -hx, hy, hz},
        {-hx, -hy, -hz, -hx, hy, -hz, hx, hy, -hz, hx, -hy, -hz},
        {hx, -hy, -hz, hx, hy, -hz, hx, hy, hz, hx, -hy, hz},
        {-hx, -hy, -hz, -hx, -hy, hz, -hx, hy, hz, -hx, hy, -hz},
        {-hx, hy, -hz, -hx, hy, hz, hx, hy, hz, hx, hy, -hz},
        {-hx, -hy, -hz, hx, -hy, -hz, hx, -hy, hz, -hx, -hy, hz},
    };
    for (int f = 0; f < 6; ++f) {
        const auto base = static_cast<std::uint32_t>(m.verts.size() / 10);
        for (int v = 0; v < 4; ++v) {
            m.verts.push_back(quad[f][v * 3 + 0]);
            m.verts.push_back(quad[f][v * 3 + 1]);
            m.verts.push_back(quad[f][v * 3 + 2]);
            m.verts.push_back(n[f].x);
            m.verts.push_back(n[f].y);
            m.verts.push_back(n[f].z);
            m.verts.push_back(1.0f);
            m.verts.push_back(1.0f);
            m.verts.push_back(1.0f);
            m.verts.push_back(1.0f);
        }
        m.idx.insert(m.idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    winding_fix(m.verts, m.idx);
    return m;
}

TemplateMesh make_sphere(float radius, int segments, int rings) {
    TemplateMesh m;
    for (int iy = 0; iy <= rings; ++iy) {
        const float theta = kPi * static_cast<float>(iy) / static_cast<float>(rings);
        const float y = std::cos(theta) * radius;
        const float rr = std::sin(theta) * radius;
        for (int ix = 0; ix < segments; ++ix) {
            const float phi = 2.0f * kPi * static_cast<float>(ix) / static_cast<float>(segments);
            const float px = rr * std::cos(phi);
            const float pz = rr * std::sin(phi);
            m.verts.insert(m.verts.end(), {px, y, pz,
                                           radius > 1e-9f ? px / radius : 0.0f,
                                           radius > 1e-9f ? y / radius : 1.0f,
                                           radius > 1e-9f ? pz / radius : 0.0f,
                                           1.0f, 1.0f, 1.0f, 1.0f});
        }
    }
    for (int iy = 0; iy < rings; ++iy) {
        for (int ix = 0; ix < segments; ++ix) {
            const std::uint32_t a = static_cast<std::uint32_t>(iy * segments + ix);
            const std::uint32_t b = static_cast<std::uint32_t>(iy * segments + (ix + 1) % segments);
            const std::uint32_t c = static_cast<std::uint32_t>((iy + 1) * segments + ix);
            const std::uint32_t d = static_cast<std::uint32_t>((iy + 1) * segments + (ix + 1) % segments);
            m.idx.insert(m.idx.end(), {a, c, b, b, c, d});
        }
    }
    winding_fix(m.verts, m.idx);
    return m;
}

TemplateMesh make_cylinder(float radius, float height, int segments) {
    // 单位圆柱（半径 1，高度 1，居中）：胶囊的柱段。两端由球模板封住，不封盖。
    TemplateMesh m;
    const float half = std::fabs(height) * 0.5f;
    for (int iy = 0; iy < 2; ++iy) {
        const float y = iy == 0 ? -half : half;
        for (int ix = 0; ix < segments; ++ix) {
            const float phi = 2.0f * kPi * static_cast<float>(ix) / static_cast<float>(segments);
            const float nx = std::cos(phi), nz = std::sin(phi);
            m.verts.insert(m.verts.end(), {radius * nx, y, radius * nz, nx, 0.0f, nz,
                                           1.0f, 1.0f, 1.0f, 1.0f});
        }
    }
    for (int ix = 0; ix < segments; ++ix) {
        const std::uint32_t a = static_cast<std::uint32_t>(ix);
        const std::uint32_t b = static_cast<std::uint32_t>((ix + 1) % segments);
        const std::uint32_t c = static_cast<std::uint32_t>(segments + ix);
        const std::uint32_t d = static_cast<std::uint32_t>(segments + (ix + 1) % segments);
        m.idx.insert(m.idx.end(), {a, c, b, b, c, d});
    }
    winding_fix(m.verts, m.idx);
    return m;
}

void upload_template(const TemplateMesh& src, GLuint* vao, GLuint* vbo, GLuint* ebo,
                     GLuint* index_count) {
    glGenVertexArrays(1, vao);
    glBindVertexArray(*vao);
    glGenBuffers(1, vbo);
    glBindBuffer(glc::GL_ARRAY_BUFFER, *vbo);
    glBufferData(glc::GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(src.verts.size() * sizeof(float)), src.verts.data(),
                 glc::GL_STATIC_DRAW);
    const GLsizei stride = 10 * sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, glc::GL_FLOAT, glc::GL_FALSE, stride, nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, glc::GL_FLOAT, glc::GL_FALSE, stride,
                          reinterpret_cast<const void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, glc::GL_FLOAT, glc::GL_FALSE, stride,
                          reinterpret_cast<const void*>(6 * sizeof(float)));
    glGenBuffers(1, ebo);
    glBindBuffer(glc::GL_ELEMENT_ARRAY_BUFFER, *ebo);
    glBufferData(glc::GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(src.idx.size() * sizeof(std::uint32_t)), src.idx.data(),
                 glc::GL_STATIC_DRAW);
    *index_count = static_cast<GLuint>(src.idx.size());
    glBindVertexArray(0);
}

Mat4 project_view_proj(const Mat4& view, const Mat4& proj) { return mul(proj, view); }

bool project_to_screen(const Mat4& vp, Vec3 p, int width, int height, float& sx, float& sy) {
    const float x = vp.at(0, 0) * p.x + vp.at(0, 1) * p.y + vp.at(0, 2) * p.z + vp.at(0, 3);
    const float y = vp.at(1, 0) * p.x + vp.at(1, 1) * p.y + vp.at(1, 2) * p.z + vp.at(1, 3);
    const float w = vp.at(3, 0) * p.x + vp.at(3, 1) * p.y + vp.at(3, 2) * p.z + vp.at(3, 3);
    if (w <= 1e-6f) return false;
    sx = (x / w * 0.5f + 0.5f) * static_cast<float>(width);
    sy = (1.0f - (y / w * 0.5f + 0.5f)) * static_cast<float>(height);
    return true;
}

float point_segment_distance(float px, float py, float ax, float ay, float bx, float by) {
    const float dx = bx - ax, dy = by - ay;
    const float d2 = dx * dx + dy * dy;
    if (d2 <= 1e-9f) return std::hypot(px - ax, py - ay);
    float t = ((px - ax) * dx + (py - ay) * dy) / d2;
    t = clampf(t, 0.0f, 1.0f);
    return std::hypot(px - (ax + dx * t), py - (ay + dy * t));
}

// 光源视图矩阵：与相机相同的右手 look-at 约定，供阴影 pass 使用。
Mat4 light_look_at(Vec3 eye, Vec3 center, Vec3 up) {
    const Vec3 f = normalize(center - eye);
    Vec3 s = cross(f, up);
    if (length(s) < 1e-6f) s = cross(f, Vec3{0.0f, 0.0f, 1.0f});
    s = normalize(s);
    const Vec3 u = cross(s, f);
    Mat4 m;
    m.at(0, 0) = s.x; m.at(0, 1) = s.y; m.at(0, 2) = s.z; m.at(0, 3) = -dot(s, eye);
    m.at(1, 0) = u.x; m.at(1, 1) = u.y; m.at(1, 2) = u.z; m.at(1, 3) = -dot(u, eye);
    m.at(2, 0) = -f.x; m.at(2, 1) = -f.y; m.at(2, 2) = -f.z; m.at(2, 3) = dot(f, eye);
    m.at(3, 3) = 1.0f;
    return m;
}

}  // namespace

SceneRenderer::~SceneRenderer() {
    release_model();
    // 模板/网格/实例缓冲只在 init 时创建一次，随渲染器一起销毁。
    auto drop = [](OverlayMesh& m) {
        if (m.vbo) glDeleteBuffers(1, &m.vbo);
        if (m.ebo) glDeleteBuffers(1, &m.ebo);
        if (m.vao) glDeleteVertexArrays(1, &m.vao);
        m = OverlayMesh{};
    };
    drop(rigid_box_);
    drop(rigid_sphere_);
    drop(rigid_cylinder_);
    drop(joint_sphere_);
    drop(bone_mesh_);
    drop(camera_mesh_);
    drop(light_mesh_);
    if (overlay_instance_ssbo_) {
        glDeleteBuffers(1, &overlay_instance_ssbo_);
        overlay_instance_ssbo_ = 0;
    }
    if (grid_vbo_) {
        glDeleteBuffers(1, &grid_vbo_);
        grid_vbo_ = 0;
    }
    if (grid_vao_) {
        glDeleteVertexArrays(1, &grid_vao_);
        grid_vao_ = 0;
    }
    for (GLuint& ramp : toon_ramps_) {
        if (ramp) glDeleteTextures(1, &ramp);
        ramp = 0;
    }
    if (ground_vbo_) {
        glDeleteBuffers(1, &ground_vbo_);
        ground_vbo_ = 0;
    }
    if (ground_vao_) {
        glDeleteVertexArrays(1, &ground_vao_);
        ground_vao_ = 0;
    }
    if (shadow_fbo_) {
        glDeleteFramebuffers(1, &shadow_fbo_);
        shadow_fbo_ = 0;
    }
    if (shadow_tex_) {
        glDeleteTextures(1, &shadow_tex_);
        shadow_tex_ = 0;
    }
}

bool SceneRenderer::init(std::string* error) {
    std::string err;
    if (!skin_program_.build_compute(kSkinCS, &err)) {
        if (error) *error = err;
        return false;
    }
    if (!mesh_program_.build(kMeshVS, kMeshFS, &err) ||
        !outline_program_.build(kOutlineVS, kOutlineFS, &err) ||
        !solid_program_.build(kSolidVS, kSolidFS, &err) ||
        !inst_program_.build(kInstVS, kSolidFS, &err) ||
        !line_program_.build(kLineVS, kLineFS, &err) ||
        !shadow_program_.build(kMeshVS, toon::shadow_fragment_source, &err) ||
        !ground_program_.build(toon::ground_vertex_source, toon::ground_fragment_source, &err)) {
        if (error) *error = err;
        return false;
    }
    if (!build_shadow_map(&err)) {
        if (error) *error = err;
        return false;
    }
    glGenBuffers(1, &overlay_instance_ssbo_);
    gpu_check_enabled_ = std::getenv("PMX_GPU_CHECK") != nullptr;
    // 肉体 GPU 求解是可选加速：编译失败只回退 CPU，不影响渲染器启动。
    std::string flesh_err;
    if (!flesh_gpu_.init(&flesh_err)) std::fprintf(stderr, "[flesh-gpu] %s\n", flesh_err.c_str());
    build_templates();
    build_toon_ramps();
    build_grid(RenderSettings{});
    build_ground();
    ready_ = true;
    return true;
}

void SceneRenderer::build_templates() {
    const TemplateMesh box = make_box(1.0f, 1.0f, 1.0f);
    upload_template(box, &rigid_box_.vao, &rigid_box_.vbo, &rigid_box_.ebo, &rigid_box_.index_count);
    const TemplateMesh sphere = make_sphere(1.0f, 18, 12);
    upload_template(sphere, &rigid_sphere_.vao, &rigid_sphere_.vbo, &rigid_sphere_.ebo,
                    &rigid_sphere_.index_count);
    upload_template(sphere, &joint_sphere_.vao, &joint_sphere_.vbo, &joint_sphere_.ebo,
                    &joint_sphere_.index_count);
    const TemplateMesh capsule = make_cylinder(1.0f, 1.0f, 18);
    upload_template(capsule, &rigid_cylinder_.vao, &rigid_cylinder_.vbo, &rigid_cylinder_.ebo,
                    &rigid_cylinder_.index_count);

    glGenVertexArrays(1, &bone_mesh_.vao);
    glBindVertexArray(bone_mesh_.vao);
    glGenBuffers(1, &bone_mesh_.vbo);
    glBindBuffer(glc::GL_ARRAY_BUFFER, bone_mesh_.vbo);
    glBufferData(glc::GL_ARRAY_BUFFER, 0, nullptr, glc::GL_STREAM_DRAW);
    const GLsizei stride = 10 * sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, glc::GL_FLOAT, glc::GL_FALSE, stride, nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, glc::GL_FLOAT, glc::GL_FALSE, stride,
                          reinterpret_cast<const void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, glc::GL_FLOAT, glc::GL_FALSE, stride,
                          reinterpret_cast<const void*>(6 * sizeof(float)));
    glBindVertexArray(0);

    // 相机线框：与网格线相同的 pos3 + color4 布局，每帧重传。
    glGenVertexArrays(1, &camera_mesh_.vao);
    glBindVertexArray(camera_mesh_.vao);
    glGenBuffers(1, &camera_mesh_.vbo);
    glBindBuffer(glc::GL_ARRAY_BUFFER, camera_mesh_.vbo);
    glBufferData(glc::GL_ARRAY_BUFFER, 0, nullptr, glc::GL_STREAM_DRAW);
    const GLsizei line_stride = 7 * sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, glc::GL_FLOAT, glc::GL_FALSE, line_stride, nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, glc::GL_FLOAT, glc::GL_FALSE, line_stride,
                          reinterpret_cast<const void*>(3 * sizeof(float)));
    glBindVertexArray(0);

    // 方向光标记：与相机线框同一套 pos3 + color4 布局，每帧重传。
    glGenVertexArrays(1, &light_mesh_.vao);
    glBindVertexArray(light_mesh_.vao);
    glGenBuffers(1, &light_mesh_.vbo);
    glBindBuffer(glc::GL_ARRAY_BUFFER, light_mesh_.vbo);
    glBufferData(glc::GL_ARRAY_BUFFER, 0, nullptr, glc::GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, glc::GL_FLOAT, glc::GL_FALSE, line_stride, nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, glc::GL_FLOAT, glc::GL_FALSE, line_stride,
                          reinterpret_cast<const void*>(3 * sizeof(float)));
    glBindVertexArray(0);
}

void SceneRenderer::build_grid(const RenderSettings& settings) {
    if (grid_vbo_) {
        glDeleteBuffers(1, &grid_vbo_);
        grid_vbo_ = 0;
    }
    if (grid_vao_) {
        glDeleteVertexArrays(1, &grid_vao_);
        grid_vao_ = 0;
    }
    std::vector<float> verts;
    const float extent = 40.0f;
    const float step = 1.0f;
    const Vec4 minor(settings.grid_minor.x, settings.grid_minor.y, settings.grid_minor.z, 1.0f);
    const Vec4 major(settings.grid_major.x, settings.grid_major.y, settings.grid_major.z, 1.0f);
    const Vec4 axis_x(settings.grid_axis_x.x, settings.grid_axis_x.y, settings.grid_axis_x.z,
                      1.0f);
    const Vec4 axis_z(settings.grid_axis_z.x, settings.grid_axis_z.y, settings.grid_axis_z.z,
                      1.0f);
    auto push = [&verts](Vec3 p, Vec4 c) {
        verts.insert(verts.end(), {p.x, p.y, p.z, c.x, c.y, c.z, c.w});
    };
    for (float x = -extent; x <= extent + 1e-4f; x += step) {
        const bool axis = std::fabs(x) < 1e-4f;
        const Vec4 c = (std::fabs(std::fmod(x, 5.0f)) < 1e-4f) ? major : minor;
        push({x, 0.0f, -extent}, axis ? axis_z : c);
        push({x, 0.0f, extent}, axis ? axis_z : c);
    }
    for (float z = -extent; z <= extent + 1e-4f; z += step) {
        const bool axis = std::fabs(z) < 1e-4f;
        const Vec4 c = (std::fabs(std::fmod(z, 5.0f)) < 1e-4f) ? major : minor;
        push({-extent, 0.0f, z}, axis ? axis_x : c);
        push({extent, 0.0f, z}, axis ? axis_x : c);
    }
    grid_minor_ = settings.grid_minor;
    grid_major_ = settings.grid_major;
    grid_axis_x_ = settings.grid_axis_x;
    grid_axis_z_ = settings.grid_axis_z;
    grid_vertex_count_ = static_cast<GLuint>(verts.size() / 7);
    glGenVertexArrays(1, &grid_vao_);
    glBindVertexArray(grid_vao_);
    glGenBuffers(1, &grid_vbo_);
    glBindBuffer(glc::GL_ARRAY_BUFFER, grid_vbo_);
    glBufferData(glc::GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), glc::GL_STATIC_DRAW);
    const GLsizei stride = 7 * sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, glc::GL_FLOAT, glc::GL_FALSE, stride, nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, glc::GL_FLOAT, glc::GL_FALSE, stride,
                          reinterpret_cast<const void*>(3 * sizeof(float)));
    glBindVertexArray(0);
}

void SceneRenderer::ensure_grid(const RenderSettings& settings) {
    if (grid_minor_.x != settings.grid_minor.x || grid_minor_.y != settings.grid_minor.y ||
        grid_minor_.z != settings.grid_minor.z || grid_major_.x != settings.grid_major.x ||
        grid_major_.y != settings.grid_major.y || grid_major_.z != settings.grid_major.z ||
        grid_axis_x_.x != settings.grid_axis_x.x || grid_axis_x_.y != settings.grid_axis_x.y ||
        grid_axis_x_.z != settings.grid_axis_x.z || grid_axis_z_.x != settings.grid_axis_z.x ||
        grid_axis_z_.y != settings.grid_axis_z.y || grid_axis_z_.z != settings.grid_axis_z.z) {
        build_grid(settings);
    }
}

Vec3 effective_background(const RenderSettings& settings) {
    const float b = clampf(settings.bg_brightness, 0.0f, 3.0f);
    auto channel = [b](float c) {
        if (b >= 1.0f) {
            // 提亮走「向白插值」，避免直接乘导致高光溢出成纯白后丢失层次。
            const float k = std::min(1.0f, (b - 1.0f) * 0.6f);
            c += (1.0f - c) * k;
        } else {
            c *= b;
        }
        return clampf(c, 0.0f, 1.0f);
    };
    return {channel(settings.background.x), channel(settings.background.y),
            channel(settings.background.z)};
}

void SceneRenderer::build_camera_gizmo(const RenderSettings& settings, Vec3 view_eye) {
    camera_verts_.clear();
    camera_vertex_count_ = 0;
    Vec3 forward = settings.camera_target - settings.camera_eye;
    if (length(forward) < 1e-4f) return;
    forward = normalize(forward);
    Vec3 up{0.0f, 1.0f, 0.0f};
    if (std::fabs(dot(forward, up)) > 0.999f) up = Vec3(0.0f, 0.0f, 1.0f);
    const Vec3 right = normalize(cross(forward, up));
    const Vec3 upv = cross(right, forward);

    // 尺寸跟到视点的距离走，屏幕上大致恒定；远处相机也不会缩成一个点。
    const float view_dist = std::max(length(settings.camera_eye - view_eye), 0.25f);
    const float s = clampf(view_dist * 0.085f, 0.12f, 6.0f);

    const Vec4 body_color(0.96f, 0.64f, 0.24f, 1.0f);
    const Vec4 cone_color(0.98f, 0.78f, 0.42f, 0.85f);
    const Vec4 frame_color(1.0f, 0.86f, 0.52f, 1.0f);

    auto push = [this](Vec3 p, Vec4 c) {
        camera_verts_.insert(camera_verts_.end(), {p.x, p.y, p.z, c.x, c.y, c.z, c.w});
    };
    auto line = [&push](Vec3 a, Vec3 b, Vec4 c) {
        push(a, c);
        push(b, c);
    };

    const Vec3 eye = settings.camera_eye;
    // 机身：eye 为中心的小盒子，+f 为镜头方向。
    const float hw = s * 0.20f;
    const float hh = s * 0.16f;
    const float front = s * 0.28f;
    const float back = -s * 0.26f;
    Vec3 corners[8];
    int at = 0;
    for (int side = 0; side < 2; ++side) {
        const float z = side == 0 ? back : front;
        for (int j = 0; j < 4; ++j) {
            const float sx = (j == 1 || j == 2) ? hw : -hw;
            const float sy = (j < 2) ? hh : -hh;
            corners[at++] = eye + right * sx + upv * sy + forward * z;
        }
    }
    const int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                              {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& e : edges) line(corners[e[0]], corners[e[1]], body_color);

    // 镜头：前脸四角收拢到前方一点。
    const Vec3 lens = eye + forward * (s * 0.55f);
    for (int j = 4; j < 8; ++j) line(corners[j], lens, body_color);

    // 顶部方向标记：一个小三角，指示相机 up。
    const Vec3 top = eye + upv * hh + forward * (s * 0.01f);
    const Vec3 mark_l = top - right * (s * 0.09f);
    const Vec3 mark_r = top + right * (s * 0.09f);
    const Vec3 mark_t = top + upv * (s * 0.26f);
    line(mark_l, mark_r, body_color);
    line(mark_l, mark_t, body_color);
    line(mark_r, mark_t, body_color);

    // 取景锥：从镜头到远平面矩形，宽高比与导出画面一致。
    const float far_d = s * 3.0f;
    const float aspect = std::max(settings.camera_aspect, 0.05f);
    // 正交机位画成方盒取景（MMD 关闭「透视」），透视机位画成锥台。
    const float fh = settings.camera_ortho
                         ? std::max(settings.camera_ortho_scale, 0.05f) * 0.5f
                         : far_d * std::tan(settings.camera_fov * kPi / 360.0f);
    const float fw = fh * aspect;
    const Vec3 center = eye + forward * (s * 0.55f + far_d);
    const Vec3 q[4] = {center - right * fw + upv * fh, center + right * fw + upv * fh,
                       center + right * fw - upv * fh, center - right * fw - upv * fh};
    for (int j = 0; j < 4; ++j) {
        line(lens, q[j], cone_color);
        line(q[j], q[(j + 1) % 4], frame_color);
    }

    // 注视点十字，标出相机对准的位置。
    const Vec3 target = settings.camera_target;
    const float t = s * 0.12f;
    line(target - right * t, target + right * t, frame_color);
    line(target - upv * t, target + upv * t, frame_color);
    line(target - forward * t, target + forward * t, frame_color);

    camera_vertex_count_ = static_cast<GLuint>(camera_verts_.size() / 7);
    glBindBuffer(glc::GL_ARRAY_BUFFER, camera_mesh_.vbo);
    glBufferData(glc::GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(camera_verts_.size() * sizeof(float)),
                 camera_verts_.data(), glc::GL_STREAM_DRAW);
    glBindBuffer(glc::GL_ARRAY_BUFFER, 0);
}

void SceneRenderer::build_light_gizmo(const RenderSettings& settings, float aspect,
                                      const Mat4& view) {
    light_verts_.clear();
    light_vertex_count_ = 0;
    Vec3 dir = settings.light_dir;
    const float dlen = length(dir);
    // 方向全零时退回正上方，避免 normalize 出 NaN。
    dir = dlen > 1e-4f ? dir * (1.0f / dlen) : Vec3{0.0f, 1.0f, 0.0f};

    // 视口右上角的光照罗盘：直接画在 NDC 里（z=0、MVP 用单位阵），
    // 不受模型尺度/取景影响，任何相机距离下都完整可见。
    // 圆用椭圆参数绘制（x 半径按宽高比压缩），屏幕上看起来仍是正圆。
    const float ry = 0.072f;
    const float rx = ry / std::max(aspect, 0.2f);
    const float cx = 0.86f - rx * 1.9f;
    const float cy = 0.86f - ry * 1.9f;

    // 光照色参与罗盘颜色，强度越低越暗，能直接看出当前灯光的色温与亮度。
    // 背景偏亮时改用深色描线，否则白底上的白色罗盘几乎不可见。
    const Vec3 bg = effective_background(settings);
    const float bg_lum = 0.2126f * bg.x + 0.7152f * bg.y + 0.0722f * bg.z;
    const float ink = bg_lum > 0.5f ? 0.22f : 1.0f;
    const Vec3 tint = settings.light_color;
    const float bright = clampf(settings.light_intensity, 0.0f, 2.0f) * 0.5f + 0.5f;
    auto tone = [&tint, bright, ink](float base, float alpha) {
        return Vec4{clampf(tint.x * base * bright * ink, 0.0f, 1.0f),
                    clampf(tint.y * base * bright * ink, 0.0f, 1.0f),
                    clampf(tint.z * base * bright * ink, 0.0f, 1.0f), alpha};
    };
    const Vec4 sun_color = tone(1.0f, 0.95f);
    const Vec4 ray_color = tone(0.82f, 0.75f);
    const Vec4 arrow_color{0.95f * ink, 0.92f * ink, 0.82f * ink, 0.85f};

    auto push = [this, cx, cy](float px, float py, Vec4 c) {
        light_verts_.insert(light_verts_.end(), {cx + px, cy + py, 0.0f, c.x, c.y, c.z, c.w});
    };
    auto line = [&push](float x0, float y0, float x1, float y1, Vec4 c) {
        push(x0, y0, c);
        push(x1, y1, c);
    };
    // 以“圆盘单位”换算到 NDC：t 为角度，r 为相对半径。
    auto point = [rx, ry](float t, float r) {
        return Vec2{std::cos(t) * rx * r, std::sin(t) * ry * r};
    };

    // 圆盘 + 8 道短射线。
    constexpr int kSegments = 32;
    for (int i = 0; i < kSegments; ++i) {
        const float t0 = 2.0f * kPi * static_cast<float>(i) / kSegments;
        const float t1 = 2.0f * kPi * static_cast<float>(i + 1) / kSegments;
        const Vec2 p0 = point(t0, 1.0f);
        const Vec2 p1 = point(t1, 1.0f);
        line(p0.x, p0.y, p1.x, p1.y, sun_color);
    }
    for (int i = 0; i < 8; ++i) {
        const float t = 2.0f * kPi * static_cast<float>(i) / 8.0f;
        const Vec2 p0 = point(t, 1.30f);
        const Vec2 p1 = point(t, 1.85f);
        line(p0.x, p0.y, p1.x, p1.y, ray_color);
    }

    // 光源在屏幕上的方向：把光向量投到相机空间，取 x/y 分量。
    const float vx = view.m[0] * dir.x + view.m[4] * dir.y + view.m[8] * dir.z;
    const float vy = view.m[1] * dir.x + view.m[5] * dir.y + view.m[9] * dir.z;
    const float planar = std::sqrt(vx * vx + vy * vy);
    if (planar < 0.08f) {
        // 光沿视线方向（顺光/逆光）：投影退化成一个点，画个小圆点表示朝向观察者。
        constexpr int kDot = 12;
        for (int i = 0; i < kDot; ++i) {
            const float t0 = 2.0f * kPi * static_cast<float>(i) / kDot;
            const float t1 = 2.0f * kPi * static_cast<float>(i + 1) / kDot;
            const Vec2 p0 = point(t0, 0.18f);
            const Vec2 p1 = point(t1, 0.18f);
            line(p0.x, p0.y, p1.x, p1.y, arrow_color);
        }
    } else {
        // 箭头指向光源来向（与 MMD 照明面板的 X/Y/Z 小球一致）。
        const Vec2 tip{vx / planar * rx * 1.55f, vy / planar * ry * 1.55f};
        const float tl = std::max(std::sqrt(tip.x * tip.x + tip.y * tip.y), 1e-5f);
        const float nx = -tip.y / tl;
        const float ny = tip.x / tl;
        const float hw = std::min(rx, ry) * 0.45f;
        const Vec2 base{tip.x * 0.45f, tip.y * 0.45f};
        line(0.0f, 0.0f, tip.x, tip.y, arrow_color);
        line(tip.x, tip.y, base.x + nx * hw, base.y + ny * hw, arrow_color);
        line(tip.x, tip.y, base.x - nx * hw, base.y - ny * hw, arrow_color);
    }

    light_vertex_count_ = static_cast<GLuint>(light_verts_.size() / 7);
    glBindBuffer(glc::GL_ARRAY_BUFFER, light_mesh_.vbo);
    glBufferData(glc::GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(light_verts_.size() * sizeof(float)),
                 light_verts_.data(), glc::GL_STREAM_DRAW);
    glBindBuffer(glc::GL_ARRAY_BUFFER, 0);
}

void SceneRenderer::build_toon_ramps() {
    // 内置 toon01..10 的程序化近似：竖直灰度渐变，底部暗、顶部亮。
    // 曲线本身在 gpu/toon.hpp 的 toon_ramp_value()，这里只负责上传成贴图。
    const int height = toon::toon_ramp_height;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(height) * 4);
    for (int i = 0; i < toon::toon_ramp_count; ++i) {
        for (int y = 0; y < height; ++y) {
            const float v = static_cast<float>(y) / static_cast<float>(height - 1);
            const auto c = static_cast<unsigned char>(toon::toon_ramp_value(i, v) * 255.0f + 0.5f);
            const std::size_t o = static_cast<std::size_t>(y) * 4;
            pixels[o + 0] = c;
            pixels[o + 1] = c;
            pixels[o + 2] = c;
            pixels[o + 3] = 255;
        }
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(glc::GL_TEXTURE_2D, tex);
        glPixelStorei(glc::GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(glc::GL_TEXTURE_2D, 0, glc::GL_RGBA8, 1, height, 0, glc::GL_RGBA,
                     glc::GL_UNSIGNED_BYTE, pixels.data());
        glTexParameteri(glc::GL_TEXTURE_2D, glc::GL_TEXTURE_MIN_FILTER,
                        static_cast<GLint>(glc::GL_LINEAR));
        glTexParameteri(glc::GL_TEXTURE_2D, glc::GL_TEXTURE_MAG_FILTER,
                        static_cast<GLint>(glc::GL_LINEAR));
        glTexParameteri(glc::GL_TEXTURE_2D, glc::GL_TEXTURE_WRAP_S,
                        static_cast<GLint>(glc::GL_CLAMP_TO_EDGE));
        glTexParameteri(glc::GL_TEXTURE_2D, glc::GL_TEXTURE_WRAP_T,
                        static_cast<GLint>(glc::GL_CLAMP_TO_EDGE));
        toon_ramps_[i] = tex;
    }
    glPixelStorei(glc::GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(glc::GL_TEXTURE_2D, 0);
}

bool SceneRenderer::build_shadow_map(std::string* error) {
    glGenTextures(1, &shadow_tex_);
    glBindTexture(glc::GL_TEXTURE_2D, shadow_tex_);
    glTexImage2D(glc::GL_TEXTURE_2D, 0, glc::GL_DEPTH_COMPONENT24, shadow_size_,
                 shadow_size_, 0, glc::GL_DEPTH_COMPONENT, glc::GL_FLOAT, nullptr);
    // 线性过滤让 sampler2DShadow 自带 2×2 双线性比较，配合 5×5 PCF 边缘更平滑。
    glTexParameteri(glc::GL_TEXTURE_2D, glc::GL_TEXTURE_MIN_FILTER,
                    static_cast<GLint>(glc::GL_LINEAR));
    glTexParameteri(glc::GL_TEXTURE_2D, glc::GL_TEXTURE_MAG_FILTER,
                    static_cast<GLint>(glc::GL_LINEAR));
    glTexParameteri(glc::GL_TEXTURE_2D, glc::GL_TEXTURE_WRAP_S,
                    static_cast<GLint>(glc::GL_CLAMP_TO_EDGE));
    glTexParameteri(glc::GL_TEXTURE_2D, glc::GL_TEXTURE_WRAP_T,
                    static_cast<GLint>(glc::GL_CLAMP_TO_EDGE));
    glBindTexture(glc::GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &shadow_fbo_);
    glBindFramebuffer(glc::GL_FRAMEBUFFER, shadow_fbo_);
    glFramebufferTexture2D(glc::GL_FRAMEBUFFER, glc::GL_DEPTH_ATTACHMENT,
                           glc::GL_TEXTURE_2D, shadow_tex_, 0);
    // 只有深度附件：显式关闭颜色读写，保证 core profile 下 FBO 完整。
    glDrawBuffer(glc::GL_NONE);
    glReadBuffer(glc::GL_NONE);
    const GLenum status = glCheckFramebufferStatus(glc::GL_FRAMEBUFFER);
    glBindFramebuffer(glc::GL_FRAMEBUFFER, 0);
    if (status != glc::GL_FRAMEBUFFER_COMPLETE) {
        if (error) *error = "阴影贴图 FBO 创建失败";
        return false;
    }
    shadow_texel_ = 1.0f / static_cast<float>(shadow_size_);
    return true;
}

void SceneRenderer::build_ground() {
    // 接收阴影的平面：不写深度，未遮挡处 discard，因此不影响现有画面。
    // 面积远大于网格，模型偏离原点时投影仍能落在平面上。
    const float e = 200.0f;
    const float verts[] = {-e, 0.0f, -e, e, 0.0f, -e, -e, 0.0f, e, e, 0.0f, e};
    glGenVertexArrays(1, &ground_vao_);
    glBindVertexArray(ground_vao_);
    glGenBuffers(1, &ground_vbo_);
    glBindBuffer(glc::GL_ARRAY_BUFFER, ground_vbo_);
    glBufferData(glc::GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof verts), verts,
                 glc::GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, glc::GL_FLOAT, glc::GL_FALSE, 3 * sizeof(float), nullptr);
    glBindVertexArray(0);
}

void SceneRenderer::update_light_matrix(const RenderSettings& settings) {
    Vec3 dir = settings.light_dir;
    if (length(dir) < 1e-6f) dir = Vec3{0.35f, 0.85f, 0.45f};
    dir = normalize(dir);
    // 正交框必须同时覆盖模型和它落到地面的投影：只按模型包围球取景时，低角度
    // 光的长影子会有一部分超出贴图范围，表现为“某个方向完全没有影子”。
    // 这里把包围盒 8 个角连同它们沿光线到 y=0 的落点一起变换到光源空间取景。
    const Vec3 fwd = -dir;  // 光线传播方向（光源 → 场景）
    const Vec3 hint = std::fabs(dir.y) > 0.99f ? Vec3{0.0f, 0.0f, 1.0f}
                                               : Vec3{0.0f, 1.0f, 0.0f};
    const Vec3 right = normalize(cross(fwd, hint));
    const Vec3 up = normalize(cross(right, fwd));

    // 光线接近水平时地面投影会拉得非常长，限制在模型尺度的几倍内。
    const float max_len = scene_radius_ * 6.0f + 1.0f;
    float min_x = 1e30f, max_x = -1e30f;
    float min_y = 1e30f, max_y = -1e30f;
    float min_z = 1e30f, max_z = -1e30f;
    auto add_point = [&](const Vec3& p) {
        const float x = dot(p, right);
        const float y = dot(p, up);
        const float z = dot(p, fwd);
        min_x = std::min(min_x, x);
        max_x = std::max(max_x, x);
        min_y = std::min(min_y, y);
        max_y = std::max(max_y, y);
        min_z = std::min(min_z, z);
        max_z = std::max(max_z, z);
    };
    for (int i = 0; i < 8; ++i) {
        const Vec3 p{(i & 1) ? scene_max_.x : scene_min_.x,
                     (i & 2) ? scene_max_.y : scene_min_.y,
                     (i & 4) ? scene_max_.z : scene_min_.z};
        add_point(p);
        // 沿光线落到 y=0：p + fwd * t，t = p.y / dir.y。
        if (p.y > 0.0f && dir.y > 0.05f) {
            const float t = std::min(p.y / dir.y, max_len);
            add_point(p + fwd * t);
        }
    }

    const Vec3 center = right * (0.5f * (min_x + max_x)) + up * (0.5f * (min_y + max_y)) +
                        fwd * (0.5f * (min_z + max_z));
    const float half = std::max(0.5f, 0.5f * std::max(max_x - min_x, max_y - min_y) * 1.05f);
    const float depth = std::max(0.5f, max_z - min_z);
    const Vec3 eye = center + dir * (depth * 0.5f + half + 1.0f);
    const Mat4 view = light_look_at(eye, center, up);
    const Mat4 proj =
        orthographic(-half, half, -half, half, 0.05f, depth + half * 2.0f + 2.0f);
    light_view_proj_ = mul(proj, view);
    // 一个阴影纹素在世界空间的大小；片元着色器用它做法线偏移（约 2 个纹素）。
    shadow_bias_ = 2.0f * half / static_cast<float>(shadow_size_) * 2.0f;
}

void SceneRenderer::draw_shadow_map(const Model& model, const RenderSettings& settings) {
    glBindFramebuffer(glc::GL_FRAMEBUFFER, shadow_fbo_);
    glViewport(0, 0, shadow_size_, shadow_size_);
    glClear(glc::GL_DEPTH_BUFFER_BIT);
    glEnable(glc::GL_DEPTH_TEST);
    glDepthFunc(glc::GL_LESS);
    glDepthMask(glc::GL_TRUE);
    glDisable(glc::GL_BLEND);
    // 双面投影：MMD 模型大量使用单面材质（裙摆、衣领），开背面剔除会漏光。
    glDisable(glc::GL_CULL_FACE);
    glPolygonMode(glc::GL_FRONT_AND_BACK, glc::GL_FILL);

    shadow_program_.use();
    shadow_program_.set_mat4("uMVP", light_view_proj_);
    shadow_program_.set1f("uExpand", 0.0f);
    shadow_program_.set1i("uTex", 0);
    glBindVertexArray(model_.vao);
    glBindBuffer(glc::GL_ELEMENT_ARRAY_BUFFER, model_.ebo);
    glBindBufferBase(glc::GL_SHADER_STORAGE_BUFFER, kBindingVerts, model_.vbo);
    glBindBufferBase(glc::GL_SHADER_STORAGE_BUFFER, kBindingPos, model_.out_pos_ssbo);
    glBindBufferBase(glc::GL_SHADER_STORAGE_BUFFER, kBindingNrm, model_.out_nrm_ssbo);

    int first = 0;
    for (const Material& mt : model.materials) {
        const int count = mt.face_count;
        if (count <= 0) continue;
        // 隐藏的 alpha=0 层不应参与投影，否则会多出一块不该存在的影子。
        if (!settings.preview_alpha && mt.diffuse.w < 0.001f) {
            first += count;
            continue;
        }
        bool use_tex = false;
        if (mt.texture >= 0) {
            const Texture& tex = texture_for(model, mt.texture);
            if (tex.id) {
                glBindTexture(glc::GL_TEXTURE_2D, tex.id);
                use_tex = true;
            }
        }
        shadow_program_.set1i("uUseTex", use_tex ? 1 : 0);
        glDrawElements(glc::GL_TRIANGLES, count, glc::GL_UNSIGNED_INT,
                       reinterpret_cast<const void*>(static_cast<std::size_t>(first) *
                                                     sizeof(std::uint32_t)));
        first += count;
    }
    glBindVertexArray(0);
    glBindTexture(glc::GL_TEXTURE_2D, 0);
}

void SceneRenderer::draw_ground_shadow(const RenderSettings& settings, const Mat4& vp) {
    glEnable(glc::GL_BLEND);
    // 只混合颜色，保持目标 alpha 不变：地面接影是画面效果，不应把导出 PNG
    // 的底色变半透明。
    glBlendFuncSeparate(glc::GL_SRC_ALPHA, glc::GL_ONE_MINUS_SRC_ALPHA, glc::GL_ZERO,
                        glc::GL_ONE);
    glDepthMask(glc::GL_FALSE);
    glDisable(glc::GL_CULL_FACE);

    ground_program_.use();
    ground_program_.set_mat4("uMVP", vp);
    ground_program_.set_mat4("uLightVP", light_view_proj_);
    ground_program_.set1f("uShadowStrength", settings.shadow_strength);
    ground_program_.set1f("uShadowSoftness", settings.shadow_softness);
    ground_program_.set1f("uShadowTexel", shadow_texel_);
    // 阴影用背景色的深色版本，而不是纯黑：在亮色主题下更像柔和的地面投影。
    const Vec3 shadow_tint = effective_background(settings) * 0.28f;
    ground_program_.set3f("uShadowColor", shadow_tint.x, shadow_tint.y, shadow_tint.z);
    glActiveTexture(glc::GL_TEXTURE0 + 3);
    glBindTexture(glc::GL_TEXTURE_2D, shadow_tex_);
    ground_program_.set1i("uShadowMap", 3);

    glBindVertexArray(ground_vao_);
    glDrawArrays(glc::GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    glDepthMask(glc::GL_TRUE);
    glDisable(glc::GL_BLEND);
    glActiveTexture(glc::GL_TEXTURE0);
    glBindTexture(glc::GL_TEXTURE_2D, 0);
}

void SceneRenderer::release_model() {
    GLuint buffers[] = {model_.vbo, model_.ebo, model_.bone_ssbo, model_.rest_ssbo,
                        model_.out_pos_ssbo, model_.out_nrm_ssbo};
    for (GLuint b : buffers)
        if (b) glDeleteBuffers(1, &b);
    if (model_.vao) glDeleteVertexArrays(1, &model_.vao);
    model_ = GpuModel{};
    for (auto& [name, tex] : textures_)
        if (tex.id) glDeleteTextures(1, &tex.id);
    textures_.clear();
    texture_by_index_.clear();
    texture_index_ready_.clear();
    base_pos_.clear();
    base_hash_ = 0;
    flesh_solver_.clear();
    flesh_gpu_.clear();
    flesh_gpu_active_ = false;
    clear_flesh_cache();
}

bool SceneRenderer::upload_model(const Model& model, std::string* error) {
    if (!ready_) {
        if (error) *error = "渲染器尚未初始化";
        return false;
    }
    release_model();

    const int nv = static_cast<int>(model.vertices.size());
    if (nv <= 0) {
        if (error) *error = "模型没有顶点";
        return false;
    }
    // 阴影正交投影需要覆盖整个模型：记录静止姿态的包围球，供每帧更新光源矩阵。
    Vec3 lo = model.vertices.pos[0];
    Vec3 hi = lo;
    for (const Vec3& p : model.vertices.pos) {
        lo.x = std::min(lo.x, p.x); lo.y = std::min(lo.y, p.y); lo.z = std::min(lo.z, p.z);
        hi.x = std::max(hi.x, p.x); hi.y = std::max(hi.y, p.y); hi.z = std::max(hi.z, p.z);
    }
    scene_center_ = (lo + hi) * 0.5f;
    scene_min_ = lo;
    scene_max_ = hi;
    float radius = 0.0f;
    for (const Vec3& p : model.vertices.pos) radius = std::max(radius, length(p - scene_center_));
    scene_radius_ = std::max(radius, 1.0f);
    std::vector<float> data(static_cast<std::size_t>(nv) * kFloatsPerVertex, 0.0f);
    for (int i = 0; i < nv; ++i) {
        const std::size_t o = static_cast<std::size_t>(i) * kFloatsPerVertex;
        const std::size_t k = static_cast<std::size_t>(i);
        data[o + 0] = model.vertices.pos[k].x;
        data[o + 1] = model.vertices.pos[k].y;
        data[o + 2] = model.vertices.pos[k].z;
        data[o + 4] = model.vertices.nrm[k].x;
        data[o + 5] = model.vertices.nrm[k].y;
        data[o + 6] = model.vertices.nrm[k].z;
        data[o + 8] = model.vertices.uv[k].x;
        data[o + 9] = model.vertices.uv[k].y;
        data[o + 10] = model.vertices.edge[k];
        data[o + 11] = static_cast<float>(model.vertices.wtype[k]);
        for (int c = 0; c < 4; ++c)
            data[o + 12 + c] = static_cast<float>(model.vertices.bones[k][static_cast<std::size_t>(c)]);
        for (int c = 0; c < 4; ++c)
            data[o + 16 + c] = model.vertices.weights[k][static_cast<std::size_t>(c)];
        const auto& sd = model.vertices.sdef[k];
        data[o + 20] = sd[0];
        data[o + 21] = sd[1];
        data[o + 22] = sd[2];
        data[o + 24] = sd[3];
        data[o + 25] = sd[4];
        data[o + 26] = sd[5];
        data[o + 28] = sd[6];
        data[o + 29] = sd[7];
        data[o + 30] = sd[8];
    }

    glGenVertexArrays(1, &model_.vao);
    glBindVertexArray(model_.vao);

    glGenBuffers(1, &model_.vbo);
    glBindBuffer(glc::GL_ARRAY_BUFFER, model_.vbo);
    glBufferData(glc::GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(data.size() * sizeof(float)),
                 data.data(), glc::GL_DYNAMIC_DRAW);
    glBindBuffer(glc::GL_SHADER_STORAGE_BUFFER, model_.vbo);

    glGenBuffers(1, &model_.ebo);
    glBindBuffer(glc::GL_ELEMENT_ARRAY_BUFFER, model_.ebo);
    glBufferData(glc::GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(model.indices.size() * sizeof(std::uint32_t)),
                 model.indices.data(), glc::GL_STATIC_DRAW);

    glBindBuffer(glc::GL_ELEMENT_ARRAY_BUFFER, model_.ebo);

    // 只绑定索引缓冲：位置/法线由 SSBO 按 gl_VertexID 取。
    glGenBuffers(1, &model_.out_pos_ssbo);
    glBindBuffer(glc::GL_SHADER_STORAGE_BUFFER, model_.out_pos_ssbo);
    glBufferData(glc::GL_SHADER_STORAGE_BUFFER,
                 static_cast<GLsizeiptr>(static_cast<std::size_t>(nv) * 4 * sizeof(float)),
                 nullptr, glc::GL_DYNAMIC_DRAW);
    glGenBuffers(1, &model_.out_nrm_ssbo);
    glBindBuffer(glc::GL_SHADER_STORAGE_BUFFER, model_.out_nrm_ssbo);
    glBufferData(glc::GL_SHADER_STORAGE_BUFFER,
                 static_cast<GLsizeiptr>(static_cast<std::size_t>(nv) * 4 * sizeof(float)),
                 nullptr, glc::GL_DYNAMIC_DRAW);

    const int nb = static_cast<int>(model.bones.size());
    glGenBuffers(1, &model_.bone_ssbo);
    glBindBuffer(glc::GL_SHADER_STORAGE_BUFFER, model_.bone_ssbo);
    glBufferData(glc::GL_SHADER_STORAGE_BUFFER,
                 static_cast<GLsizeiptr>(std::max(1, nb) * 16 * sizeof(float)), nullptr,
                 glc::GL_DYNAMIC_DRAW);
    glGenBuffers(1, &model_.rest_ssbo);
    glBindBuffer(glc::GL_SHADER_STORAGE_BUFFER, model_.rest_ssbo);
    glBufferData(glc::GL_SHADER_STORAGE_BUFFER,
                 static_cast<GLsizeiptr>(std::max(1, nb) * 4 * sizeof(float)), nullptr,
                 glc::GL_DYNAMIC_DRAW);

    glBindVertexArray(0);
    glBindBuffer(glc::GL_SHADER_STORAGE_BUFFER, 0);

    model_.vertex_count = static_cast<GLuint>(nv);
    model_.index_count = static_cast<GLuint>(model.indices.size());
    model_.bone_count = static_cast<GLuint>(nb);
    model_.ranges = material_ranges(model);
    model_.path = model.path;

    // 半透明材质排序用的面区间中心与骨骼锚点：中心取静止坐标平均值，
    // 锚点取区间首顶点权重最大的骨骼，蒙皮后中心跟着骨骼走。
    range_centers_.assign(model_.ranges.size(), Vec3{});
    range_bones_.assign(model_.ranges.size(), -1);
    for (std::size_t r = 0; r < model_.ranges.size(); ++r) {
        const MaterialRange& range = model_.ranges[r];
        if (range.count == 0 || range.start >= model.indices.size()) continue;
        Vec3 sum{};
        std::size_t n = 0;
        const std::size_t end = std::min(range.start + range.count, model.indices.size());
        for (std::size_t k = range.start; k < end; ++k) {
            const std::uint32_t vi = model.indices[k];
            if (vi >= model.vertices.size()) continue;
            sum += model.vertices.pos[vi];
            ++n;
        }
        if (n > 0) range_centers_[r] = sum * (1.0f / static_cast<float>(n));

        const std::uint32_t v0 = model.indices[range.start];
        if (v0 >= model.vertices.size()) continue;
        float best_weight = 0.0f;
        for (int c = 0; c < 4; ++c) {
            const int b = model.vertices.bones[v0][static_cast<std::size_t>(c)];
            const float w = model.vertices.weights[v0][static_cast<std::size_t>(c)];
            if (b >= 0 && w > best_weight) {
                best_weight = w;
                range_bones_[r] = b;
            }
        }
    }
    skin_cpu_.clear();

    // 骨骼静止位置只随模型结构变化，先上传一次。
    std::vector<float> rest(static_cast<std::size_t>(std::max(1, nb)) * 4, 0.0f);
    for (int i = 0; i < nb; ++i) {
        rest[static_cast<std::size_t>(i) * 4 + 0] = model.bones[static_cast<std::size_t>(i)].pos.x;
        rest[static_cast<std::size_t>(i) * 4 + 1] = model.bones[static_cast<std::size_t>(i)].pos.y;
        rest[static_cast<std::size_t>(i) * 4 + 2] = model.bones[static_cast<std::size_t>(i)].pos.z;
    }
    glBindBuffer(glc::GL_SHADER_STORAGE_BUFFER, model_.rest_ssbo);
    glBufferSubData(glc::GL_SHADER_STORAGE_BUFFER, 0,
                    static_cast<GLsizeiptr>(rest.size() * sizeof(float)), rest.data());

    skin_matrices_.assign(static_cast<std::size_t>(std::max(1, nb)) * 16, 0.0f);
    inst_data_.clear();
    textures_.clear();
    // 贴图索引缓存随模型一起重建：每个索引只解析一次路径。
    texture_by_index_.assign(model.textures.size(), Texture{});
    texture_index_ready_.assign(model.textures.size(), 0);
    // 肉体变形的焊接拓扑与 CPU 蒙皮基准坐标随模型结构重建。
    flesh_solver_.reset(model);
    base_pos_ = model.vertices.pos;
    base_hash_ = hash_rest_positions(base_pos_);
    std::string flesh_err;
    if (!flesh_gpu_.reset(model, &flesh_err))
        std::fprintf(stderr, "[flesh-gpu] %s，将回退 CPU 求解\n", flesh_err.c_str());
    flesh_gpu_active_ = false;
    clear_flesh_cache();
    return true;
}

void SceneRenderer::update_base(const Model& model, const VertexMorphResult& morph) {
    if (model_.vbo == 0) return;
    if (morph.pos == nullptr || morph.uv == nullptr) return;
    // CPU 蒙皮路径（肉体变形）从这份坐标取值，必须与 VBO 的形态结果同步；
    // 形态恢复为 0 时也要覆盖，否则会残留上一帧的顶点位置。
    if (morph.pos->size() == model.vertices.size()) {
        base_pos_.assign(morph.pos->begin(), morph.pos->end());
    } else {
        base_pos_.clear();
    }
    const std::uint64_t next_hash = hash_rest_positions(base_pos_);
    if (next_hash != base_hash_) {
        // 形态改变了静止坐标：已烘焙的顶点全部失效。
        base_hash_ = next_hash;
        clear_flesh_cache();
    }
    if (morph.pos == &model.vertices.pos && morph.uv == &model.vertices.uv) return;
    const int nv = static_cast<int>(model.vertices.size());
    glBindBuffer(glc::GL_SHADER_STORAGE_BUFFER, model_.vbo);
    // 逐顶点跨越 128 字节：整体映射后只改位置(0..2)与 UV(8..9)。
    const GLsizeiptr total =
        static_cast<GLsizeiptr>(static_cast<std::size_t>(nv) * kFloatsPerVertex * sizeof(float));
    auto* base = static_cast<std::uint8_t*>(
        glMapBufferRange(glc::GL_SHADER_STORAGE_BUFFER, 0, total,
                         glc::GL_MAP_WRITE_BIT | glc::GL_MAP_UNSYNCHRONIZED_BIT));
    if (base) {
        const auto& pos = *morph.pos;
        const auto& uv = *morph.uv;
        for (int i = 0; i < nv; ++i) {
            float* o = reinterpret_cast<float*>(
                base + static_cast<std::size_t>(i) * kFloatsPerVertex * sizeof(float));
            o[0] = pos[static_cast<std::size_t>(i)].x;
            o[1] = pos[static_cast<std::size_t>(i)].y;
            o[2] = pos[static_cast<std::size_t>(i)].z;
            o[8] = uv[static_cast<std::size_t>(i)].x;
            o[9] = uv[static_cast<std::size_t>(i)].y;
        }
        glUnmapBuffer(glc::GL_SHADER_STORAGE_BUFFER);
    }
    glBindBuffer(glc::GL_SHADER_STORAGE_BUFFER, 0);
}

void SceneRenderer::validate_gpu_skin(const Model& model, Pose& pose) {
    const std::size_t nv = model.vertices.size();
    glBindBuffer(glc::GL_SHADER_STORAGE_BUFFER, model_.out_pos_ssbo);
    auto* gpu = static_cast<float*>(glMapBufferRange(
        glc::GL_SHADER_STORAGE_BUFFER, 0,
        static_cast<GLsizeiptr>(nv * 4 * sizeof(float)), glc::GL_MAP_READ_BIT));
    if (!gpu) {
        std::printf("[GPU-CHECK] 读取 outPos 失败\n");
        return;
    }
    SkinDeformer deformer(model);
    std::vector<Vec3> cpu_pos, cpu_nrm;
    deformer.deform(model.vertices.pos, model.vertices.nrm, pose.world(), cpu_pos, cpu_nrm);
    double max_diff = 0.0;
    int bad = 0;
    for (std::size_t i = 0; i < nv; ++i) {
        const Vec3 g{gpu[i * 4 + 0], gpu[i * 4 + 1], gpu[i * 4 + 2]};
        const double d = std::max({std::fabs(static_cast<double>(g.x - cpu_pos[i].x)),
                                   std::fabs(static_cast<double>(g.y - cpu_pos[i].y)),
                                   std::fabs(static_cast<double>(g.z - cpu_pos[i].z))});
        max_diff = std::max(max_diff, d);
        if (!std::isfinite(g.x) || !std::isfinite(g.y) || !std::isfinite(g.z)) ++bad;
    }
    glUnmapBuffer(glc::GL_SHADER_STORAGE_BUFFER);
    std::printf("[GPU-CHECK] 顶点 %zu，最大偏差 %.9f，非法分量顶点 %d\n", nv, max_diff, bad);
    std::fflush(stdout);
}

void SceneRenderer::upload_skin_matrices(const Model& model, Pose& pose) {
    const std::vector<Mat4>& W = pose.world();
    const std::size_t nb = std::max<std::size_t>(1, model.bones.size());
    skin_matrices_.assign(nb * 16, 0.0f);
    // 半透明排序要用同一套 skin 矩阵把静止中心变换到当前姿态。
    skin_cpu_.assign(nb, identity4());
    for (std::size_t i = 0; i < model.bones.size(); ++i) {
        // skin = W * T(-rest)：等价于世界矩阵作用于 (p - rest)。
        // SSBO 里的 mat4 是列主序，必须转置后再写。
        Mat4 skin = mul(W[i], translation(-model.bones[i].pos));
        skin_cpu_[i] = skin;
        store_column_major(skin, skin_matrices_.data() + i * 16);
    }
    glBindBuffer(glc::GL_SHADER_STORAGE_BUFFER, model_.bone_ssbo);
    glBufferSubData(glc::GL_SHADER_STORAGE_BUFFER, 0,
                    static_cast<GLsizeiptr>(skin_matrices_.size() * sizeof(float)),
                    skin_matrices_.data());
}

void SceneRenderer::dispatch_gpu_skin() {
    glBindBufferBase(glc::GL_SHADER_STORAGE_BUFFER, kBindingBones, model_.bone_ssbo);
    glBindBufferBase(glc::GL_SHADER_STORAGE_BUFFER, kBindingRest, model_.rest_ssbo);
    glBindBufferBase(glc::GL_SHADER_STORAGE_BUFFER, kBindingVerts, model_.vbo);
    glBindBufferBase(glc::GL_SHADER_STORAGE_BUFFER, kBindingPos, model_.out_pos_ssbo);
    glBindBufferBase(glc::GL_SHADER_STORAGE_BUFFER, kBindingNrm, model_.out_nrm_ssbo);
    skin_program_.use();
    const GLuint groups = (model_.vertex_count + 63u) / 64u;
    glDispatchCompute(groups, 1, 1);
    glMemoryBarrier(glc::GL_SHADER_STORAGE_BARRIER_BIT | glc::GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);
    gpu_skin_dispatch_count = static_cast<int>(groups);
}

void SceneRenderer::upload_cpu_skin(const Model& model, Pose& pose) {
    SkinDeformer deformer(model);
    std::vector<Vec3> pos, nrm;
    const auto& base = base_pos_.size() == model.vertices.size() ? base_pos_ : model.vertices.pos;
    deformer.deform(base, model.vertices.nrm, pose.world(), pos, nrm);
    upload_skinned(pos, nrm);
}

namespace {

constexpr std::size_t kFleshCacheLimit = 512ull << 20;  // 512 MB

std::uint64_t hash_bytes(std::uint64_t h, const void* data, std::size_t bytes) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < bytes; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

std::uint64_t hash_float(std::uint64_t h, float v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return hash_bytes(h, &bits, sizeof(bits));
}

std::uint64_t hash_vec3(std::uint64_t h, Vec3 v) {
    h = hash_float(h, v.x);
    h = hash_float(h, v.y);
    return hash_float(h, v.z);
}

// 当前骨骼矩阵与「烘焙帧（或两帧插值）」的最大分量差。
float matrix_diff(const Mat4& cur, const Mat4& a, const Mat4* b, float t) {
    float diff = 0.0f;
    for (int i = 0; i < 16; ++i) {
        const float ref = b != nullptr ? a.m[i] * (1.0f - t) + b->m[i] * t : a.m[i];
        diff = std::max(diff, std::fabs(cur.m[i] - ref));
    }
    return diff;
}

}  // namespace

void SceneRenderer::build_flesh_colliders(Pose& pose, const RenderSettings& settings,
                                          std::vector<FleshColliderShape>& out) const {
    out.clear();
    if (settings.flesh_colliders.empty()) return;
    const std::vector<Mat4>& W = pose.world();
    out.reserve(settings.flesh_colliders.size());
    for (const FleshColliderSpec& spec : settings.flesh_colliders) {
        if (spec.bone < 0 || spec.bone >= pose.n) continue;
        const Mat4& m = W[static_cast<std::size_t>(spec.bone)];
        FleshColliderShape shape;
        shape.a = transform_point(m, spec.offset);
        shape.capsule = spec.shape == 1;
        shape.b = shape.capsule
                      ? transform_point(m, spec.offset + Vec3(0.0f, spec.length, 0.0f))
                      : shape.a;
        shape.radius = std::max(0.001f, spec.radius);
        shape.firmness = std::clamp(spec.firmness, 0.0f, 1.0f);
        out.push_back(shape);
    }
}

std::uint64_t SceneRenderer::flesh_param_key(
    const RenderSettings& settings, const std::vector<FleshColliderShape>& colliders) const {
    std::uint64_t h = 1469598103934665603ull;
    const float values[] = {settings.flesh_range,
                            settings.flesh_dent,
                            settings.flesh_bulge,
                            settings.flesh_width,
                            settings.flesh_firmness,
                            settings.flesh_region_firmness,
                            settings.flesh_taper,
                            settings.flesh_taper_reach,
                            settings.flesh_taper_falloff};
    for (float v : values) h = hash_float(h, v);
    const int region = settings.flesh_region_bone;
    h = hash_bytes(h, &region, sizeof(region));
    for (const FleshColliderShape& c : colliders) {
        h = hash_vec3(h, c.a);
        h = hash_vec3(h, c.b);
        h = hash_float(h, c.radius);
        h = hash_float(h, c.firmness);
        const std::uint8_t cap = c.capsule ? 1u : 0u;
        h = hash_bytes(h, &cap, sizeof(cap));
    }
    h = hash_bytes(h, &base_hash_, sizeof(base_hash_));
    return h;
}

bool SceneRenderer::upload_baked_flesh(const RenderSettings& settings, Pose& pose,
                                       std::uint64_t key) {
    if (!settings.flesh_cache || settings.flesh_frame < 0.0f) return false;
    const float frame = settings.flesh_frame;
    const int f0 = static_cast<int>(std::floor(frame));
    const float frac = frame - static_cast<float>(f0);
    const auto it0 = flesh_cache_.find(f0);
    if (it0 == flesh_cache_.end() || it0->second.param_key != key) return false;
    const FleshBakeFrame& a = it0->second;
    const FleshBakeFrame* b = nullptr;
    if (frac > 0.001f) {
        const auto it1 = flesh_cache_.find(f0 + 1);
        if (it1 == flesh_cache_.end() || it1->second.param_key != key) return false;
        b = &it1->second;
        if (b->pos.size() != a.pos.size() || b->nrm.size() != a.nrm.size()) return false;
    }
    const std::vector<Mat4>& W = pose.world();
    if (W.size() != a.bones.size() || (b != nullptr && b->bones.size() != a.bones.size()))
        return false;
    // 手动拖骨骼 / 物理回写会让当前姿态偏离烘焙帧，此时不能复用缓存。
    for (std::size_t i = 0; i < W.size(); ++i) {
        if (matrix_diff(W[i], a.bones[i], b != nullptr ? &b->bones[i] : nullptr, frac) > 0.02f)
            return false;
    }
    std::vector<Vec3> pos(a.pos.size());
    std::vector<Vec3> nrm(a.nrm.size());
    for (std::size_t i = 0; i < pos.size(); ++i) {
        pos[i] = b != nullptr ? a.pos[i] * (1.0f - frac) + b->pos[i] * frac : a.pos[i];
        Vec3 n = b != nullptr ? a.nrm[i] * (1.0f - frac) + b->nrm[i] * frac : a.nrm[i];
        if (length(n) > 1e-8f) n = normalize(n);
        nrm[i] = n;
    }
    upload_skinned(pos, nrm);
    ++flesh_cache_hits_;
    return true;
}

void SceneRenderer::deform_flesh(const Model& model, Pose& pose,
                                 const RenderSettings& settings, float frame,
                                 bool allow_smooth, std::vector<Vec3>& pos,
                                 std::vector<Vec3>& nrm) {
    SkinDeformer deformer(model);
    const auto& base = base_pos_.size() == model.vertices.size() ? base_pos_ : model.vertices.pos;
    deformer.deform(base, model.vertices.nrm, pose.world(), pos, nrm);

    FleshParams fp;
    fp.range = settings.flesh_range;
    fp.dent = settings.flesh_dent;
    fp.bulge = settings.flesh_bulge;
    fp.width = settings.flesh_width;
    fp.firmness = settings.flesh_firmness;
    fp.region_bone = settings.flesh_region_bone;
    fp.region_firmness = settings.flesh_region_firmness;
    fp.taper = settings.flesh_taper;
    fp.taper_reach = settings.flesh_taper_reach;
    fp.taper_falloff = settings.flesh_taper_falloff;

    std::vector<FleshColliderShape> colliders;
    build_flesh_colliders(pose, settings, colliders);
    const std::uint64_t key = flesh_param_key(settings, colliders);

    std::vector<Vec3> skinned;
    const bool smooth = allow_smooth && settings.flesh_smooth > 0.0f;
    if (smooth) skinned = pos;
    flesh_solver_.apply(pos, nrm, fp, colliders);
    if (smooth) {
        const float blend = std::clamp(settings.flesh_smooth, 0.0f, 0.9f) * 0.8f;
        // 只在真正连续的帧之间混合；拖时间轴 / 跳帧时重新取基准，避免拖影。
        const bool contiguous = std::fabs(frame - (flesh_prev_frame_ + 1.0f)) < 0.75f &&
                                key == flesh_prev_key_;
        if (!contiguous) flesh_prev_delta_.clear();
        smooth_flesh_pops(skinned, pos, flesh_prev_delta_, contiguous ? blend : 0.0f);
        flesh_prev_frame_ = frame;
        flesh_prev_key_ = key;
    }
}

void SceneRenderer::upload_cpu_skin_flesh(const Model& model, Pose& pose,
                                          const RenderSettings& settings) {
    std::vector<FleshColliderShape> colliders;
    build_flesh_colliders(pose, settings, colliders);
    if (upload_baked_flesh(settings, pose, flesh_param_key(settings, colliders))) return;
    std::vector<Vec3> pos, nrm;
    deform_flesh(model, pose, settings, settings.flesh_frame, true, pos, nrm);
    upload_skinned(pos, nrm);
}

bool SceneRenderer::bake_flesh_frame(const Model& model, Pose& pose,
                                     const RenderSettings& settings, int frame,
                                     std::string* error) {
    if (!flesh_solver_.valid()) {
        if (error) *error = "模型没有可用的肉体拓扑";
        return false;
    }
    RenderSettings local = settings;
    local.flesh_cache = false;
    std::vector<Vec3> pos, nrm;
    deform_flesh(model, pose, local, static_cast<float>(frame), true, pos, nrm);

    std::vector<FleshColliderShape> colliders;
    build_flesh_colliders(pose, settings, colliders);
    const std::uint64_t key = flesh_param_key(settings, colliders);

    FleshBakeFrame dst;
    dst.pos = std::move(pos);
    dst.nrm = std::move(nrm);
    dst.bones = pose.world();
    dst.param_key = key;
    auto frame_bytes = [](const FleshBakeFrame& f) {
        return (f.pos.size() + f.nrm.size()) * sizeof(Vec3) + f.bones.size() * sizeof(Mat4);
    };
    const std::size_t bytes = frame_bytes(dst);
    const auto old = flesh_cache_.find(frame);
    if (old != flesh_cache_.end()) {
        flesh_cache_bytes_ -= frame_bytes(old->second);
        flesh_cache_.erase(old);
    }
    if (flesh_cache_bytes_ + bytes > kFleshCacheLimit) {
        if (error) *error = "烘焙缓存超过 512 MB，请缩短区间或清理缓存";
        return false;
    }
    flesh_cache_bytes_ += bytes;
    flesh_cache_[frame] = std::move(dst);
    return true;
}

void SceneRenderer::clear_flesh_cache() {
    flesh_cache_.clear();
    flesh_cache_bytes_ = 0;
    flesh_cache_hits_ = 0;
    flesh_prev_delta_.clear();
    flesh_prev_frame_ = -1e9f;
    flesh_prev_key_ = 0;
}

void SceneRenderer::upload_skinned(const std::vector<Vec3>& pos, const std::vector<Vec3>& nrm) {
    std::vector<float> pbuf(static_cast<std::size_t>(pos.size()) * 4);
    std::vector<float> nbuf(static_cast<std::size_t>(nrm.size()) * 4);
    for (std::size_t i = 0; i < pos.size(); ++i) {
        pbuf[i * 4 + 0] = pos[i].x;
        pbuf[i * 4 + 1] = pos[i].y;
        pbuf[i * 4 + 2] = pos[i].z;
        pbuf[i * 4 + 3] = 1.0f;
        nbuf[i * 4 + 0] = nrm[i].x;
        nbuf[i * 4 + 1] = nrm[i].y;
        nbuf[i * 4 + 2] = nrm[i].z;
        nbuf[i * 4 + 3] = 0.0f;
    }
    glBindBuffer(glc::GL_SHADER_STORAGE_BUFFER, model_.out_pos_ssbo);
    glBufferSubData(glc::GL_SHADER_STORAGE_BUFFER, 0,
                    static_cast<GLsizeiptr>(pbuf.size() * sizeof(float)), pbuf.data());
    glBindBuffer(glc::GL_SHADER_STORAGE_BUFFER, model_.out_nrm_ssbo);
    glBufferSubData(glc::GL_SHADER_STORAGE_BUFFER, 0,
                    static_cast<GLsizeiptr>(nbuf.size() * sizeof(float)), nbuf.data());
    // CPU 路径下顶点着色器同样按 SSBO 读取，绑定要与计算路径保持一致。
    glBindBufferBase(glc::GL_SHADER_STORAGE_BUFFER, kBindingVerts, model_.vbo);
    glBindBufferBase(glc::GL_SHADER_STORAGE_BUFFER, kBindingPos, model_.out_pos_ssbo);
    glBindBufferBase(glc::GL_SHADER_STORAGE_BUFFER, kBindingNrm, model_.out_nrm_ssbo);
}

SceneRenderer::Texture& SceneRenderer::texture_for(const Model& model, int index) {
    static Texture missing;
    if (index < 0 || index >= static_cast<int>(model.textures.size())) return missing;
    if (static_cast<std::size_t>(index) < texture_index_ready_.size() &&
        texture_index_ready_[static_cast<std::size_t>(index)])
        return texture_by_index_[static_cast<std::size_t>(index)];
    const std::string path = model.resolve_texture(index);
    auto it = textures_.find(path);
    if (it != textures_.end()) {
        // 同一个文件被多个材质引用：复用已加载的纹理，只补一次索引缓存。
        if (static_cast<std::size_t>(index) < texture_by_index_.size()) {
            texture_by_index_[static_cast<std::size_t>(index)] = it->second;
            texture_index_ready_[static_cast<std::size_t>(index)] = 1;
            return texture_by_index_[static_cast<std::size_t>(index)];
        }
        return it->second;
    }
    Texture tex;
    int w = 0, h = 0, comp = 0;
    // PMX 的 UV 原点在左上角（DirectX 约定），stb 默认把首行作为纹理第 0 行，
    // 正好对应 v=0，因此这里不能再做垂直翻转；翻转会让脸部/表情 UV 落到图集
    // 的另一块区域（眼睛变成黑框、露出张嘴表情）。
    stbi_set_flip_vertically_on_load(0);
    // stbi_load 用 fopen 打开窄字符路径，中文/日文纹理目录会失败；先按 UTF-8 路径读入内存。
    std::ifstream file = open_ifstream(path);
    const std::string bytes((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
    unsigned char* pixels =
        bytes.empty() ? nullptr
                      : stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()),
                                              static_cast<int>(bytes.size()), &w, &h, &comp, 4);
    if (pixels) {
        // 统计贴图透明覆盖：丝袜、蕾丝、发丝高光这类材质靠贴图 alpha 表现
        // 半透明，但 PMX 里 diffuse alpha 仍是 1。超过 1% 的像素带透明就
        // 认为需要走透明 pass，避免低 alpha 像素的黑色 RGB 被不透明画出。
        std::size_t transparent = 0;
        const std::size_t pixel_count =
            static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
        for (std::size_t i = 0; i < pixel_count; ++i) {
            if (pixels[i * 4 + 3] < 250) ++transparent;
        }
        tex.has_alpha = transparent * 100 > pixel_count;
        glGenTextures(1, &tex.id);
        glBindTexture(glc::GL_TEXTURE_2D, tex.id);
        glPixelStorei(glc::GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(glc::GL_TEXTURE_2D, 0, glc::GL_RGBA8, w, h, 0, glc::GL_RGBA,
                     glc::GL_UNSIGNED_BYTE, pixels);
        glGenerateMipmap(glc::GL_TEXTURE_2D);
        glTexParameteri(glc::GL_TEXTURE_2D, glc::GL_TEXTURE_MIN_FILTER,
                        static_cast<GLint>(glc::GL_LINEAR_MIPMAP_LINEAR));
        glTexParameteri(glc::GL_TEXTURE_2D, glc::GL_TEXTURE_MAG_FILTER,
                        static_cast<GLint>(glc::GL_LINEAR));
        glTexParameteri(glc::GL_TEXTURE_2D, glc::GL_TEXTURE_WRAP_S,
                        static_cast<GLint>(glc::GL_REPEAT));
        glTexParameteri(glc::GL_TEXTURE_2D, glc::GL_TEXTURE_WRAP_T,
                        static_cast<GLint>(glc::GL_REPEAT));
        // 各向异性过滤：发丝/裙摆这类斜视表面只用 trilinear 会被拉到偏低的
        // mip 层级，细节被平均成一片。按驱动上限取 16×，不支持时该调用无效。
        GLfloat max_aniso = 1.0f;
        glGetFloatv(glc::GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &max_aniso);
        if (max_aniso > 1.0f) {
            glTexParameteri(glc::GL_TEXTURE_2D, glc::GL_TEXTURE_MAX_ANISOTROPY_EXT,
                            static_cast<GLint>(std::min(max_aniso, 16.0f)));
        }
        stbi_image_free(pixels);
        tex.valid = true;
    }
    auto res = textures_.emplace(path, tex);
    if (static_cast<std::size_t>(index) < texture_by_index_.size()) {
        texture_by_index_[static_cast<std::size_t>(index)] = res.first->second;
        texture_index_ready_[static_cast<std::size_t>(index)] = 1;
        return texture_by_index_[static_cast<std::size_t>(index)];
    }
    return res.first->second;
}

SceneRenderer::Texture SceneRenderer::toon_for(const Model& model, const Material& mt) {
    if (mt.toon_shared == 0) return texture_for(model, mt.toon);
    const int index = std::clamp(static_cast<int>(mt.toon), 0, toon::toon_ramp_count - 1);
    Texture builtin;
    builtin.id = toon_ramps_[index];
    builtin.valid = builtin.id != 0;
    return builtin;
}

void SceneRenderer::bind_material(const Model& model, const Material& mt,
                                  const RenderSettings& settings) {
    // 预览模式下 alpha=0 的材质按不透明绘制（分类时已放进 opaque 列表）。
    float alpha = mt.diffuse.w;
    if (settings.preview_alpha && alpha < 0.001f) alpha = 1.0f;
    mesh_program_.set4f("uDiffuse", mt.diffuse.x, mt.diffuse.y, mt.diffuse.z, alpha);
    mesh_program_.set3f("uAmbient", mt.ambient);
    mesh_program_.set3f("uSpecular", mt.specular);
    mesh_program_.set1f("uSpecCoef", mt.specular_coef);
    mesh_program_.set1f("uToon", settings.toon);
    mesh_program_.set3f("uLightDir", normalize(settings.light_dir));

    Texture& tex = texture_for(model, mt.texture);
    glActiveTexture(glc::GL_TEXTURE0);
    glBindTexture(glc::GL_TEXTURE_2D, tex.valid ? tex.id : 0);
    mesh_program_.set1i("uTex", 0);
    mesh_program_.set1i("uUseTex", tex.valid ? 1 : 0);

    Texture& sph = texture_for(model, mt.sphere);
    glActiveTexture(glc::GL_TEXTURE0 + 1);
    glBindTexture(glc::GL_TEXTURE_2D, sph.valid ? sph.id : 0);
    mesh_program_.set1i("uSphereTex", 1);
    mesh_program_.set1i("uUseSphere", sph.valid ? 1 : 0);
    mesh_program_.set1i("uSphereMode", static_cast<int>(mt.sphere_mode));

    const Texture toon = toon_for(model, mt);
    glActiveTexture(glc::GL_TEXTURE0 + 2);
    glBindTexture(glc::GL_TEXTURE_2D, toon.valid ? toon.id : 0);
    mesh_program_.set1i("uToonTex", 2);
    mesh_program_.set1i("uUseToonTex", toon.valid ? 1 : 0);
    glActiveTexture(glc::GL_TEXTURE0);
}

void SceneRenderer::rebuild_bone_overlay(const Model& model, Pose& pose,
                                         const RenderSettings& settings,
                                         const Selection& selection) {
    bone_verts_.clear();
    const std::vector<Mat4>& W = pose.world();
    const int nb = static_cast<int>(model.bones.size());
    // 配色与 Python 版 theme.py 一致：普通 = BONE_RGB，物理后骨骼偏青，IK 偏橙，
    // 选中 = ACCENT（#4f9dff）。
    const Vec4 normal_color(0.72f, 0.78f, 0.88f, settings.bone_alpha);
    const Vec4 after_physics_color(0.35f, 0.85f, 0.78f, settings.bone_alpha);
    const Vec4 ik_color(0.98f, 0.75f, 0.35f, settings.bone_alpha);
    const Vec4 selected_color(0.31f, 0.62f, 1.0f, std::min(1.0f, settings.bone_alpha + 0.29f));
    auto add_tri = [this](Vec3 a, Vec3 b, Vec3 c, Vec4 color) {
        const Vec3 n = normalize(cross(b - a, c - a));
        for (const Vec3& p : {a, b, c}) {
            bone_verts_.insert(bone_verts_.end(), {p.x, p.y, p.z, n.x, n.y, n.z,
                                                   color.x, color.y, color.z, color.w});
        }
    };
    for (int i = 0; i < nb; ++i) {
        const bool selected = selection.kind == Selection::BONE && selection.index == i;
        // 骨骼整体隐藏时，只把双击选中的那一根画出来（高亮），取消选择后恢复隐藏。
        if (!settings.show_bones && !(settings.focus_selection && selected)) continue;
        const Bone& b = model.bones[static_cast<std::size_t>(i)];
        if (!(b.flags & BF_VISIBLE) || !(b.flags & BF_ENABLED)) continue;
        const Mat4& M = W[static_cast<std::size_t>(i)];
        const Vec3 head(M.at(0, 3), M.at(1, 3), M.at(2, 3));
        Vec3 tail;
        if ((b.flags & BF_TAIL_INDEX) && b.tail_bone >= 0 && b.tail_bone < nb) {
            const Mat4& T = W[static_cast<std::size_t>(b.tail_bone)];
            tail = {T.at(0, 3), T.at(1, 3), T.at(2, 3)};
        } else {
            // 尾端偏移是骨骼局部方向：必须跟着骨骼自身旋转，否则摆姿后骨骼线仍指向
            // 静止方向，与蒙皮后的网格脱节（拾取也会跟着错位）。
            tail = head + transform_dir(M, b.tail_offset);
        }
        Vec3 dir = tail - head;
        float len = length(dir);
        if (len < 1e-5f) {
            tail = head + Vec3(0.0f, 0.4f, 0.0f);
            dir = tail - head;
            len = length(dir);
        }
        dir = dir * (1.0f / std::max(1e-6f, len));
        Vec3 side(M.at(0, 0), M.at(1, 0), M.at(2, 0));
        Vec3 up2(M.at(0, 2), M.at(1, 2), M.at(2, 2));
        if (length(side) < 1e-6f || length(up2) < 1e-6f) {
            Vec3 up(0.0f, 1.0f, 0.0f);
            if (std::fabs(dir.y) > 0.98f) up = Vec3(0.0f, 0.0f, 1.0f);
            side = normalize(cross(dir, up));
            up2 = normalize(cross(side, dir));
        } else {
            side = normalize(side);
            up2 = normalize(up2);
        }
        const float width = std::max(0.008f, len * (selected ? 0.05f : 0.035f));
        const Vec3 mid = head + dir * (len * 0.22f);
        const Vec3 ring[4] = {mid + side * width, mid + up2 * width, mid - side * width,
                              mid - up2 * width};
        Vec4 color = normal_color;
        if (b.flags & BF_AFTER_PHYSICS) color = after_physics_color;
        if (b.flags & BF_IK) color = ik_color;
        if (selected) color = selected_color;
        for (int k = 0; k < 4; ++k) {
            const Vec3 a = ring[k];
            const Vec3 b2 = ring[(k + 1) % 4];
            add_tri(head, a, b2, color);
            add_tri(tail, b2, a, color);
        }
    }
    if (bone_verts_.empty()) return;
    glBindBuffer(glc::GL_ARRAY_BUFFER, bone_mesh_.vbo);
    glBufferData(glc::GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(bone_verts_.size() * sizeof(float)), bone_verts_.data(),
                 glc::GL_STREAM_DRAW);
}

void SceneRenderer::build_instances(const Model& model, Pose& pose, Physics* physics,
                                    const RenderSettings& settings,
                                    const Selection& selection) {
    inst_data_.clear();
    instance_counts_[0] = instance_counts_[1] = instance_counts_[2] = instance_counts_[3] = 0;
    const std::vector<Mat4>& W = pose.world();
    // 与 Python 版一致：静态刚体压暗为 RIGID_STATIC_RGB，动态刚体按组着色；
    // 关节 type 1 蓝、弹簧橙；选中统一用 ACCENT（#4f9dff）。
    const Vec4 rigid_static(0.58f, 0.60f, 0.66f, settings.rigid_alpha);
    const Vec4 rigid_selected(0.31f, 0.62f, 1.0f, std::min(1.0f, settings.rigid_alpha + 0.30f));
    const Vec4 joint_color(0.62f, 0.72f, 0.95f, settings.joint_alpha);
    const Vec4 joint_spring(0.95f, 0.72f, 0.33f, settings.joint_alpha);
    const Vec4 joint_selected(0.31f, 0.62f, 1.0f, std::min(1.0f, settings.joint_alpha + 0.32f));

    using Item = std::pair<Mat4, Vec4>;
    // 复用成员缓冲：刚体/关节数量在编辑过程中基本稳定，避免每帧重新分配。
    inst_boxes_.clear();
    inst_spheres_.clear();
    inst_cylinders_.clear();
    inst_joints_.clear();
    std::vector<Item>& boxes = inst_boxes_;
    std::vector<Item>& spheres = inst_spheres_;
    std::vector<Item>& cylinders = inst_cylinders_;
    std::vector<Item>& joints = inst_joints_;
    auto scale_mat = [](float sx, float sy, float sz) {
        Mat4 s;
        s.m[0] = sx;
        s.m[5] = sy;
        s.m[10] = sz;
        return s;
    };

    // 叠加层整体隐藏时，只绘制双击选中的那一个刚体/关节（高亮），取消选择后恢复隐藏。
    const bool focus_rigid = settings.focus_selection && selection.kind == Selection::RIGID;
    const bool focus_joint = settings.focus_selection && selection.kind == Selection::JOINT;

    if (settings.show_rigid || focus_rigid) {
        const int nr = static_cast<int>(model.rigid.size());
        for (int i = 0; i < nr; ++i) {
            const bool sel = selection.kind == Selection::RIGID && selection.index == i;
            if (!settings.show_rigid && !sel) continue;
            const RigidBody& rb = model.rigid[static_cast<std::size_t>(i)];
            Vec3 pos = rb.pos;
            Quat q = rigid_euler_quat(rb.rot.x, rb.rot.y, rb.rot.z);
            if (!query_body(physics, i, pos, q) && rb.bone >= 0 && rb.bone < pose.n) {
                const Mat4 off = query_bone_offset(physics, i);
                const Mat4 m = mul(W[static_cast<std::size_t>(rb.bone)], off);
                pos = {m.at(0, 3), m.at(1, 3), m.at(2, 3)};
                q = mat3_to_quat(upper3(m));
            }
            const Mat4 rot = from_quat_pos(q, pos);
            const float sx = std::fabs(rb.size.x), sy = std::fabs(rb.size.y),
                        sz = std::fabs(rb.size.z);
            // PMX 以 mode == 0 表示「跟随骨骼」，即静态（运动学）刚体。
            Vec4 color = rigid_static;
            if (rb.mode != 0) {
                const Vec3& gc = kGroupColors[static_cast<std::size_t>(rb.group) % 16u];
                color = Vec4(gc.x, gc.y, gc.z, settings.rigid_alpha);
            }
            if (sel) color = rigid_selected;
            if (rb.shape == 0) {
                const float r = std::max(1e-3f, sx);
                spheres.emplace_back(mul(rot, scale_mat(r, r, r)), color);
            } else if (rb.shape == 2) {
                // PMX 胶囊：size.x = 半径，size.y = 圆柱段高度，两端半球由球模板补齐。
                const float r = std::max(1e-3f, sx);
                const float h = std::max(1e-3f, sy);
                cylinders.emplace_back(mul(rot, scale_mat(r, h, r)), color);
                const Mat4 cap0 = mul(rot, mul(translation({0.0f, -h * 0.5f, 0.0f}),
                                                scale_mat(r, r, r)));
                const Mat4 cap1 = mul(rot, mul(translation({0.0f, h * 0.5f, 0.0f}),
                                                scale_mat(r, r, r)));
                spheres.emplace_back(cap0, color);
                spheres.emplace_back(cap1, color);
            } else {
                boxes.emplace_back(mul(rot, scale_mat(std::max(1e-3f, sx), std::max(1e-3f, sy),
                                                      std::max(1e-3f, sz))),
                                   color);
            }
        }
    }

    if (settings.show_joints || focus_joint) {
        const int nj = static_cast<int>(model.joints.size());
        for (int i = 0; i < nj; ++i) {
            const bool sel = selection.kind == Selection::JOINT && selection.index == i;
            if (!settings.show_joints && !sel) continue;
            const Joint& j = model.joints[static_cast<std::size_t>(i)];
            const Mat4 m = from_quat_pos(rigid_euler_quat(j.rot.x, j.rot.y, j.rot.z), j.pos);
            // 关节只显示为实体球：选中略大，弹簧（type 0）橙色，6DOF（type 1）蓝色。
            const float radius = sel ? 0.055f : 0.048f;
            joints.emplace_back(mul(m, scale_mat(radius, radius, radius)),
                                sel ? joint_selected
                                    : (j.type == 0 ? joint_spring : joint_color));
        }
    }

    // 肉体碰撞体：绿色半透明球 / 胶囊，跟随骨骼显示，便于在视口里对齐位置。
    if (settings.flesh && settings.show_flesh_colliders) {
        const Vec4 color(0.45f, 0.90f, 0.55f, 0.30f);
        for (const FleshColliderSpec& c : settings.flesh_colliders) {
            if (c.bone < 0 || c.bone >= pose.n) continue;
            const Mat4& m = W[static_cast<std::size_t>(c.bone)];
            const float r = std::max(0.001f, c.radius);
            if (c.shape == 0) {
                spheres.emplace_back(
                    mul(m, mul(translation(c.offset), scale_mat(r, r, r))), color);
            } else {
                const float h = std::max(0.001f, c.length);
                cylinders.emplace_back(
                    mul(m, mul(translation(c.offset + Vec3(0.0f, h * 0.5f, 0.0f)),
                              scale_mat(r, h, r))),
                    color);
                spheres.emplace_back(
                    mul(m, mul(translation(c.offset), scale_mat(r, r, r))), color);
                spheres.emplace_back(
                    mul(m, mul(translation(c.offset + Vec3(0.0f, h, 0.0f)),
                              scale_mat(r, r, r))),
                    color);
            }
        }
    }

    auto push_bucket = [this](const std::vector<Item>& items, int slot) {
        instance_counts_[slot] = static_cast<int>(items.size());
        for (const Item& it : items) {
            // 实例 SSBO 里的 mat4 是列主序，需转置；颜色紧跟其后。
            const std::size_t at = inst_data_.size();
            inst_data_.resize(at + 20);
            store_column_major(it.first, inst_data_.data() + at);
            inst_data_[at + 16] = it.second.x;
            inst_data_[at + 17] = it.second.y;
            inst_data_[at + 18] = it.second.z;
            inst_data_[at + 19] = it.second.w;
        }
    };
    // 顺序必须是 盒 → 球 → 柱 → 关节，对应 instance_counts_ 与渲染分桶。
    push_bucket(boxes, 0);
    push_bucket(spheres, 1);
    push_bucket(cylinders, 2);
    push_bucket(joints, 3);
}

void SceneRenderer::render(const Model& model, Pose& pose, Physics* physics, const Mat4& view,
                           const Mat4& proj, int width, int height,
                           const RenderSettings& settings, const Selection& selection) {
    if (!ready_ || model_.vao == 0 || width <= 0 || height <= 0) return;
    const Mat4 vp = mul(proj, view);
    // 相机世界位置：view = [R | t]，eye = -R^T t。
    const Vec3 view_t(view.at(0, 3), view.at(1, 3), view.at(2, 3));
    const Vec3 eye_neg = mul(transpose(upper3(view)), view_t);
    const Vec3 eye{-eye_neg.x, -eye_neg.y, -eye_neg.z};

    // 阴影 pass 会临时绑定自己的 FBO，结束后必须切回调用者的目标（窗口或离屏）。
    GLint target_fbo = 0;
    glGetIntegerv(glc::GL_DRAW_FRAMEBUFFER_BINDING, &target_fbo);

    glViewport(0, 0, width, height);
    const Vec3 clear_color = effective_background(settings);
    glClearColor(clear_color.x, clear_color.y, clear_color.z, 1.0f);
    glClear(glc::GL_COLOR_BUFFER_BIT | glc::GL_DEPTH_BUFFER_BIT | glc::GL_STENCIL_BUFFER_BIT);
    glEnable(glc::GL_DEPTH_TEST);
    // 用 LEQUAL 而不是 LESS：PMX 里存在大量完全共面的重复材质（如 袖口/袖口+、
    // 肌2/肌2衣、裙擺/裙擺+），LESS 会让后出现的覆盖层永远过不了深度测试，
    // 被前一层吃掉。LEQUAL 让后画的材质在等深处胜出，与 mmd_tools 的
    // __fixOverlappingFaceMaterials 处理方式一致。
    glDepthFunc(glc::GL_LEQUAL);
    glDepthMask(glc::GL_TRUE);
    glEnable(glc::GL_MULTISAMPLE);
    glDisable(glc::GL_CULL_FACE);
    glDisable(glc::GL_BLEND);
    glPolygonMode(glc::GL_FRONT_AND_BACK, settings.wireframe ? glc::GL_LINE : glc::GL_FILL);

    // ---- 蒙皮：GPU 计算着色器写 outPos/outNrm，CPU 路径则直接上传
    // 必须先于阴影 pass：阴影深度图读取的是蒙皮后的位置。
    upload_skin_matrices(model, pose);
    // 肉体变形：优先复用烘焙缓存，其次 GPU 求解，最后回退 CPU 多线程。
    flesh_gpu_active_ = false;
    if (settings.flesh) {
        std::vector<FleshColliderShape> colliders;
        build_flesh_colliders(pose, settings, colliders);
        const std::uint64_t key = flesh_param_key(settings, colliders);
        bool done = upload_baked_flesh(settings, pose, key);
        if (!done && settings.flesh_gpu && flesh_gpu_available()) {
            // GPU 肉体以 GPU 蒙皮结果为输入，即使关闭了「GPU 蒙皮」也要先跑一次。
            dispatch_gpu_skin();
            GpuFleshParams fp;
            fp.range = settings.flesh_range;
            fp.dent = settings.flesh_dent;
            fp.bulge = settings.flesh_bulge;
            fp.width = settings.flesh_width;
            fp.firmness = settings.flesh_firmness;
            fp.region_bone = settings.flesh_region_bone;
            fp.region_firmness = settings.flesh_region_firmness;
            fp.taper = settings.flesh_taper;
            fp.taper_reach = settings.flesh_taper_reach;
            fp.taper_falloff = settings.flesh_taper_falloff;
            fp.smooth = settings.flesh_smooth;
            fp.frame = settings.flesh_frame;
            std::string flesh_err;
            done = flesh_gpu_.solve(fp, colliders, key, model_.out_pos_ssbo,
                                    model_.out_nrm_ssbo, &flesh_err);
            if (done) {
                flesh_gpu_active_ = true;
                // 求解器把 binding 2 借去当空间哈希了，顶点着色器还要从 2 读 UV。
                glBindBufferBase(glc::GL_SHADER_STORAGE_BUFFER, kBindingVerts, model_.vbo);
                glBindBufferBase(glc::GL_SHADER_STORAGE_BUFFER, kBindingPos, model_.out_pos_ssbo);
                glBindBufferBase(glc::GL_SHADER_STORAGE_BUFFER, kBindingNrm, model_.out_nrm_ssbo);
            } else {
                std::fprintf(stderr, "[flesh-gpu] %s\n", flesh_err.c_str());
            }
        }
        if (!done) upload_cpu_skin_flesh(model, pose, settings);
    } else if (settings.use_gpu_skinning) {
        dispatch_gpu_skin();
    } else {
        upload_cpu_skin(model, pose);
    }
    if (!settings.flesh && settings.use_gpu_skinning && gpu_check_enabled_ && !gpu_checked_) {
        validate_gpu_skin(model, pose);
        gpu_checked_ = true;
    }

    // ---- 方向光阴影：先渲染深度图，再切回主目标绘制接收阴影的几何。
    const bool shadows_on = settings.shadows && settings.show_mesh && shadow_fbo_ != 0;
    if (shadows_on) {
        update_light_matrix(settings);
        draw_shadow_map(model, settings);
        glBindFramebuffer(glc::GL_FRAMEBUFFER, static_cast<GLuint>(target_fbo));
        glViewport(0, 0, width, height);
        glEnable(glc::GL_DEPTH_TEST);
        glDepthFunc(glc::GL_LEQUAL);
        glDepthMask(glc::GL_TRUE);
        glPolygonMode(glc::GL_FRONT_AND_BACK, settings.wireframe ? glc::GL_LINE : glc::GL_FILL);
        glDisable(glc::GL_CULL_FACE);
        glDisable(glc::GL_BLEND);
        if (settings.shadow_ground) draw_ground_shadow(settings, vp);
    }

    // ---- 网格地面
    if (settings.grid) {
        ensure_grid(settings);
        line_program_.use();
        line_program_.set_mat4("uMVP", vp);
        line_program_.set1f("uAlpha", 1.0f);
        glBindVertexArray(grid_vao_);
        glDrawArrays(glc::GL_LINES, 0, static_cast<GLsizei>(grid_vertex_count_));
        glBindVertexArray(0);
    }

    // ---- 主网格
    if (settings.show_mesh) {
        glBindVertexArray(model_.vao);
        glBindBuffer(glc::GL_ELEMENT_ARRAY_BUFFER, model_.ebo);

        // 复用成员缓冲：每帧只 clear 不重新分配，材质数通常几十个。
        opaque_ranges_.clear();
        alpha_ranges_.clear();
        std::vector<int>& opaque = opaque_ranges_;
        std::vector<int>& alpha = alpha_ranges_;
        // PMX 面片在右手系视图里是逆时针朝外。此前误设为 CW，会把所有单面材质
        // 的背面当成正面绘制：脸部/眼睛/嘴这些细节层被背面剔除吃掉（只剩空眼
        // 白和贴图里张开的嘴），衣物的法线也随之反转、越画越亮（深色海军蓝变
        // 成浅紫）。这里必须用 CCW。
        glFrontFace(glc::GL_CCW);
        // 相机位置与视图矩阵是全局 uniform，逐材质只改材质参数。
        mesh_program_.use();
        mesh_program_.set_mat4("uView", view);
        mesh_program_.set3f("uEye", eye);
        mesh_program_.set1f("uRim", settings.rim);
        mesh_program_.set1f("uToonEdge", settings.toon_edge);
        mesh_program_.set1f("uModelBrightness", settings.model_brightness);
        // 全场统一的方向光参数，逐材质只改材质自身的漫反射/贴图等。
        mesh_program_.set3f("uLightColor", settings.light_color);
        mesh_program_.set1f("uLightIntensity", settings.light_intensity);
        mesh_program_.set3f("uSceneAmbient", settings.ambient_color);
        mesh_program_.set1i("uShadows", shadows_on ? 1 : 0);
        mesh_program_.set_mat4("uLightVP", light_view_proj_);
        mesh_program_.set1f("uShadowStrength", settings.shadow_strength);
        mesh_program_.set1f("uShadowSoftness", settings.shadow_softness);
        mesh_program_.set1f("uShadowTexel", shadow_texel_);
        mesh_program_.set1f("uShadowBias", shadow_bias_);
        glActiveTexture(glc::GL_TEXTURE0 + 3);
        glBindTexture(glc::GL_TEXTURE_2D, shadow_tex_);
        mesh_program_.set1i("uShadowMap", 3);
        glActiveTexture(glc::GL_TEXTURE0);
        for (std::size_t r = 0; r < model_.ranges.size(); ++r) {
            const int mi = model_.ranges[r].material;
            float a = mi >= 0 ? model.materials[static_cast<std::size_t>(mi)].diffuse.w : 1.0f;
            // PmxEditor 风格预览：忽略材质透明度，让 alpha=0 的隐藏层（外裙、
            // 光带等）进入不透明 pass 可见。
            if (settings.preview_alpha && a < 0.001f) a = 1.0f;
            // 贴图自带 alpha 的材质（丝袜、蕾丝、发丝高光）必须混合：放进不透明
            // pass 会把低 alpha 像素的黑色 RGB 原样画出来，形成密集黑点/黑边。
            bool tex_alpha = false;
            if (mi >= 0) {
                const Material& mt = model.materials[static_cast<std::size_t>(mi)];
                if (mt.texture >= 0) tex_alpha = texture_for(model, mt.texture).has_alpha;
            }
            if (a >= 0.999f && !tex_alpha) opaque.push_back(static_cast<int>(r));
            else alpha.push_back(static_cast<int>(r));
        }

        auto draw_range = [&](int ri, bool outline) {
            const MaterialRange& range = model_.ranges[static_cast<std::size_t>(ri)];
            if (range.count == 0) return;
            const Material& mt = range.material >= 0
                                     ? model.materials[static_cast<std::size_t>(range.material)]
                                     : Material{};
            if (outline) {
                outline_program_.use();
                outline_program_.set_mat4("uView", view);
                outline_program_.set_mat4("uProj", proj);
                // NDC 半宽：投影后约 0.4~1.2 像素，且不随相机距离变化。
                outline_program_.set1f("uWidth", 0.0009f + mt.edge_size * 0.00045f);
                outline_program_.set4f("uOutlineColor", mt.edge_color.x, mt.edge_color.y,
                                       mt.edge_color.z, std::min(1.0f, mt.edge_color.w * 0.9f));
                // 外壳沿用材质贴图做 alpha 裁剪，避免填满贴图的镂空区域。
                Texture& tex = texture_for(model, mt.texture);
                glActiveTexture(glc::GL_TEXTURE0);
                glBindTexture(glc::GL_TEXTURE_2D, tex.valid ? tex.id : 0);
                outline_program_.set1i("uTex", 0);
                outline_program_.set1i("uUseTex", tex.valid ? 1 : 0);
            } else {
                mesh_program_.use();
                mesh_program_.set_mat4("uMVP", vp);
                mesh_program_.set1f("uExpand", 0.0f);
                bind_material(model, mt, settings);
            }
            glDrawElements(glc::GL_TRIANGLES, static_cast<GLsizei>(range.count),
                           glc::GL_UNSIGNED_INT,
                           reinterpret_cast<const void*>(static_cast<std::uintptr_t>(
                               range.start * sizeof(std::uint32_t))));
        };

        for (int ri : opaque) {
            const MaterialRange& range = model_.ranges[static_cast<std::size_t>(ri)];
            const Material& mt = range.material >= 0
                                     ? model.materials[static_cast<std::size_t>(range.material)]
                                     : Material{};
            if (mt.draw_flags & MF_DOUBLE_SIDED) {
                glDisable(glc::GL_CULL_FACE);
            } else {
                glEnable(glc::GL_CULL_FACE);
                glCullFace(glc::GL_BACK);
            }
            draw_range(ri, false);
        }

        // 倒置壳描边必须在网格之后：网格已写好深度，外壳只在轮廓外侧露出；
        // 若先画外壳会把整个可见面涂成描边色。描边又要画在透明材质之前：
        // 透明材质不写深度，若描边后画，外壳会盖在纱/布表面，露出大块描边色。
        // 因贴图 alpha 进入透明 pass 的材质（发丝、蕾丝）也要描边——它们的
        // diffuse alpha 仍为 1，只是贴图带透明；真正半透明/隐藏的材质保持不画。
        auto wants_edge = [&](int ri) {
            const MaterialRange& range = model_.ranges[static_cast<std::size_t>(ri)];
            if (range.material < 0) return false;
            const Material& mt = model.materials[static_cast<std::size_t>(range.material)];
            return (mt.draw_flags & MF_EDGE) != 0 && mt.edge_size > 0.0f;
        };
        auto edge_in_alpha = [&](int ri) {
            const MaterialRange& range = model_.ranges[static_cast<std::size_t>(ri)];
            return range.material >= 0 &&
                   model.materials[static_cast<std::size_t>(range.material)].diffuse.w >= 0.999f;
        };
        if (settings.outline) {
            bool any_edge = false;
            for (int ri : opaque) any_edge = any_edge || wants_edge(ri);
            for (int ri : alpha) any_edge = any_edge || (edge_in_alpha(ri) && wants_edge(ri));
            if (any_edge) {
                glEnable(glc::GL_BLEND);
                glBlendFunc(glc::GL_SRC_ALPHA, glc::GL_ONE_MINUS_SRC_ALPHA);
                glDepthMask(glc::GL_FALSE);
                // 与 Python 版一致：先用模板缓冲标记当前材质已覆盖的像素，
                // 外壳只在材质轮廓外侧绘制。单层/朝内的布料如果用纯深度裁剪，
                // 外扩后的壳会落在表面之前，把整片衣服涂成描边色。
                glEnable(glc::GL_STENCIL_TEST);
                glStencilMask(0xFF);
                auto draw_outline = [&](int ri) {
                    glClear(glc::GL_STENCIL_BUFFER_BIT);
                    // 只写模板：材质已绘制，深度相等即视为覆盖。
                    glDisable(glc::GL_CULL_FACE);
                    glColorMask(glc::GL_FALSE, glc::GL_FALSE, glc::GL_FALSE, glc::GL_FALSE);
                    glDepthFunc(glc::GL_LEQUAL);
                    glStencilFunc(glc::GL_ALWAYS, 1, 0xFF);
                    glStencilOp(glc::GL_KEEP, glc::GL_KEEP, glc::GL_REPLACE);
                    draw_range(ri, false);

                    glColorMask(glc::GL_TRUE, glc::GL_TRUE, glc::GL_TRUE, glc::GL_TRUE);
                    glDepthFunc(glc::GL_LESS);
                    glStencilFunc(glc::GL_NOTEQUAL, 1, 0xFF);
                    glStencilOp(glc::GL_KEEP, glc::GL_KEEP, glc::GL_KEEP);
                    glEnable(glc::GL_CULL_FACE);
                    glCullFace(glc::GL_FRONT);
                    draw_range(ri, true);
                };
                for (int ri : opaque) {
                    if (wants_edge(ri)) draw_outline(ri);
                }
                for (int ri : alpha) {
                    if (edge_in_alpha(ri) && wants_edge(ri)) draw_outline(ri);
                }
                glDisable(glc::GL_STENCIL_TEST);
                glCullFace(glc::GL_BACK);
                glDepthMask(glc::GL_TRUE);
                glDisable(glc::GL_BLEND);
            }
            // 描边内部把深度比较改回了 LESS，这里恢复共面覆盖语义，供透明 pass 使用。
            glDepthFunc(glc::GL_LEQUAL);
        }

        // 半透明材质最后画，才能正确覆盖前面已经完成的描边。
        if (!alpha.empty()) {
            // 关掉深度写入后，绘制顺序就是最终叠加顺序：按面区间中心到相机的
            // 距离从远到近排序（中心随锚点骨骼蒙皮），否则固定材质顺序会让
            // 远侧布料盖住近侧。
            auto range_distance = [&](int ri) {
                const std::size_t k = static_cast<std::size_t>(ri);
                Vec3 center = k < range_centers_.size() ? range_centers_[k] : Vec3{};
                const int bone = k < range_bones_.size() ? range_bones_[k] : -1;
                if (bone >= 0 && static_cast<std::size_t>(bone) < skin_cpu_.size())
                    center = transform_point(skin_cpu_[static_cast<std::size_t>(bone)], center);
                return length(center - eye);
            };
            // 距离相同时（共面叠加层，如丝袜 + 闪点）保持材质顺序，
            // 否则 std::sort 的任意顺序会让两层前后翻转、闪烁。
            std::sort(alpha.begin(), alpha.end(), [&](int a, int b) {
                const float da = range_distance(a);
                const float db = range_distance(b);
                if (da != db) return da > db;
                return a < b;
            });
            glEnable(glc::GL_BLEND);
            glBlendFunc(glc::GL_SRC_ALPHA, glc::GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(glc::GL_FALSE);
            glDisable(glc::GL_CULL_FACE);
            for (int ri : alpha) draw_range(ri, false);
            glDepthMask(glc::GL_TRUE);
            glDisable(glc::GL_BLEND);
        }
        glBindVertexArray(0);
        glFrontFace(glc::GL_CCW);
    }

    // ---- 叠加层（半透明实体，不写深度；关闭深度测试使骨骼/刚体不被模型遮挡）
    glPolygonMode(glc::GL_FRONT_AND_BACK, glc::GL_FILL);
    glEnable(glc::GL_BLEND);
    glBlendFunc(glc::GL_SRC_ALPHA, glc::GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(glc::GL_FALSE);
    glDisable(glc::GL_DEPTH_TEST);
    glDisable(glc::GL_CULL_FACE);

    // 叠加层整体隐藏时仍要画选中的那一个（双击临时显示）。
    const bool focus_bone = settings.focus_selection && selection.kind == Selection::BONE;
    const bool focus_overlay_inst = settings.focus_selection &&
                                    (selection.kind == Selection::RIGID ||
                                     selection.kind == Selection::JOINT);

    if (settings.show_bones || focus_bone) {
        rebuild_bone_overlay(model, pose, settings, selection);
        if (!bone_verts_.empty()) {
            solid_program_.use();
            solid_program_.set_mat4("uMVP", vp);
            solid_program_.set3f("uLightDir", kOverlayLightDir);
            solid_program_.set3f("uEye", eye);
            glBindVertexArray(bone_mesh_.vao);
            glDrawArrays(glc::GL_TRIANGLES, 0, static_cast<GLsizei>(bone_verts_.size() / 10));
            glBindVertexArray(0);
        }
    }

    const bool show_colliders =
        settings.flesh && settings.show_flesh_colliders && !settings.flesh_colliders.empty();
    if (settings.show_rigid || settings.show_joints || focus_overlay_inst || show_colliders) {
        build_instances(model, pose, physics, settings, selection);
        if (!inst_data_.empty()) {
            glBindBuffer(glc::GL_SHADER_STORAGE_BUFFER, overlay_instance_ssbo_);
            glBufferData(glc::GL_SHADER_STORAGE_BUFFER,
                         static_cast<GLsizeiptr>(inst_data_.size() * sizeof(float)),
                         inst_data_.data(), glc::GL_STREAM_DRAW);
            glBindBufferBase(glc::GL_SHADER_STORAGE_BUFFER, kBindingInstances,
                             overlay_instance_ssbo_);
            inst_program_.use();
            inst_program_.set_mat4("uMVP", vp);
            inst_program_.set3f("uLightDir", kOverlayLightDir);
            inst_program_.set3f("uEye", eye);
            OverlayMesh* meshes[4] = {&rigid_box_, &rigid_sphere_, &rigid_cylinder_,
                                      &joint_sphere_};
            int base_instance = 0;
            for (int b = 0; b < 4; ++b) {
                const int count = instance_counts_[b];
                if (count > 0 && meshes[b]->index_count > 0) {
                    inst_program_.set1i("uInstanceBase", base_instance);
                    glBindVertexArray(meshes[b]->vao);
                    glDrawElementsInstanced(glc::GL_TRIANGLES,
                                            static_cast<GLsizei>(meshes[b]->index_count),
                                            glc::GL_UNSIGNED_INT, nullptr, count);
                }
                base_instance += count;
            }
            glBindVertexArray(0);
        }
    }

    // ---- 光源罗盘：画在视口右上角（NDC 空间），任何取景下都完整可见。
    if (settings.show_light) {
        build_light_gizmo(settings, static_cast<float>(width) / static_cast<float>(height),
                          view);
        if (light_vertex_count_ > 0) {
            line_program_.use();
            line_program_.set_mat4("uMVP", identity4());
            line_program_.set1f("uAlpha", 1.0f);
            glBindVertexArray(light_mesh_.vao);
            glDrawArrays(glc::GL_LINES, 0, static_cast<GLsizei>(light_vertex_count_));
            glBindVertexArray(0);
        }
    }

    // ---- 场景相机线框：最后画，始终可见（与 Blender 的相机对象一致）。
    if (settings.show_camera && settings.camera_valid) {
        build_camera_gizmo(settings, eye);
        if (camera_vertex_count_ > 0) {
            line_program_.use();
            line_program_.set_mat4("uMVP", vp);
            line_program_.set1f("uAlpha", 1.0f);
            glBindVertexArray(camera_mesh_.vao);
            glDrawArrays(glc::GL_LINES, 0, static_cast<GLsizei>(camera_vertex_count_));
            glBindVertexArray(0);
        }
    }

    glDepthMask(glc::GL_TRUE);
    glEnable(glc::GL_DEPTH_TEST);
    glDisable(glc::GL_BLEND);
    glPolygonMode(glc::GL_FRONT_AND_BACK, glc::GL_FILL);
    glBindBuffer(glc::GL_SHADER_STORAGE_BUFFER, 0);
}

void SceneRenderer::collect_picks(const Model& model, Pose& pose, Physics* physics,
                                  const Mat4& view, const Mat4& proj, int width, int height,
                                  float sx, float sy, float max_px, int kinds,
                                  std::vector<PickHit>& out) const {
    out.clear();
    if (model_.vao == 0) return;
    const Mat4 vp = project_view_proj(view, proj);
    const std::vector<Mat4>& W = pose.world();

    // 骨骼：线段（头 → 尾）距离
    const int nb = static_cast<int>(model.bones.size());
    for (int i = 0; (kinds & PICK_BONE) && i < nb; ++i) {
        const Bone& b = model.bones[static_cast<std::size_t>(i)];
        if (!(b.flags & BF_VISIBLE)) continue;
        const Mat4& M = W[static_cast<std::size_t>(i)];
        const Vec3 head(M.at(0, 3), M.at(1, 3), M.at(2, 3));
        Vec3 tail;
        if ((b.flags & BF_TAIL_INDEX) && b.tail_bone >= 0 && b.tail_bone < nb) {
            const Mat4& T = W[static_cast<std::size_t>(b.tail_bone)];
            tail = {T.at(0, 3), T.at(1, 3), T.at(2, 3)};
        } else {
            tail = head + transform_dir(M, b.tail_offset);
        }
        if (length(tail - head) < 1e-4f) tail = head + Vec3(0.0f, 0.3f, 0.0f);
        float hx, hy, tx, ty;
        if (!project_to_screen(vp, head, width, height, hx, hy)) continue;
        if (!project_to_screen(vp, tail, width, height, tx, ty)) continue;
        // 命中半径随投影长度变化：远处骨骼投影只有几像素，固定阈值会误抓整片区域。
        const float seg_px = std::hypot(tx - hx, ty - hy);
        const float radius = std::min(std::max(max_px, 0.4f * seg_px), 16.0f);
        const float d = point_segment_distance(sx, sy, hx, hy, tx, ty);
        if (d < radius) out.push_back({Selection{Selection::BONE, i}, d});
    }

    // 刚体/关节：中心投影距离
    const int nr = static_cast<int>(model.rigid.size());
    for (int i = 0; (kinds & PICK_RIGID) && i < nr; ++i) {
        const RigidBody& rb = model.rigid[static_cast<std::size_t>(i)];
        Vec3 pos = rb.pos;
        Quat q = rigid_euler_quat(rb.rot.x, rb.rot.y, rb.rot.z);
        query_body(physics, i, pos, q);
        float px, py;
        if (!project_to_screen(vp, pos, width, height, px, py)) continue;
        const float d = std::hypot(px - sx, py - sy);
        if (d < max_px) out.push_back({Selection{Selection::RIGID, i}, d});
    }
    const int nj = static_cast<int>(model.joints.size());
    for (int i = 0; (kinds & PICK_JOINT) && i < nj; ++i) {
        const Joint& j = model.joints[static_cast<std::size_t>(i)];
        float px, py;
        if (!project_to_screen(vp, j.pos, width, height, px, py)) continue;
        const float d = std::hypot(px - sx, py - sy);
        if (d < max_px) out.push_back({Selection{Selection::JOINT, i}, d});
    }
}

Selection SceneRenderer::pick(const Model& model, Pose& pose, Physics* physics, const Mat4& view,
                              const Mat4& proj, int width, int height, float sx, float sy,
                              float max_px, int kinds) const {
    std::vector<PickHit> hits;
    collect_picks(model, pose, physics, view, proj, width, height, sx, sy, max_px, kinds, hits);
    // 刚体/关节画在最上层，全量拾取时优先命中它们（与 Python 版双击语义一致）。
    Selection overlay_hit;
    Selection bone_hit;
    float overlay_dist = max_px;
    float bone_dist = max_px;
    for (const PickHit& h : hits) {
        if (h.sel.kind == Selection::BONE) {
            if (h.dist < bone_dist) {
                bone_dist = h.dist;
                bone_hit = h.sel;
            }
        } else if (h.dist < overlay_dist) {
            overlay_dist = h.dist;
            overlay_hit = h.sel;
        }
    }
    if (overlay_hit.kind != Selection::NONE) return overlay_hit;
    return bone_hit;
}

std::vector<Selection> SceneRenderer::pick_candidates(const Model& model, Pose& pose,
                                                      Physics* physics, const Mat4& view,
                                                      const Mat4& proj, int width, int height,
                                                      float sx, float sy, float max_px,
                                                      int kinds) const {
    std::vector<PickHit> hits;
    collect_picks(model, pose, physics, view, proj, width, height, sx, sy, max_px, kinds, hits);
    std::sort(hits.begin(), hits.end(), [](const PickHit& a, const PickHit& b) {
        // 距离相差超过 1 像素才按远近；近似重合时骨骼在前（Kind 枚举 BONE=1）。
        if (std::fabs(a.dist - b.dist) > 1.0f) return a.dist < b.dist;
        return static_cast<int>(a.sel.kind) < static_cast<int>(b.sel.kind);
    });
    std::vector<Selection> out;
    out.reserve(std::min<std::size_t>(hits.size(), 12));
    for (const PickHit& h : hits) {
        if (out.size() >= 12) break;  // 极密集处只列最近的一批
        out.push_back(h.sel);
    }
    return out;
}

}  // namespace pmx
