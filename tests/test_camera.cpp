// 相机视图约定：MMD 使用左手坐标系，正视图（yaw=π）下世界 +X 必须出现在
// 画面右侧；否则模型会与 MMD 左右镜像（发饰、单侧花纹全部反置）。
#include <cmath>
#include <cstdio>

#include "app/camera.hpp"

namespace {

int failures = 0;

void check(bool ok, const char* label) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[fail]", label);
    if (!ok) ++failures;
}

pmx::Vec3 to_ndc(const pmx::Mat4& vp, pmx::Vec3 p) {
    const float w = vp.at(3, 0) * p.x + vp.at(3, 1) * p.y + vp.at(3, 2) * p.z +
                    vp.at(3, 3);
    const pmx::Vec3 c = pmx::transform_point(vp, p);
    return {c.x / w, c.y / w, c.z / w};
}

}  // namespace

int main() {
    pmx::OrbitCamera cam;
    cam.target = {0.0f, 10.0f, 0.0f};
    cam.distance = 40.0f;
    cam.yaw = pmx::kPi;
    cam.pitch = 0.0f;
    const pmx::Mat4 view = cam.view();
    const pmx::Mat4 proj = cam.projection(1.0f);
    const pmx::Mat4 vp = pmx::mul(proj, view);

    check(std::fabs(to_ndc(vp, {0.0f, 10.0f, 0.0f}).x) < 1e-5f,
          "正视图注视点位于画面中心");
    check(to_ndc(vp, {1.0f, 10.0f, 0.0f}).x > 0.0f,
          "正视图世界 +X 在画面右侧（MMD 约定）");
    check(to_ndc(vp, {-1.0f, 10.0f, 0.0f}).x < 0.0f,
          "正视图世界 -X 在画面左侧");
    check(to_ndc(vp, {0.0f, 11.0f, 0.0f}).y > 0.0f, "正视图世界 +Y 在画面上方");

    check(cam.right().x > 0.99f, "camera.right 对准世界 +X");
    check(std::fabs(cam.up_vec().y - 1.0f) < 1e-4f, "camera.up_vec 对准世界 +Y");

    // 屏幕右侧的拾取射线必须指向世界 +X，否则点选与画面左右颠倒。
    const pmx::Ray right_ray = cam.ray(699.0f, 350.0f, 700, 700);
    check(right_ray.direction.x > 0.0f, "屏幕右侧射线朝世界 +X");
    const pmx::Ray left_ray = cam.ray(0.0f, 350.0f, 700, 700);
    check(left_ray.direction.x < 0.0f, "屏幕左侧射线朝世界 -X");

    if (failures == 0) {
        std::printf("全部通过\n");
    } else {
        std::printf("失败 %d 项\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
