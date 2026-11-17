// 逐帧导出任务：每帧渲染一张 PNG，完成后可选调用 ffmpeg 编码。
//
// 渲染必须在 GL 线程上做，所以任务不另开线程：主循环每帧推进一帧，
// 期间弹一个不可交互的进度窗。编码阶段是阻塞的（一次调用）。
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "app/export.hpp"
#include "gpu/offscreen.hpp"

struct Viewer;

struct ExportJob {
    pmx::ExportSettings settings;
    pmx::ExportProgress progress;
    std::unique_ptr<pmx::OffscreenTarget> target;
    std::vector<std::uint8_t> rgba;

    bool active = false;
    bool finished = false;
    bool failed = false;
    bool encoding = false;
    int current = 0;
    int written = 0;
    int total = 0;
    std::string error;
    std::string log;
    // 物理按动画时间推进：记录上一帧，帧步进大于 1 时也保持连续。
    int physics_frame = 0;
};

void export_job_start(Viewer& v);   // 用 v.output 与时间线范围启动
void export_job_tick(Viewer& v);    // 主循环每帧调用，推进一帧
void export_job_cancel(Viewer& v);
void export_job_draw(Viewer& v);    // 进度窗口
