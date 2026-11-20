// OpenGL 4.6 渲染器：Compute Shader 蒙皮 + 材质着色 + 骨骼/刚体/关节叠加层。
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/flesh.hpp"
#include "gpu/flesh_gpu.hpp"
#include "core/math.hpp"
#include "core/morph.hpp"
#include "core/pmx.hpp"
#include "core/pose.hpp"
#include "gpu/gl_api.hpp"
#include "gpu/shader.hpp"
#include "gpu/toon.hpp"

namespace pmx {

class Physics;

// 外部碰撞体（UI 与配置里的形态）：挂在骨骼上，运行时按骨骼世界矩阵换算成
// 球 / 胶囊。offset 是相对骨骼原点的局部平移，胶囊沿骨骼局部 +Y 伸展 length。
struct FleshColliderSpec {
    int bone = -1;
    int shape = 0;  // 0 球，1 胶囊
    Vec3 offset{0.0f, 0.0f, 0.0f};
    float length = 0.0f;
    float radius = 0.5f;
    float firmness = 0.0f;
};

struct RenderSettings {
    bool show_mesh = true;
    bool show_bones = true;
    bool show_rigid = true;
    bool show_joints = true;
    // 按骨骼显示分类算出的可见性（1 显示 / 0 隐藏）。为空表示全部显示。
    std::vector<std::uint8_t> bone_visible;
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
    // 背景亮度倍率：<1 向黑压缩，>1 向白插值（与 python 分支一致的提亮方式）。
    float bg_brightness = 1.0f;
    // 模型亮度倍率：乘在材质漫反射上，只影响显示与渲染输出。
    float model_brightness = 1.0f;
    // 地面网格配色，由主题或用户自定义提供。
    Vec3 grid_minor{0.30f, 0.31f, 0.34f};
    Vec3 grid_major{0.42f, 0.43f, 0.47f};
    Vec3 grid_axis_x{0.75f, 0.45f, 0.45f};
    Vec3 grid_axis_z{0.45f, 0.55f, 0.80f};
    // 方向光：light_dir 指向光源（与 shader 的 N·L 一致）。
    Vec3 light_dir{0.35f, 0.72f, 0.45f};
    Vec3 light_color{1.0f, 1.0f, 1.0f};
    float light_intensity = 1.0f;
    // 环境色：乘在暗部，等价于 MMD 照明面板的「環境色」。
    Vec3 ambient_color{1.0f, 1.0f, 1.0f};
    // 视口里画一个太阳标记与方向箭头（Blender 的灯光 gizmo）。
    bool show_light = true;

    // 方向光实时阴影：深度贴图 + 3×3 PCF；地面用 shadow catcher 接收投影。
    bool shadows = true;
    float shadow_strength = 0.65f;
    float shadow_softness = 1.0f;
    bool shadow_ground = true;

    // 接触式肉体变形（第一阶段）：自接触凹陷 + 周边隆起。
    // 启用后蒙皮走 CPU 路径（需要在求解器里访问顶点位置），区域硬度按骨骼权重生效。
    bool flesh = false;
    // GPU 求解：拓扑打包进 SSBO，每帧不回读 CPU；不可用时自动回退 CPU 多线程。
    bool flesh_gpu = true;
    float flesh_range = 0.012f;   // 自接触距离（占身高比例）
    float flesh_dent = 0.60f;     // 凹陷深度
    float flesh_bulge = 1.0f;     // 隆起强度
    float flesh_width = 3.0f;     // 隆起范围（× 接触距离）
    float flesh_firmness = 0.0f;  // 整体硬度
    int flesh_region_bone = -1;   // 额外硬区域骨骼
    float flesh_region_firmness = 0.7f;
    // Corner Taper：折痕闭合端回收隆起。
    float flesh_taper = 0.55f;
    float flesh_taper_reach = 0.70f;
    float flesh_taper_falloff = 1.0f;
    // 外部碰撞体：随骨骼运动，压入肉体并按各自硬度形变。
    std::vector<FleshColliderSpec> flesh_colliders;
    bool show_flesh_colliders = true;
    // 烘焙缓存：按整数帧复用蒙皮 + 肉体解算结果；flesh_frame 是当前时间轴位置。
    bool flesh_cache = false;
    float flesh_smooth = 0.5f;   // Smooth Pops 强度（0 关闭）
    float flesh_frame = -1.0f;   // < 0 表示不做缓存查找

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

// 按背景亮度换算离屏清屏色：<1 向黑压缩，>1 向白插值。
Vec3 effective_background(const RenderSettings& settings);

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

    // 只刷新 [first, nv) 这段顶点的位置 / 法线（附件变换用，避免整模型重建）。
    void update_vertex_range(const Model& model, std::size_t first);

    void render(const Model& model, Pose& pose, Physics* physics, const Mat4& view,
                const Mat4& proj, int width, int height, const RenderSettings& settings,
                const Selection& selection);

    // 烘焙单帧：按当前姿态计算蒙皮 + 肉体结果并写入缓存（不依赖 GL 状态）。
    bool bake_flesh_frame(const Model& model, Pose& pose, const RenderSettings& settings,
                          int frame, std::string* error);
    void clear_flesh_cache();
    int flesh_cache_frames() const { return static_cast<int>(flesh_cache_.size()); }
    std::size_t flesh_cache_bytes() const { return flesh_cache_bytes_; }
    int flesh_cache_hits() const { return flesh_cache_hits_; }

    // 屏幕空间拾取：返回命中的骨骼/刚体/关节索引。
    Selection pick(const Model& model, Pose& pose, Physics* physics, const Mat4& view,
                   const Mat4& proj, int width, int height, float sx, float sy,
                   float max_px, int kinds = PICK_ALL,
                   const std::vector<std::uint8_t>* bone_visible = nullptr) const;

    // 落点附近所有可拾取对象，按屏幕距离升序（距离接近时骨骼在前）。
    // 双击时若不止一个，用它弹出候选列表让用户指定。
    std::vector<Selection> pick_candidates(const Model& model, Pose& pose, Physics* physics,
                                           const Mat4& view, const Mat4& proj, int width,
                                           int height, float sx, float sy, float max_px,
                                           int kinds = PICK_ALL,
                                           const std::vector<std::uint8_t>* bone_visible =
                                               nullptr) const;

    int gpu_skin_dispatch_count = 0;
    std::string error;

    // 肉体 GPU 路径状态（UI 用来显示提示）。
    bool flesh_gpu_available() const { return flesh_gpu_.available() && flesh_gpu_.has_model(); }
    bool flesh_gpu_active() const { return flesh_gpu_active_; }

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
        // 贴图自身带显著透明区域（蕾丝/丝袜/发丝高光等）。PMX 里这类材质的
        // diffuse alpha 往往仍是 1，只看材质 alpha 会把它当不透明：低 alpha
        // 像素的黑色 RGB 被原样画出，表现为黑点、黑边。
        bool has_alpha = false;
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
    // 最近一次构建网格时使用的颜色：主题 / 自定义颜色变化时重建 VBO。
    Vec3 grid_minor_{0.30f, 0.31f, 0.34f};
    Vec3 grid_major_{0.42f, 0.43f, 0.47f};
    Vec3 grid_axis_x_{0.75f, 0.45f, 0.45f};
    Vec3 grid_axis_z_{0.45f, 0.55f, 0.80f};
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
    ShaderProgram shadow_program_;
    ShaderProgram ground_program_;
    bool ready_ = false;

    // 方向光阴影贴图与地面接影。shadow_size_ 固定 2048：对 MMD 模型尺寸足够，
    // 也不需要随窗口缩放重建。
    GLuint shadow_fbo_ = 0;
    GLuint shadow_tex_ = 0;
    GLsizei shadow_size_ = 2048;
    Mat4 light_view_proj_;
    float shadow_texel_ = 1.0f / 2048.0f;
    float shadow_bias_ = 0.05f;
    Vec3 scene_center_{0.0f, 8.0f, 0.0f};
    Vec3 scene_min_{-4.0f, 0.0f, -4.0f};
    Vec3 scene_max_{4.0f, 16.0f, 4.0f};
    float scene_radius_ = 20.0f;
    GLuint ground_vao_ = 0;
    GLuint ground_vbo_ = 0;

    // 顶点形态（含表情/口型）后的静止坐标，CPU 蒙皮用；GPU 路径由 VBO 提供。
    std::vector<Vec3> base_pos_;
    // 静止坐标指纹：形态变化会让烘焙缓存失效。
    std::uint64_t base_hash_ = 0;
    // 自接触求解器：结构变化时 reset，逐帧形变不改变拓扑。
    FleshSolver flesh_solver_;
    // GPU 求解器：静态拓扑只上传一次，逐帧只提交参数与碰撞体。
    GpuFleshSolver flesh_gpu_;
    bool flesh_gpu_active_ = false;
    // 上一帧 GPU 解算的姿态指纹：静止时直接复用 outPos/outNrm，省掉整条流水线。
    std::uint64_t flesh_pose_key_ = 0;
    bool flesh_pose_valid_ = false;

    // 烘焙缓存：按整数帧保存蒙皮 + 肉体形变结果，以及该帧的骨骼世界矩阵
    // （用于识别手动改姿态 / 物理回写后缓存是否仍然有效）。
    struct FleshBakeFrame {
        std::vector<Vec3> pos;
        std::vector<Vec3> nrm;
        std::vector<Mat4> bones;
        std::uint64_t param_key = 0;
    };
    std::unordered_map<int, FleshBakeFrame> flesh_cache_;
    std::size_t flesh_cache_bytes_ = 0;
    int flesh_cache_hits_ = 0;
    // Smooth Pops 的上一帧位移；只在连续帧、同一参数下混合。
    std::vector<Vec3> flesh_prev_delta_;
    float flesh_prev_frame_ = -1e9f;
    std::uint64_t flesh_prev_key_ = 0;

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
                       float max_px, int kinds,
                       const std::vector<std::uint8_t>* bone_visible,
                       std::vector<PickHit>& out) const;

    void build_templates();
    void build_grid(const RenderSettings& settings);
    void ensure_grid(const RenderSettings& settings);
    void build_toon_ramps();
    bool build_shadow_map(std::string* error);
    void build_ground();
    void update_light_matrix(const RenderSettings& settings);
    void draw_shadow_map(const Model& model, const RenderSettings& settings);
    void draw_ground_shadow(const RenderSettings& settings, const Mat4& vp);
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
    void upload_cpu_skin_flesh(const Model& model, Pose& pose, const RenderSettings& settings);
    void upload_skinned(const std::vector<Vec3>& pos, const std::vector<Vec3>& nrm);
    // 按骨骼姿态把 UI 里的碰撞体换算成世界空间形状。
    void build_flesh_colliders(Pose& pose, const RenderSettings& settings,
                               std::vector<FleshColliderShape>& out) const;
    // 参数 + 静止坐标 + 碰撞体的指纹：缓存只在指纹一致时复用。
    std::uint64_t flesh_param_key(const RenderSettings& settings,
                                  const std::vector<FleshColliderShape>& colliders) const;
    // 姿态指纹（量化后）：骨骼矩阵没变时 GPU 肉体结果无需重算。
    std::uint64_t flesh_pose_key(const RenderSettings& settings, std::uint64_t param_key) const;
    // 尝试用相邻两个烘焙帧插值上传；成功返回 true。
    bool upload_baked_flesh(const RenderSettings& settings, Pose& pose, std::uint64_t key);
    // 蒙皮 + 肉体解算（可选 Smooth Pops），供实时与烘焙共用。
    void deform_flesh(const Model& model, Pose& pose, const RenderSettings& settings,
                      float frame, bool allow_smooth, std::vector<Vec3>& pos,
                      std::vector<Vec3>& nrm);
};

}  // namespace pmx
