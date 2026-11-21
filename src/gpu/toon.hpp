// 卡通着色（Toon）管线：MMD 风格的网格/描边着色器源码与共用 toon 渐变贴图。
// 单独成模块，渲染器与外部工具共用同一份 GLSL，避免着色器文本重复维护。
#pragma once

namespace pmx {
namespace toon {

// 网格顶点着色器：顶点位置/法线来自计算着色器写出的 SSBO（binding 3/4），
// UV 仍从输入顶点缓冲（binding 2，每顶点 8 个 vec4，第 2 个是 UV）读取。
inline const char* const vertex_source = R"(#version 460 core
layout(std430, binding = 2) readonly buffer VertIn { vec4 verts[]; };
layout(std430, binding = 3) readonly buffer PosOut { vec4 outPos[]; };
layout(std430, binding = 4) readonly buffer NrmOut { vec4 outNrm[]; };
uniform mat4 uMVP;
uniform float uExpand;
out vec3 vN;
out vec2 vUV;
out vec3 vWorld;
void main() {
    uint i = uint(gl_VertexID);
    vec3 p = outPos[i].xyz;
    vec3 n = outNrm[i].xyz;
    vN = n;
    vUV = verts[i * 8u + 2u].xy;
    vWorld = p + n * uExpand;
    gl_Position = uMVP * vec4(vWorld, 1.0);
}
)";

// 网格片元着色器：Toon 光照（ramp 贴图）+ 球面贴图 + 边缘光。
inline const char* const fragment_source = R"(#version 460 core
in vec3 vN;
in vec2 vUV;
in vec3 vWorld;
out vec4 fragColor;
uniform sampler2D uTex;
uniform sampler2D uSphereTex;
uniform sampler2D uToonTex;
uniform int uUseTex;
uniform int uUseSphere;
uniform int uSphereMode;
uniform int uUseToonTex;
uniform vec4 uDiffuse;
uniform vec3 uAmbient;
uniform vec3 uSpecular;
uniform float uSpecCoef;
uniform float uToon;
uniform float uToonEdge;
uniform float uModelBrightness;
uniform float uRim;
uniform vec3 uLightDir;
uniform vec3 uLightColor;
uniform float uLightIntensity;
uniform vec3 uSceneAmbient;
uniform vec3 uEye;
uniform mat4 uView;
// 方向光阴影贴图：uLightVP 把世界坐标投到光源裁剪空间，uShadows 为 0 时完全跳过。
uniform sampler2D uShadowMap;
uniform mat4 uLightVP;
uniform int uShadows;
uniform float uShadowStrength;
uniform float uShadowSoftness;
uniform float uShadowTexel;
uniform float uShadowBias;
// 内置材质扩展插件：法线 / 金属度-粗糙度。uUse* 全为 0 时与未启用插件一致。
uniform sampler2D uNormalTex;
uniform sampler2D uOrmTex;
uniform sampler2D uMetallicTex;
uniform int uUseNormalMap;
uniform int uUseOrm;
uniform int uUseMetallic;
uniform float uNormalStrength;
float shadow_visibility(vec3 world, vec3 n, float ndl) {
    if (uShadows == 0) return 1.0;
    // 法线偏移：沿法线离开表面一小段，抵消深度贴图的采样误差，避免自阴影条纹。
    // uShadowBias 是光源空间一个纹素对应的世界尺寸，法线越背离光源偏移越大。
    float offset = uShadowBias * mix(1.0, 2.5, 1.0 - clamp(ndl, 0.0, 1.0));
    vec4 lp = uLightVP * vec4(world + n * offset, 1.0);
    vec3 sc = lp.xyz / lp.w * 0.5 + 0.5;
    if (sc.z > 1.0 || sc.x < 0.0 || sc.x > 1.0 || sc.y < 0.0 || sc.y > 1.0) return 1.0;
    // 再留一点深度偏置，薄片/共面层（睫毛、衣领）也不容易产生摩尔纹。
    float ref = sc.z - 0.0008;
    float step = uShadowTexel * max(uShadowSoftness, 0.5);
    float sum = 0.0;
    for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x) {
            float d = texture(uShadowMap, sc.xy + vec2(x, y) * step).r;
            sum += ref <= d ? 1.0 : 0.0;
        }
    return sum / 25.0;
}
// 无预计算切线的 TBN（屏幕空间导数）：PMX 不带切线也能用法线贴图。
vec3 material_ext_normal(vec3 n) {
    if (uUseNormalMap == 0) return n;
    vec3 dp1 = dFdx(vWorld);
    vec3 dp2 = dFdy(vWorld);
    vec2 duv1 = dFdx(vUV);
    vec2 duv2 = dFdy(vUV);
    vec3 dp2perp = cross(dp2, n);
    vec3 dp1perp = cross(n, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float tl = length(T);
    float bl = length(B);
    if (tl < 1e-8 || bl < 1e-8) return n;
    T /= tl;
    B /= bl;
    // 贴图按左上角为原点加载（PMX / DirectX 约定），绿色通道对应 +v。
    vec3 nt = texture(uNormalTex, vUV).xyz * 2.0 - 1.0;
    nt.xy *= uNormalStrength;
    vec3 mapped = normalize(T * nt.x + B * nt.y + n * max(nt.z, 0.05));
    return dot(mapped, n) > 0.0 ? mapped : n;
}
void main() {
    vec4 tex = uUseTex == 1 ? texture(uTex, vUV) : vec4(1.0);
    if (tex.a < 0.02) discard;
    vec4 base = vec4(tex.rgb * uDiffuse.rgb * uModelBrightness, tex.a * uDiffuse.a);
    vec3 ng = normalize(vN);
    if (!gl_FrontFacing) ng = -ng;
    // 阴影查找必须用几何法线：法线贴图只应改变明暗，不能改变可见/遮蔽关系。
    // 用扰动后的法线做 normal-offset 会把采样点移进相邻表面的深度里，在斜面
    // （腿、衣袖）上产生大片跟随贴图纹理的自阴影伪影。
    vec3 n = material_ext_normal(ng);
    // 金属度 / 粗糙度：仅在 ORM 或金属度贴图存在时改变着色。
    bool use_pbr = uUseOrm == 1 || uUseMetallic == 1;
    float metallic = 0.0;
    float roughness = 0.5;
    if (uUseOrm == 1) {
        vec4 mr = texture(uOrmTex, vUV);
        roughness = clamp(mr.g, 0.0, 1.0);
        metallic = clamp(mr.b, 0.0, 1.0);
    } else if (uUseMetallic == 1) {
        metallic = clamp(texture(uMetallicTex, vUV).r, 0.0, 1.0);
        roughness = 0.45;
    }
    // 金属没有漫反射：按金属度压暗本体色，高光改用本体色着色。
    vec3 surface = base.rgb * (1.0 - 0.9 * metallic);
    vec3 light = normalize(uLightDir);
    float ndl = dot(n, light);
    float ndl_c = max(ndl, 0.0);
    // 背光侧保留 0.30 的直射光底、受光侧升到 1.0：灯光方向改变时
    // 整个模型都有明确的明暗过渡，而不只是明暗交界处的一条窄带。
    float lambert = 0.30 + 0.70 * ndl_c;
    // Toon 光照：ramp 贴图只参与阴影侧调制，亮面保持解析光照。
    // 直接把 ramp 当作 shade 会被"大面积纯白"的 toon 压平（例如 57 个材质
    // 共用 toon3/toon4、85% 是纯白），整只模型失去明暗；mmd_tools 同样只
    // 让 toon 影响阴影色（shader.py: "toon only affect shadow color"）。
    float shade;
    if (uUseToonTex == 1) {
        float ramp = dot(texture(uToonTex, vec2(0.5, clamp(ndl, 0.0, 1.0))).rgb,
                         vec3(0.299, 0.587, 0.114));
        float lit = 0.55 + 0.45 * ndl_c;
        // 明暗分界的过渡宽度由 uToonEdge 控制：越小越接近 MMD 的硬边，
        // 过大会让整块阴影变成渐变，观感发虚。
        float shadow = 1.0 - smoothstep(0.0, max(uToonEdge, 0.01), ndl_c);
        shade = mix(lambert, lit * mix(1.0, ramp, shadow), uToon);
    } else {
        float toon = 0.60 + 0.40 * smoothstep(0.20, 0.80, ndl_c);
        shade = mix(lambert, toon, uToon);
    }
    // 阴影只调制直射光：暗部仍保留环境色，避免阴影里变成纯黑。
    float ndl_geom = max(dot(ng, light), 0.0);
    float shadow = mix(1.0, shadow_visibility(vWorld, ng, ndl_geom), uShadowStrength);
    shade *= shadow;
    // 照明色 × 强度作用在直射光上；环境色乘在暗部（MMD 的「環境色」）。
    // 环境色必须先乘一个固定强度：若直接 mix(ambient, 1, shade)，当环境色为
    // 纯白（默认值）时两端都是 1，整个漫反射恒为 1，模型完全不随灯光方向变化，
    // 只有骨骼叠加层（另一套 solid 着色器）在变——看起来像灯光绑定到了骨骼上。
    const float kAmbientStrength = 0.32;
    vec3 light_rgb = uLightColor * uLightIntensity;
    vec3 col = surface * mix(uSceneAmbient * kAmbientStrength, vec3(1.0), shade) * light_rgb;
    col += uAmbient * surface * uSceneAmbient * 0.06;
    vec3 view_dir = normalize(uEye - vWorld);
    float spec_power = 14.0;
    float spec_strength = uSpecCoef;
    vec3 spec_color = uSpecular;
    if (use_pbr) {
        // 粗糙度控制高光宽度（近似 Blinn-Phong 指数），金属度把高光染成本体色。
        spec_power = mix(140.0, 6.0, roughness * roughness);
        spec_strength = mix(uSpecCoef, 1.1, metallic);
        spec_color = mix(uSpecular, base.rgb, metallic);
    }
    float spec = pow(max(ndl, 0.0), spec_power) * spec_strength * shadow;
    col += spec_color * spec * light_rgb;
    // 没有环境贴图时给金属一个天空/地面渐变的廉价反射，避免金属区域变纯黑。
    if (use_pbr && metallic > 0.001) {
        vec3 R = reflect(-view_dir, n);
        vec3 env = mix(vec3(0.08, 0.09, 0.11), light_rgb, clamp(R.y * 0.5 + 0.5, 0.0, 1.0));
        float fres = pow(1.0 - max(dot(n, view_dir), 0.0), 3.0);
        col += env * metallic * (1.0 - roughness * 0.75) * (0.35 + 0.65 * fres);
    }
    // 球面贴图按相机空间法线的 XY 采样（MMD 约定），转动视角时高光会滑动。
    if (uUseSphere == 1) {
        vec3 nv = normalize(mat3(uView) * n);
        vec2 suv = nv.xy * 0.5 + 0.5;
        vec4 sp = texture(uSphereTex, suv);
        if (uSphereMode == 1) col = col * (sp.rgb * 1.6);
        else if (uSphereMode == 2) col = col + sp.rgb * sp.a;
        else if (uSphereMode == 3) col = mix(col, sp.rgb, sp.a);
    }
    // 边缘光：贴着轮廓的位置补一点本体色，增强立体感又不破坏卡通分层。
    float rim = smoothstep(0.6, 1.0, 1.0 - max(dot(n, view_dir), 0.0));
    col += base.rgb * (uRim * rim);
    fragColor = vec4(clamp(col, 0.0, 1.0), base.a);
}
)";

// 描边顶点着色器：视空间沿法线外扩，偏移量乘深度，投影后屏幕宽度恒定。
inline const char* const outline_vertex_source = R"(#version 460 core
layout(std430, binding = 2) readonly buffer VertIn { vec4 verts[]; };
layout(std430, binding = 3) readonly buffer PosOut { vec4 outPos[]; };
layout(std430, binding = 4) readonly buffer NrmOut { vec4 outNrm[]; };
uniform mat4 uView;
uniform mat4 uProj;
uniform float uWidth;   // 描边半宽（NDC），屏幕像素宽度恒定
out vec2 vUV;
void main() {
    uint i = uint(gl_VertexID);
    vec3 p = outPos[i].xyz;
    vec3 n = outNrm[i].xyz;
    vUV = verts[i * 8u + 2u].xy;
    vec4 vp = uView * vec4(p, 1.0);
    // 视空间沿法线外扩，偏移量乘深度：投影后描边在屏幕上宽度恒定，
    // 相机拉远不会消失，拉近也不会变成粗黑边。
    float k = uWidth * (-vp.z) / uProj[1][1];
    vp.xyz += (mat3(uView) * n) * k;
    gl_Position = uProj * vp;
}
)";

inline const char* const outline_fragment_source = R"(#version 460 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;
uniform int uUseTex;
uniform vec4 uOutlineColor;
void main() {
    // 与网格同一套 alpha 裁剪：否则蕾丝/纱这类贴图镂空的材质，外扩壳会
    // 填满被网格丢弃的透明像素，整片变成描边色（红褐色）。
    if (uUseTex == 1 && texture(uTex, vUV).a < 0.02) discard;
    fragColor = uOutlineColor;
}
)";

// 阴影深度 pass：顶点沿用网格着色器（读取蒙皮后 SSBO），片元只做贴图 alpha 裁剪，
// 让发丝、蕾丝等镂空材质投出正确形状的阴影。没有颜色输出，只写深度。
inline const char* const shadow_fragment_source = R"(#version 460 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;
uniform int uUseTex;
void main() {
    if (uUseTex == 1 && texture(uTex, vUV).a < 0.5) discard;
    fragColor = vec4(1.0);
}
)";

// 地面接影：只在实际被遮挡的区域输出半透明暗色（shadow catcher），
// 未遮挡处完全透明，因此不会改变现有背景与网格的外观。
inline const char* const ground_vertex_source = R"(#version 460 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;
out vec3 vWorld;
void main() {
    vWorld = aPos;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

inline const char* const ground_fragment_source = R"(#version 460 core
in vec3 vWorld;
out vec4 fragColor;
uniform sampler2D uShadowMap;
uniform mat4 uLightVP;
uniform float uShadowStrength;
uniform float uShadowSoftness;
uniform float uShadowTexel;
uniform vec3 uShadowColor;
void main() {
    vec4 lp = uLightVP * vec4(vWorld, 1.0);
    vec3 sc = lp.xyz / lp.w * 0.5 + 0.5;
    float vis = 1.0;
    if (sc.z <= 1.0 && sc.x >= 0.0 && sc.x <= 1.0 && sc.y >= 0.0 && sc.y <= 1.0) {
        float ref = sc.z - 0.0008;
        float step = uShadowTexel * max(uShadowSoftness, 0.5);
        float sum = 0.0;
        for (int y = -2; y <= 2; ++y)
            for (int x = -2; x <= 2; ++x)
                sum += ref <= texture(uShadowMap, sc.xy + vec2(x, y) * step).r ? 1.0 : 0.0;
        vis = sum / 25.0;
    }
    float a = (1.0 - vis) * uShadowStrength;
    if (a < 0.004) discard;
    fragColor = vec4(uShadowColor, a);
}
)";

// ---- 共用 toon 渐变（MMD 内置 toon01.bmp 的等价物）
// 10 档竖直渐变，采样时 v 坐标即 N·L(0..1)：0 为暗部，1 为亮部。
// 编号越小明暗分界越硬（接近 MMD 默认的两段卡通），越大过渡越柔和。
// 渲染器据此生成 toon_ramps_ 贴图，着色器与渐变曲线只此一处。
inline constexpr int toon_ramp_count = 10;
inline constexpr int toon_ramp_height = 64;

// 返回第 index 档在 N·L = v 处的亮度（0..1）。
inline float toon_ramp_value(int index, float v) {
    const int i = index < 0 ? 0 : (index >= toon_ramp_count ? toon_ramp_count - 1 : index);
    const float step = 9.0f - i * 0.55f;    // 越大越硬
    const float edge = 0.40f + i * 0.015f;  // 明暗分界
    float t = (v - edge) * step + 0.5f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return 0.42f + 0.58f * t;
}

}  // namespace toon
}  // namespace pmx
