// 多模型场景：额外 PMX 的加载 / 摆放 / 姿态与物理推进 / 追加渲染。
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <string>

#include "app/viewer.hpp"
#include "core/fs.hpp"
#include "core/model_transform.hpp"
#ifdef PMX_ENABLE_PHYSICS
#include "physics/physics.hpp"
#endif

namespace {

// 新导入模型的默认摆放：排在主模型右侧，间距按主模型宽度估算。
float default_scene_offset(const pmx::Model& main_model, std::size_t index) {
    float width = 4.0f;
    if (!main_model.vertices.pos.empty()) {
        float lo = main_model.vertices.pos[0].x;
        float hi = lo;
        for (const pmx::Vec3& p : main_model.vertices.pos) {
            lo = std::min(lo, p.x);
            hi = std::max(hi, p.x);
        }
        width = std::max(hi - lo, 1.0f);
    }
    return width * (0.65f + 0.65f * static_cast<float>(index));
}

}  // namespace

bool Viewer::add_scene_model(const std::string& path, std::string* err) {
    auto fail = [err](const std::string& message) {
        if (err) *err = message;
        return false;
    };
    SceneModel sm;
    try {
        sm.path = path;
        sm.base = pmx::load_pmx(path);
        if (sm.base.vertices.pos.empty()) return fail("模型没有顶点：" + path);
        sm.name = !sm.base.name.empty()
                      ? sm.base.name
                      : pmx::utf8_string(pmx::utf8_path(path).stem());
        sm.pos[0] = default_scene_offset(model, scene_models.size());
        sm.model = sm.base;
        pmx::transform_model(sm.model, {sm.pos[0], sm.pos[1], sm.pos[2]}, sm.yaw, sm.scale);
    } catch (const std::exception& e) {
        return fail(e.what());
    }

    // 先放进容器再创建 Pose / Physics：它们保存的是模型引用，必须绑定到
    // 容器里稳定地址上的对象（deque 追加不会移动已有元素）。
    scene_models.push_back(std::move(sm));
    SceneModel& added = scene_models.back();
    try {
        added.pose = std::make_unique<pmx::Pose>(added.model);
        added.pose->apply();
        added.pose->world();

        added.renderer = std::make_unique<pmx::SceneRenderer>();
        std::string render_error;
        if (!added.renderer->init(&render_error)) {
            scene_models.pop_back();
            return fail("场景模型渲染器初始化失败：" + render_error);
        }
        if (!added.renderer->upload_model(added.model, &render_error)) {
            scene_models.pop_back();
            return fail("场景模型上传失败：" + render_error);
        }

#ifdef PMX_ENABLE_PHYSICS
        added.physics = std::make_unique<pmx::Physics>();
        if (!added.physics->build(added.model)) added.physics.reset();
#endif
        return true;
    } catch (const std::exception& e) {
        scene_models.pop_back();
        return fail(e.what());
    }
}

void Viewer::remove_scene_model(int index) {
    if (index < 0 || index >= static_cast<int>(scene_models.size())) return;
    scene_models.erase(scene_models.begin() + index);
}

void Viewer::clear_scene_models() { scene_models.clear(); }

bool Viewer::rebuild_scene_model(int index, std::string* err) {
    if (index < 0 || index >= static_cast<int>(scene_models.size())) {
        if (err) *err = "场景模型索引越界";
        return false;
    }
    SceneModel& sm = scene_models[static_cast<std::size_t>(index)];
    try {
        sm.model = sm.base;
        pmx::transform_model(sm.model, {sm.pos[0], sm.pos[1], sm.pos[2]}, sm.yaw,
                             sm.scale);
        sm.pose = std::make_unique<pmx::Pose>(sm.model);
        sm.pose->apply();
        sm.pose->world();
        std::string render_error;
        if (!sm.renderer) {
            sm.renderer = std::make_unique<pmx::SceneRenderer>();
            if (!sm.renderer->init(&render_error)) {
                if (err) *err = "场景模型渲染器初始化失败：" + render_error;
                return false;
            }
        }
        if (!sm.renderer->upload_model(sm.model, &render_error)) {
            if (err) *err = "场景模型上传失败：" + render_error;
            return false;
        }
#ifdef PMX_ENABLE_PHYSICS
        if (sm.physics) {
            sm.physics->release();
            if (!sm.physics->build(sm.model)) sm.physics.reset();
        } else {
            sm.physics = std::make_unique<pmx::Physics>();
            if (!sm.physics->build(sm.model)) sm.physics.reset();
        }
        sm.physics_active = false;
#endif
        return true;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
}

void Viewer::update_scene_models(float dt) {
    for (SceneModel& sm : scene_models) {
        if (!sm.pose) continue;
#ifdef PMX_ENABLE_PHYSICS
        if (!sm.physics) continue;
        if (physics_enabled) {
            sm.physics->set_params(gravity, iterations);
            sm.physics->ground = ground;
            sm.pose->apply();
            sm.pose->world();
            if (!sm.physics_active) {
                sm.physics->enabled = true;
                sm.physics->reset(*sm.pose, nullptr, true);
                sm.physics_active = true;
            }
            sm.physics->advance(*sm.pose, dt);
            sm.pose->world();
        } else if (sm.physics_active) {
            sm.physics->enabled = false;
            sm.physics_active = false;
            sm.pose->apply();
            sm.pose->world();
        }
#else
        (void)dt;
#endif
    }
}

void Viewer::render_scene_models(const pmx::RenderSettings& base, const pmx::Mat4& view,
                                 const pmx::Mat4& proj, int width, int height) {
    if (scene_models.empty()) return;
    pmx::RenderSettings settings = base;
    // 额外模型只展示网格本身：骨骼/刚体/关节、网格地面与灯光罗盘都只画一次。
    settings.show_bones = false;
    settings.show_rigid = false;
    settings.show_joints = false;
    settings.focus_selection = false;
    settings.grid = false;
    settings.show_light = false;
    settings.show_camera = false;
    // 肉体求解只针对主模型，场景模型跳过以省 CPU/GPU。
    settings.flesh = false;
    settings.flesh_colliders.clear();
    settings.show_flesh_colliders = false;
    for (SceneModel& sm : scene_models) {
        if (!sm.visible || !sm.pose || !sm.renderer) continue;
        sm.renderer->render(sm.model, *sm.pose, nullptr, view, proj, width, height, settings,
                            pmx::Selection{}, false);
    }
}
