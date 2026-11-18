#include "app/export_job.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include <imgui.h>

#include "app/modules.hpp"
#include "app/viewer.hpp"
#include "core/anim.hpp"
#include "core/bake.hpp"
#include "gpu/offscreen.hpp"

#ifdef PMX_ENABLE_PHYSICS
#include "physics/physics.hpp"
#endif

namespace {

void cleanup_target(ExportJob& job) {
    job.target.reset();
    job.rgba.clear();
}

}  // namespace

void export_job_start(Viewer& v) {
    ExportJob& job = v.export_job;
    if (job.active) return;
    if (!v.pose || v.model.bones.empty()) {
        v.status = "先载入模型再导出";
        return;
    }

    // 场景长度取模型动作与相机动作的较长者。
    const int scene_frames = timeline_max_frame(v);
    const int max_frame = scene_frames > 0 ? scene_frames : 120;
    job.settings.width = std::clamp(v.output.width, 16, 8192);
    job.settings.height = std::clamp(v.output.height, 16, 8192);
    job.settings.supersample = std::clamp(v.output.supersample, 1, 4);
    job.settings.begin_frame = std::clamp(v.output.begin, 0, max_frame);
    job.settings.end_frame = std::clamp(v.output.end, job.settings.begin_frame, max_frame);
    job.settings.step = std::max(1, v.output.step);
    job.settings.fps = 30;
    job.settings.directory = v.output.directory;
    job.settings.video_path = v.output.video_path;
    job.settings.audio_path = v.audio_path;
    job.settings.crf = std::clamp(v.output.crf, 0, 51);

    const int w = job.settings.width * job.settings.supersample;
    const int h = job.settings.height * job.settings.supersample;
    job.target = std::make_unique<pmx::OffscreenTarget>();
    std::string error;
    if (!job.target->resize(w, h, job.settings.supersample, &error)) {
        v.status = "创建离屏渲染目标失败：" + error;
        cleanup_target(job);
        return;
    }

    job.active = true;
    job.finished = false;
    job.failed = false;
    job.encoding = false;
    job.current = job.settings.begin_frame;
    job.physics_frame = job.settings.begin_frame;
    job.written = 0;
    job.total = (job.settings.end_frame - job.settings.begin_frame) / job.settings.step + 1;
    job.error.clear();
    job.log.clear();

    // 起始帧复位物理，保证每次导出从同一状态出发。
#ifdef PMX_ENABLE_PHYSICS
    if (v.physics_ptr && v.physics_ptr->enabled) {
        const bool playing = v.motion_playing;
        v.motion_playing = false;
        if (v.timeline.clip) {
            pmx::MorphWeights weights;
            pmx::evaluate_clip(*v.timeline.clip, job.settings.begin_frame, *v.pose, &weights);
            v.physics_ptr->reset(*v.pose, &weights, true);
        }
        v.motion_playing = playing;
    }
#endif
    v.motion_playing = false;
    v.status = "开始导出 " + std::to_string(job.total) + " 帧";
}

void export_job_tick(Viewer& v) {
    ExportJob& job = v.export_job;
    if (!job.active || !v.pose) return;

    const double saved_frame = v.motion_frame;
    const int w = job.settings.width * job.settings.supersample;
    const int h = job.settings.height * job.settings.supersample;

    pmx::MorphWeights weights;
    if (v.timeline.clip)
        pmx::evaluate_clip(*v.timeline.clip, job.current, *v.pose, &weights);
    else if (v.motion)
        pmx::evaluate_vmd(*v.motion, v.motion_binding, job.current, *v.pose, &weights);
    v.pose->solve_ik();

#ifdef PMX_ENABLE_PHYSICS
    if (v.physics_ptr && v.physics_ptr->enabled) {
        pmx::advance_physics_to(*v.physics_ptr, *v.pose, &weights, job.physics_frame,
                                job.current);
    }
#endif
    job.physics_frame = job.current;
    v.pose->world();

    // 场景相机：有相机动作就按相机轨渲染（与视口是否处于相机视角无关）。
    pmx::OrbitCamera camera = v.camera;
    v.camera_motion_orbit_at(static_cast<double>(job.current), camera);

    pmx::RenderSettings settings = v.settings;
    settings.show_mesh = true;
    settings.show_bones = false;
    settings.show_rigid = false;
    settings.show_joints = false;
    settings.grid = false;
    settings.focus_selection = false;
    // 导出只保留模型本身：光源罗盘是视口辅助，进画面会挡主体。
    settings.show_light = false;
    // 边缘光会在剪影内侧加一圈提亮，削掉贴图细节；导出按 MMD 原版（无边缘光）出图。
    settings.rim = 0.0f;

    const float aspect = static_cast<float>(w) / static_cast<float>(std::max(1, h));
    job.target->bind();
    v.renderer.render(v.model, *v.pose, nullptr, camera.view(), camera.projection(aspect), w,
                      h, settings, pmx::Selection{});
    const bool read_ok = job.target->read_rgba(w, h, job.rgba, &job.error);
    job.target->unbind();

    if (!read_ok) {
        job.failed = true;
        job.active = false;
        v.status = "回读像素失败：" + job.error;
        cleanup_target(job);
        return;
    }

    const std::string path = pmx::frame_file_path(job.settings, job.current);
    if (!pmx::write_png(path, w, h, job.rgba, &job.error)) {
        job.failed = true;
        job.active = false;
        v.status = "写出 PNG 失败：" + job.error;
        cleanup_target(job);
        return;
    }
    ++job.written;
    job.progress.frame = job.current;
    job.progress.total = job.total;

    v.motion_frame = static_cast<float>(saved_frame);

    job.current += job.settings.step;
    if (job.current > job.settings.end_frame) {
        job.active = false;
        job.finished = true;
        cleanup_target(job);
        if (!job.settings.video_path.empty()) {
            job.encoding = true;
            std::string log;
            if (pmx::encode_video(job.settings, &job.error, &log)) {
                v.status = "导出完成：" + job.settings.video_path;
            } else {
                job.failed = true;
                v.status = "视频编码失败：" + job.error;
            }
            job.log = log;
            job.encoding = false;
        } else {
            v.status = "序列帧导出完成：" + std::to_string(job.written) + " 张";
        }
    }
}

void export_job_cancel(Viewer& v) {
    ExportJob& job = v.export_job;
    if (!job.active) return;
    job.active = false;
    job.finished = false;
    cleanup_target(job);
    v.status = "已取消导出";
}

void export_job_draw(Viewer& v) {
    ExportJob& job = v.export_job;
    if (!job.active && !job.finished && !job.failed) return;
    if (!job.active && job.finished && job.error.empty()) {
        // 完成提示由状态栏承担，不额外弹窗。
        return;
    }

    const char* title = "渲染输出###pmx_export_progress";
    if (!ImGui::IsPopupOpen(title)) ImGui::OpenPopup(title);
    ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(title, nullptr,
                               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("导出中：第 %d / %d 帧", job.written, std::max(1, job.total));
        const float fraction = job.total > 0 ? static_cast<float>(job.written) / job.total : 0.0f;
        ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f));
        if (job.encoding) ImGui::TextDisabled("FFmpeg 编码中…");
        if (!job.error.empty()) ImGui::TextWrapped("错误：%s", job.error.c_str());
        if (job.active) {
            if (ImGui::Button("取消")) {
                export_job_cancel(v);
                ImGui::CloseCurrentPopup();
            }
        } else if (ImGui::Button("关闭")) {
            job.failed = false;
            job.error.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
