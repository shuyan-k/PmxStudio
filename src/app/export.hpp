// 序列帧与视频导出：离屏渲染 -> PNG -> 可选 FFmpeg 编码。
//
// 渲染由调用方通过 RenderFrameFn 提供，导出层只负责命名、写盘与调用 ffmpeg，
// 因此不依赖 Viewer / GL，可在 CLI 与 GUI 之间复用。
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pmx {

struct ExportSettings {
    int width = 1920;
    int height = 1080;
    int supersample = 2;  // 1 / 2 / 4，渲染分辨率 = width*supersample
    int begin_frame = 0;
    int end_frame = 0;
    int step = 1;
    int fps = 30;
    std::string directory;   // 序列帧目录；空则使用当前目录下的 frames/
    std::string prefix = "frame_";
    std::string video_path;  // 非空则编码 mp4
    std::string audio_path;  // 可选音轨
    int crf = 18;
};

struct ExportProgress {
    int frame = 0;
    int total = 0;
    bool done = false;
    bool cancel_requested = false;
    std::string message;
};

// frame 为 VMD 帧号；回调把该帧渲染成自上而下的 RGBA8（width*height*4）。
using RenderFrameFn =
    std::function<bool(int frame, int width, int height, std::vector<std::uint8_t>& rgba,
                       std::string* error)>;

// 写 PNG 序列帧。返回写出的文件数，失败返回 -1。
int export_frames(const ExportSettings& settings, const RenderFrameFn& render,
                  ExportProgress* progress, std::string* error);

// 写单张 RGBA8 PNG（自上而下）。供逐帧导出的状态机复用。
bool write_png(const std::string& path, int width, int height,
               const std::vector<std::uint8_t>& rgba, std::string* error);

// 序列帧文件路径（目录 + prefix + 4 位帧号）。
std::string frame_file_path(const ExportSettings& settings, int frame);

// 在 PATH / 常见安装目录里查找 ffmpeg；找到返回 true 并写 full_path。
bool find_ffmpeg(std::string* full_path);

// 用序列帧编码视频（ffmpeg 同步调用）。日志写入 log。
bool encode_video(const ExportSettings& settings, std::string* error, std::string* log);

// 探测 ffmpeg 版本（失败返回空字符串）。
std::string ffmpeg_version();

}  // namespace pmx
