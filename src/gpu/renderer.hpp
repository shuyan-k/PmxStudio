// OpenGL 4.6 渲染器：Compute Shader 蒙皮 + 材质着色 + 骨骼/刚体/关节叠加层。
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/math.hpp"
#include "core/morph.hpp"
#include "core/pmx.hpp"
#include "core/pose.hpp"
#include "gpu/gl_api.hpp"
#include "gpu/shader.hpp"
#include "gpu/toon.hpp"

namespace pmx {

class Physics;

struct RenderSettings {
    bool show_mesh = true;
    bool show_bones = true;
    bool show_rigid = true;
    bool show_joints = true;
    // 叠加层整体隐藏时是否仍绘制当前选中的那一个（双击选中后临时显示并高亮）。
    bool focus_selection = false;
    bool use_gpu_skinning = true;
    bool wireframe = false;
    bool grid = true;
    bool outline = true;
    // 忽略材质透明度：把 alpha=0 的层当作不透明绘制。
    // 本模型里大裙擺(49-53)、光帶(54-56) 的 diffuse.a 就是 0，由材质形态
    // 「change」「光帶ON」在需要时把它们加到 1，所以默认必须是关闭（隐藏）。
    bool preview_alpha = false;
    float bone_alpha = 0.85f;
    float rigid_alpha = 0.55f;
    float joint_alpha = 0.75f;
    float toon = 1.0f;
    float rim = 0.18f;  // 边缘光强度
    // 卡通明暗分界的过渡宽度（0.01 为硬边，0.45 为完全渐变）。
    float toon_edge = 0.12f;
    Vec3 background{0.13f, 0.14f, 0.16f};
    // 方向光：light_dir 指向光源（与 shader 的 N·L 一致）。
    Vec3 light_dir{0.35f, 0.72f, 0.45f};
    Vec3 light_color{1.0f, 1.0f, 1.0f};
    float light_intensity = 1.0f;
    // 环境色：乘在暗部，等价于 MMD 照明面板的「環境色」。
    Vec3 ambient_color{1.0f, 1.0f, 1.0f};
    // 视口里画一个太阳标记与方向箭头（Blender 的灯光 gizmo）。
    bool show_light = true;

    // 场景相机可视化（Blender 风格）：机位线框 + 取景锥。
    bool show_camera = false;
    bool camera_valid = false;
    Vec3 camera_eye{0.0f, 10.0f, 0.0f};
    Vec3 camera_target{0.0f, 10.0f, 0.0f};
    float camera_fov = 38.0f;
    bool camera_ortho = false;
    float camera_ortho_scale = 30.0f;
    float camera_aspect = 16.0f / 9.0f;
};

struct Selection {
    enum Kind { NONE, BONE, RIGID, JOINT };
    Kind kind = NONE;
    int index = -1;
};

// 拾取范围：视口按下时只认骨骼（避免误抓刚体），双击才包含全部叠加层。
enum PickKind {
    PICK_BONE = 1 << 0,
    PICK_RIGID = 1 << 1,
    PICK_JOINT = 1 << 2,
    PICK_ALL = PICK_BONE | PICK_RIGID | PICK_JOINT,
};

class SceneRenderer {
public:
    SceneRenderer() = default;
    ~SceneRenderer();

    bool init(std::string* error);

    // 结构变化时重建 GPU 缓冲（顶点/索引/权重/纹理）。
    bool upload_model(const Model& model, std::string* error);
    void release_model();

    // 形态导致的静止坐标/UV 变化：刷新输入 SSBO。
    void update_base(const Model& model, const VertexMorphResult& morph);

    void render(const Model& model, Pose& pose, Physics* physics, const Mat4& view,
                const Mat4& proj, int width, int height, const RenderSettings& settings,
                const Selection& selection);

    // 屏幕空间拾取：返回命中的骨骼/刚体/关节索引。
    Selection pick(const Model& model, Pose& pose, Physics* physics, const Mat4& view,
                   const Mat4& proj, int width, int height, float sx, float sy,
                   float max_px, int kinds = PICK_ALL) const;

    // 落点附近所有可拾取对象，按屏幕距离升序（距离接近时骨骼在前）。
    // 双击时若不止一个，用它弹出候选列表让用户指定。
    std::vector<Selection> pick_candidates(const Model& model, Pose& pose, Physics* physics,
                                           const Mat4& view, const Mat4& proj, int width,
                                           int height, float sx, float sy, float max_px,
                                           int kinds = PICK_ALL) const;

    int gpu_skin_dispatch_count = 0;
    std::string error;

private:
    // 首次 GPU 蒙皮后与 CPU 参考实现比对（PMX_GPU_CHECK=1 时启用）。
    void validate_gpu_skin(const Model& model, Pose& pose);
    bool gpu_check_enabled_ = false;
    bool gpu_checked_ = false;

    struct GpuModel {
        GLuint vao = 0;
        GLuint vbo = 0;
        GLuint ebo = 0;
        GLuint bone_ssbo = 0;
        GLuint rest_ssbo = 0;
        GLuint out_pos_ssbo = 0;
        GLuint out_nrm_ssbo = 0;
        GLuint index_count = 0;
        GLuint vertex_count = 0;
        GLuint bone_count = 0;
        std::vector<MaterialRange> ranges;
        std::string path;
    };

    struct Texture {
        GLuint id = 0;
        bool valid = false;
    };

    struct OverlayMesh {
        GLuint vao = 0;
        GLuint vbo = 0;
        GLuint ebo = 0;
        GLuint index_count = 0;
    };

    GpuModel model_;
    std::unordered_map<std::string, Texture> textures_;
    // 按贴图索引缓存：解析路径 / 查表只在首次使用时做，绘制时 O(1) 取纹理。
    std::vector<Texture> texture_by_index_;
    std::vector<std::uint8_t> texture_index_ready_;

    OverlayMesh bone_mesh_;       // 每帧重建：八面体（顶点色）
    OverlayMesh camera_mesh_;     // 每帧重建：场景相机线框（pos3 + color4）
    GLuint camera_vertex_count_ = 0;
    std::vector<float> camera_verts_;
    OverlayMesh light_mesh_;      // 每帧重建：光照罗盘（NDC 坐标）
    GLuint light_vertex_count_ = 0;
    std::vector<float> light_verts_;
    OverlayMesh rigid_box_;       // 模板：单位盒
    OverlayMesh rigid_sphere_;    // 模板：单位球
    OverlayMesh rigid_cylinder_;  // 模板：单位圆柱（胶囊柱段）
    OverlayMesh joint_sphere_;    // 模板：单位球
    // 实例分桶计数：盒 / 球 / 柱 / 关节（顺序与 build_instances 一致）
    int instance_counts_[4] = {0, 0, 0, 0};
    GLuint overlay_instance_ssbo_ = 0;
    GLuint grid_vao_ = 0;
    GLuint grid_vbo_ = 0;
    GLuint grid_vertex_count_ = 0;
    // MMD 内置 toon01..toon10 的程序化替代：竖直灰度渐变，低编号硬边、高编号柔和。
    GLuint toon_ramps_[10] = {};
    // 半透明材质的距离排序：面区间包围中心（静止坐标）+ 最大权重骨骼锚点。
    std::vector<Vec3> range_centers_;
    std::vector<int> range_bones_;
    std::vector<Mat4> skin_cpu_;

    // 最近一帧的相机状态：供叠加层投影、拾取与描边宽度计算读取。
    // render() 每帧刷新，拾取等只读路径可直接使用。
    Mat4 view_;
    Mat4 view_proj_;
    Vec3 eye_;
    int viewport_w_ = 0;
    int viewport_h_ = 0;

    ShaderProgram skin_program_;
    ShaderProgram mesh_program_;
    ShaderProgram outline_program_;
    ShaderProgram solid_program_;
    ShaderProgram inst_program_;
    ShaderProgram line_program_;
    bool ready_ = false;

    std::vector<float> skin_matrices_;
    std::vector<float> inst_data_;
    std::vector<float> bone_verts_;
    std::vector<float> line_verts_;
    // 每帧复用的临时缓冲，避免在绘制循环里反复分配 / 释放。
    std::vector<int> opaque_ranges_;
    std::vector<int> alpha_ranges_;
    std::vector<std::pair<Mat4, Vec4>> inst_boxes_;
    std::vector<std::pair<Mat4, Vec4>> inst_spheres_;
    std::vector<std::pair<Mat4, Vec4>> inst_cylinders_;
    std::vector<std::pair<Mat4, Vec4>> inst_joints_;

    // 单次拾取的原始命中：对象 + 到鼠标的屏幕像素距离。
    struct PickHit {
        Selection sel;
        float dist = 0.0f;
    };
    // 收集 max_px 范围内的全部命中，供 pick / pick_candidates 共用。
    void collect_picks(const Model& model, Pose& pose, Physics* physics, const Mat4& view,
                       const Mat4& proj, int width, int height, float sx, float sy,
                       float max_px, int kinds, std::vector<PickHit>& out) const;

    void build_templates();
    void build_grid();
    void build_toon_ramps();
    void rebuild_bone_overlay(const Model& model, Pose& pose, const RenderSettings& settings,
                              const Selection& selection);
    // 场景相机 gizmo：机身线框 + 前端取景锥，尺寸按到视点距离自适应。
    void build_camera_gizmo(const RenderSettings& settings, Vec3 view_eye);
    // 光照罗盘：视口右上角的圆盘 + 指向光源的箭头，画在 NDC 空间。
    void build_light_gizmo(const RenderSettings& settings, float aspect, const Mat4& view);
    void build_instances(const Model& model, Pose& pose, Physics* physics,
                         const RenderSettings& settings, const Selection& selection);
    Texture& texture_for(const Model& model, int index);
    // 材质卡通光照用的 toon 贴图：材质自带贴图或内置 toon01..10。
    Texture toon_for(const Model& model, const Material& mt);
    void bind_material(const Model& model, const Material& mt, const RenderSettings& settings);
    void upload_skin_matrices(const Model& model, Pose& pose);
    void dispatch_gpu_skin();
    void upload_cpu_skin(const Model& model, Pose& pose);
};

}  // namespace pmx
