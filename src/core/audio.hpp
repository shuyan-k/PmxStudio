// 音频轨：WAV / MP3 解码、播放与波形缩略图。
//
// Windows 走 miniaudio 设备播放；其他平台只解码（用于波形与时长），播放为静音时钟。
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace pmx {

class AudioTrack {
public:
    AudioTrack();
    ~AudioTrack();
    AudioTrack(const AudioTrack&) = delete;
    AudioTrack& operator=(const AudioTrack&) = delete;

    bool load(const std::string& path, std::string* error);
    void unload();
    bool loaded() const;

    double duration() const;   // 秒
    int sample_rate() const;
    int channels() const;

    void play(double offset_seconds);
    void stop();
    bool playing() const;
    double position() const;   // 秒

    // 波形：每 bin 的峰值（0..1），已按固定数量降采样，供 UI 绘制。
    const std::vector<float>& peaks() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pmx
