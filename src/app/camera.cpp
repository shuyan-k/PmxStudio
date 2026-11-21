#include "app/camera.hpp"

#include <algorithm>
#include <cmath>

namespace pmx {

Vec3 OrbitCamera::eye() const {
    const float cp = std::cos(pitch);
    const Vec3 dir{cp * std::sin(yaw), std::sin(pitch), cp * std::cos(yaw)};
    return target + dir * distance;
}

Mat4 OrbitCamera::view() const {
    const Vec3 e = eye();
    const Vec3 f = normalize(target - e);
    Vec3 up{0.0f, 1.0f, 0.0f};
    if (std::fabs(dot(f, up)) > 0.999f) up = Vec3(0.0f, 0.0f, 1.0f);
    const Vec3 s = normalize(cross(f, up));
    const Vec3 u = cross(s, f);
    // MMD 是左手坐标系：从正面看时世界 +X 应在画面右侧。右手 look-at 的
    // 相机 +X 轴是 cross(f, up)（正视图时指向世界 -X），直接渲染会得到与
    // MMD 左右镜像的模型（发饰、单侧花纹都反了）。这里把相机 X 轴取反，
    // 等价于用左手系相机观察原始 PMX 数据；镜像会翻转三角形绕序，网格
    // 绘制需相应用 glFrontFace(GL_CW)。
    const Vec3 r = -s;
    Mat4 m;
    for (int c = 0; c < 3; ++c) {
        m.at(0, c) = r[c];
        m.at(1, c) = u[c];
        m.at(2, c) = -f[c];
    }
    m.at(0, 3) = -dot(r, e);
    m.at(1, 3) = -dot(u, e);
    m.at(2, 3) = dot(f, e);
    return m;
}

Mat4 OrbitCamera::projection(float aspect) const {
    const float a = aspect > 0.01f ? aspect : 0.01f;
    if (ortho) {
        const float half_h = std::max(0.01f, ortho_scale * 0.5f);
        const float half_w = half_h * a;
        return orthographic(-half_w, half_w, -half_h, half_h, near_plane, far_plane);
    }
    return perspective(fov, a, near_plane, far_plane);
}

Vec3 OrbitCamera::right() const {
    const Mat4 v = view();
    return {v.at(0, 0), v.at(0, 1), v.at(0, 2)};
}

Vec3 OrbitCamera::up_vec() const {
    const Mat4 v = view();
    return {v.at(1, 0), v.at(1, 1), v.at(1, 2)};
}

void OrbitCamera::orbit(float dx, float dy) {
    // 视图是左手系镜像，屏幕右移对应 yaw 增大，保持「抓住场景拖动」的手感。
    yaw += dx * 0.0075f;
    pitch = clampf(pitch + dy * 0.0065f, -1.52f, 1.52f);
}

void OrbitCamera::pan(float dx, float dy) {
    const float k = distance * 0.0018f;
    target -= right() * (dx * k);
    target += up_vec() * (dy * k);
}

void OrbitCamera::zoom(float scroll) {
    distance = clampf(distance * std::pow(0.88f, scroll), 0.6f, 2000.0f);
}

void OrbitCamera::fit(Vec3 lo, Vec3 hi) {
    const Vec3 center = (lo + hi) * 0.5f;
    const float size = length(hi - lo);
    target = center;
    // 略远的取景并保留俯角：模型站在 y=0 地面上时，投影落在脚边，
    // 接近平视会把地面裁出画面，看起来就像“没有阴影”。
    distance = std::max(1.0f, size * 1.9f);
    pitch = 0.5f;
    yaw = kPi;
}

void OrbitCamera::frame(Vec3 center, float distance_) {
    target = center;
    distance = std::max(0.6f, std::min(distance_, 2000.0f));
}

Ray OrbitCamera::ray(float sx, float sy, int width, int height) const {
    const float w = static_cast<float>(width > 1 ? width : 1);
    const float h = static_cast<float>(height > 1 ? height : 1);
    const float ndc_x = sx / w * 2.0f - 1.0f;
    const float ndc_y = 1.0f - sy / h * 2.0f;
    const float aspect = w / h;
    const float tan_half = std::tan(fov * kPi / 180.0f * 0.5f);

    const Vec3 forward = normalize(target - eye());
    const Vec3 r = right();
    const Vec3 u = up_vec();
    Ray out;
    if (ortho) {
        // 正交：射线互相平行，起点在视平面上按取景高度平移。
        const float half_h = std::max(0.01f, ortho_scale * 0.5f);
        const float half_w = half_h * aspect;
        out.origin = eye() + r * (ndc_x * half_w) + u * (ndc_y * half_h);
        out.direction = forward;
        return out;
    }
    out.origin = eye();
    out.direction = normalize(forward + r * (ndc_x * tan_half * aspect) +
                             u * (ndc_y * tan_half));
    return out;
}

}  // namespace pmx
