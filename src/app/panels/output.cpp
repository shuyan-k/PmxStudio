// 输出页：渲染分辨率、PNG 序列帧、FFmpeg 编码与音频对轴。
//
// 导出由 ExportJob 在主循环里逐帧推进，界面不会整段卡死；只有最后的
// FFmpeg 编码是一次阻塞调用。
#include "app/panels.hpp"

#include <algorithm>
#include <string>

#include <imgui.h>

#include "app/export.hpp"
#include "app/export_job.hpp"
#include "app/modules.hpp"
#include "app/panels/common.hpp"
#include "core/audio.hpp"

void panel_output(Viewer& v) {
    if (!pmx::modules().render_output && !pmx::modules().audio) {
        ImGui::TextDisabled("当前构建未启用输出模块");
        return;
    }

    // ------------------------------------------------------------ 渲染
    if (pmx::modules().render_output) {
        ImGui::SeparatorText("渲染");
        ImGui::SetNextItemWidth(pmxui::scale(-96.0f));
        ImGui::DragInt("宽度", &v.output.width, 8.0f, 16, 8192);
        ImGui::SetNextItemWidth(pmxui::scale(-96.0f));
        ImGui::DragInt("高度", &v.output.height, 8.0f, 16, 8192);
        ImGui::SetNextItemWidth(pmxui::scale(-96.0f));
        ImGui::SliderInt("分辨率倍数", &v.output.supersample, 1, 4);
        ImGui::TextDisabled("实际输出 %d × %d", v.output.width * v.output.supersample,
                            v.output.height * v.output.supersample);

        ImGui::SeparatorText("范围");
        const int max_frame = std::max(0, timeline_max_frame(v));
        ImGui::TextDisabled(v.camera_motion.loaded()
                                ? "取景：相机动作（独立相机轨）"
                                : "取景：当前视口相机（没有相机动作）");
        ImGui::SetNextItemWidth(pmxui::scale(-96.0f));
        ImGui::DragInt("起始帧", &v.output.begin, 1.0f, 0, max_frame);
        ImGui::SetNextItemWidth(pmxui::scale(-96.0f));
        ImGui::DragInt("结束帧", &v.output.end, 1.0f, 0, max_frame);
        ImGui::SetNextItemWidth(pmxui::scale(-96.0f));
        ImGui::DragInt("帧步长", &v.output.step, 0.2f, 1, 30);
        ImGui::SetNextItemWidth(pmxui::scale(-96.0f));
        ImGui::DragInt("CRF", &v.output.crf, 0.5f, 0, 51);
        ImGui::TextDisabled("共 %d 帧 · %d fps",
                            std::max(0, (v.output.end - v.output.begin) / std::max(1, v.output.step) + 1),
                            30);

        ImGui::SeparatorText("路径");
        ImGui::SetNextItemWidth(pmxui::scale(-96.0f));
        ImGui::InputTextWithHint("序列帧目录", "空 = frames/", &v.output.directory);
        ImGui::SetNextItemWidth(pmxui::scale(-96.0f));
        ImGui::InputTextWithHint("视频文件", "空 = 只出序列帧", &v.output.video_path);

        std::string ffmpeg_path;
        if (pmx::find_ffmpeg(&ffmpeg_path)) {
            ImGui::TextDisabled("FFmpeg：%s", ffmpeg_path.c_str());
        } else {
            ImGui::TextDisabled("未找到 FFmpeg：只能导出序列帧。");
            if (ImGui::Button("复制 ffmpeg 命令说明")) {
                ImGui::SetClipboardText(
                    "安装 FFmpeg 并加入 PATH，或把 ffmpeg.exe 放到程序目录；"
                    "再设置「视频文件」即可直接编码 mp4。");
                v.status = "已复制说明";
            }
        }

        ImGui::Spacing();
        ImGui::BeginDisabled(v.export_job.active);
        if (ImGui::Button("开始导出", ImVec2(pmxui::scale(150.0f), 0.0f))) export_job_start(v);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!v.export_job.active);
        if (ImGui::Button("取消")) export_job_cancel(v);
        ImGui::EndDisabled();
        if (v.export_job.active) {
            const int total = std::max(1, v.export_job.total);
            ImGui::ProgressBar(static_cast<float>(v.export_job.written) / total,
                               ImVec2(-1.0f, 0.0f));
            ImGui::TextDisabled("第 %d / %d 帧", v.export_job.written, total);
        }
        if (!v.export_job.error.empty())
            ImGui::TextWrapped("错误：%s", v.export_job.error.c_str());
    }

    // ------------------------------------------------------------ 音频
    if (pmx::modules().audio) {
        ImGui::SeparatorText("音频");
        ImGui::SetNextItemWidth(pmxui::scale(-96.0f));
        ImGui::InputTextWithHint("音频文件", "WAV / MP3 路径", &v.audio_path);
        if (ImGui::Button("载入")) {
            if (v.audio_path.empty()) {
                v.status = "先填写音频路径";
            } else {
                if (!v.audio) v.audio = std::make_unique<pmx::AudioTrack>();
                std::string error;
                if (v.audio->load(v.audio_path, &error)) {
                    v.audio_offset = 0.0f;
                    v.status = "音频已载入：" + std::to_string(v.audio->duration()) + " 秒";
                } else {
                    v.status = "音频载入失败：" + error;
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("卸载")) {
            if (v.audio) v.audio->unload();
            v.status = "音频已卸载";
        }

        if (v.audio && v.audio->loaded()) {
            ImGui::Text("%.2f 秒 · %d Hz · %d 声道", v.audio->duration(),
                        v.audio->sample_rate(), v.audio->channels());
            const std::vector<float>& peaks = v.audio->peaks();
            if (!peaks.empty())
                ImGui::PlotLines("##peaks", peaks.data(), static_cast<int>(peaks.size()), 0,
                                 nullptr, 0.0f, 1.0f, ImVec2(-1.0f, pmxui::scale(48.0f)));

            ImGui::SetNextItemWidth(pmxui::scale(-96.0f));
            ImGui::SliderFloat("对齐偏移", &v.audio_offset, -0.5f, 0.5f, "%.2f 秒");
            if (ImGui::Button("随动作播放")) {
                v.audio->play(static_cast<double>(v.motion_frame) / 30.0 + v.audio_offset);
            }
            ImGui::SameLine();
            if (ImGui::Button("停止")) v.audio->stop();
            ImGui::SameLine();
            ImGui::TextDisabled("%.2f 秒", v.audio->position());
        }
        ImGui::TextDisabled("音频只作为对轴基准，不写入 VMD；导出视频时作为第二条输入。");
    }
}
