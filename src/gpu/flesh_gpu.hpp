// GPU 肉体求解：计算着色器版本的 Fleshy（自接触 / 凹陷隆起 / Corner Taper /
// 外部碰撞体 / Smooth Pops）。与 CPU FleshSolver 使用相同的焊接拓扑与公式，
// 但所有逐顶点、逐三角形的工作都在 GPU 上完成，CPU 每帧只提交参数和碰撞体。
//
// 缓冲绑定（与蒙皮/网格着色器共用同一批 binding，避免超出 GL 4.6 的最低
// 8 个 SSBO 绑定点）：
//   0 = 静态拓扑   1 = 动态数据   2 = 空间哈希网格
//   3 = out_pos    4 = out_nrm    5 = 外部碰撞体
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/flesh.hpp"
#include "core/math.hpp"
#include "gpu/gl_api.hpp"
#include "gpu/shader.hpp"

namespace pmx {

class Model;

// 每帧参数快照；range 是占身高比例，内部按 reset 记录的身高换算成世界单位。
struct GpuFleshParams {
    float range = 0.012f;
    float dent = 0.6f;
    float bulge = 1.0f;
    float width = 3.0f;
    float firmness = 0.0f;
    int region_bone = -1;
    float region_firmness = 0.7f;
    float taper = 0.55f;
    float taper_reach = 0.7f;
    float taper_falloff = 1.0f;
    float smooth = 0.5f;
    // 时间轴帧号；只有连续帧且参数一致时才做 Smooth Pops。
    float frame = -1.0f;
};

class GpuFleshSolver {
public:
    GpuFleshSolver() = default;
    ~GpuFleshSolver();
    GpuFleshSolver(const GpuFleshSolver&) = delete;
    GpuFleshSolver& operator=(const GpuFleshSolver&) = delete;

    // 编译计算程序（需要当前 GL 上下文）。失败时返回 false 并记录日志。
    bool init(std::string* error);
    void release();

    // 模型结构变化时重建焊接拓扑、邻接表和所有 GPU 缓冲。
    bool reset(const Model& model, std::string* error);
    void clear();

    bool available() const { return ready_; }
    bool has_model() const { return weld_count_ > 0 && static_ssbo_ != 0; }
    int dispatch_count() const { return dispatch_count_; }
    const std::string& last_error() const { return error_; }

    // out_pos/out_nrm 是已由 GPU 蒙皮写好的 vec4 SSBO（每顶点 4 个 float）。
    // 求解结果原地写回，不需要回读 CPU。
    bool solve(const GpuFleshParams& params,
               const std::vector<FleshColliderShape>& colliders, std::uint64_t key,
               GLuint out_pos_ssbo, GLuint out_nrm_ssbo, std::string* error);

    // 时间线跳变 / 模型切换时调用，下一帧重新初始化 Smooth Pops 历史。
    void reset_history() { prev_valid_ = false; }

private:
    bool build_static(const Model& model, std::string* error);
    void upload_static();
    void delete_buffers();
    void set_constant_uniforms();
    void upload_colliders(const std::vector<FleshColliderShape>& colliders);

    bool ready_ = false;
    std::string error_;

    int vertex_count_ = 0;
    int weld_count_ = 0;
    int tri_count_ = 0;
    float height_ = 1.0f;

    GLuint static_ssbo_ = 0;
    GLuint dynamic_ssbo_ = 0;
    GLuint grid_ssbo_ = 0;
    GLuint collider_ssbo_ = 0;
    int collider_capacity_ = 0;

    // 静态拓扑（uint 索引，static_ssbo_ 内的偏移）。
    std::uint32_t o_canon_ = 0;
    std::uint32_t o_dup_off_ = 0;
    std::uint32_t o_dup_vert_ = 0;
    std::uint32_t o_rep_bone_ = 0;
    std::uint32_t o_rep_weight_ = 0;
    std::uint32_t o_rep_count_ = 0;
    std::uint32_t o_nb_off_ = 0;
    std::uint32_t o_nb_vert_ = 0;
    std::uint32_t o_vtri_off_ = 0;
    std::uint32_t o_vtri_id_ = 0;
    std::uint32_t o_tri_ = 0;
    std::uint32_t static_words_ = 0;
    std::vector<std::uint32_t> static_data_;

    // 动态数据（uint 索引，dynamic_ssbo_ 内的偏移；float 以位模式存放）。
    std::uint32_t o_weld_pos_ = 0;
    std::uint32_t o_weld_nrm_ = 0;
    std::uint32_t o_tri_nrm_ = 0;
    std::uint32_t o_firm_ = 0;
    std::uint32_t o_taper_ = 0;
    std::uint32_t o_bulge_ = 0;
    std::uint32_t o_delta_ = 0;
    std::uint32_t o_contact_ = 0;
    std::uint32_t o_dent_ = 0;
    std::uint32_t o_prev_ = 0;
    std::uint32_t o_skin_snap_ = 0;
    std::uint32_t dynamic_words_ = 0;

    // 空间哈希（grid_ssbo_）：
    //   [三角桶头][接触桶头][三角计数器, 接触计数器][三角入口池][接触入口池]
    // 桶头每帧清零；入口池只在插入时写入，桶头清零后旧入口不可达，无需清空。
    std::uint32_t tri_head_off_ = 0;
    std::uint32_t tri_head_count_ = 1;
    std::uint32_t contact_head_off_ = 0;
    std::uint32_t contact_head_count_ = 1;
    std::uint32_t counter_off_ = 0;
    std::uint32_t tri_pool_off_ = 0;
    std::uint32_t tri_pool_cap_ = 0;
    std::uint32_t contact_pool_off_ = 0;
    std::uint32_t contact_pool_cap_ = 0;
    std::uint32_t grid_words_ = 0;

    ShaderProgram skin_snapshot_;
    ShaderProgram weld_prepare_;
    ShaderProgram tri_grid_;
    ShaderProgram contact_;
    ShaderProgram contact_grid_;
    ShaderProgram taper_;
    ShaderProgram bulge_;
    ShaderProgram solve_;
    ShaderProgram weld_normal_;
    ShaderProgram finalize_;
    ShaderProgram smooth_;

    bool prev_valid_ = false;
    float prev_frame_ = -1e9f;
    std::uint64_t prev_key_ = 0;
    int dispatch_count_ = 0;
};

}  // namespace pmx
