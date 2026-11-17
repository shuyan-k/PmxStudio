#include "core/audio.hpp"

// miniaudio 单头文件实现：整个工程只在本编译单元定义一次。
#define MINIAUDIO_IMPLEMENTATION
#ifndef _WIN32
// 本项目的非 Windows 构建未链接 ALSA / PulseAudio / JACK，也没有 GUI 设备需求：
// 只保留解码器用于波形与时长，播放退化为按时间推进的静音时钟。
#define MA_NO_ALSA
#define MA_NO_PULSEAUDIO
#define MA_NO_JACK
#define MA_NO_DEVICE_IO
#endif
#include <miniaudio.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace pmx {
namespace {

constexpr std::size_t kPeakCount = 1024;
constexpr ma_uint64 kDecodeChunkFrames = 4096;

#ifdef _WIN32
std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int len = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                          static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(),
                          len);
    return out;
}
#endif

std::string result_text(ma_result result) {
    const char* text = ma_result_description(result);
    return text != nullptr ? text : "未知错误";
}

}  // namespace

struct AudioTrack::Impl {
    bool loaded = false;
    double duration = 0.0;
    int sample_rate = 0;
    int channels = 0;
    std::vector<float> peaks;

    bool playing = false;
    double position = 0.0;
    double play_offset = 0.0;
    std::chrono::steady_clock::time_point play_start{};

#ifdef _WIN32
    ma_engine engine{};
    bool engine_ready = false;
    ma_sound sound{};
    bool sound_ready = false;
#endif

    // 每帧刷新播放位置：Windows 读 ma_sound 游标，其余平台推进静音时钟。
    void refresh() {
#ifdef _WIN32
        if (!sound_ready) return;
        float cursor = 0.0f;
        if (ma_sound_get_cursor_in_seconds(&sound, &cursor) == MA_SUCCESS) {
            position = static_cast<double>(cursor);
        }
        playing = ma_sound_is_playing(&sound) && !ma_sound_at_end(&sound);
#else
        if (!playing) return;
        const auto now = std::chrono::steady_clock::now();
        const double elapsed =
            std::chrono::duration<double>(now - play_start).count();
        position = play_offset + elapsed;
        if (position >= duration) {
            position = duration;
            playing = false;
        }
#endif
    }
};

AudioTrack::AudioTrack() : impl_(std::make_unique<Impl>()) {}

AudioTrack::~AudioTrack() { unload(); }

bool AudioTrack::load(const std::string& path, std::string* error) {
    std::string scratch;
    std::string* err = error ? error : &scratch;
    err->clear();

    if (impl_ == nullptr) impl_ = std::make_unique<Impl>();
    unload();

    if (path.empty()) {
        *err = "音频路径为空";
        return false;
    }

    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_decoder decoder{};
    ma_result result = MA_ERROR;
#ifdef _WIN32
    const std::wstring wide_path = utf8_to_wide(path);
    if (wide_path.empty()) {
        *err = "无法转换音频路径";
        return false;
    }
    result = ma_decoder_init_file_w(wide_path.c_str(), &config, &decoder);
#else
    result = ma_decoder_init_file(path.c_str(), &config, &decoder);
#endif
    if (result != MA_SUCCESS) {
        *err = "无法打开音频：" + result_text(result);
        return false;
    }

    const int channels = static_cast<int>(decoder.outputChannels);
    const int sample_rate = static_cast<int>(decoder.outputSampleRate);
    if (channels <= 0 || sample_rate <= 0) {
        ma_decoder_uninit(&decoder);
        *err = "音频通道或采样率无效";
        return false;
    }

    ma_uint64 total_frames = 0;
    const bool length_known =
        ma_decoder_get_length_in_pcm_frames(&decoder, &total_frames) == MA_SUCCESS &&
        total_frames > 0;

    std::vector<float> peaks(kPeakCount, 0.0f);
    std::vector<float> chunk(kDecodeChunkFrames * static_cast<std::size_t>(channels));

    if (length_known) {
        ma_uint64 frame_index = 0;
        for (;;) {
            ma_uint64 read = 0;
            const ma_result rr = ma_decoder_read_pcm_frames(
                &decoder, chunk.data(), kDecodeChunkFrames, &read);
            if (rr != MA_SUCCESS && rr != MA_AT_END) {
                ma_decoder_uninit(&decoder);
                *err = "解码音频失败：" + result_text(rr);
                return false;
            }
            if (read == 0) break;

            for (ma_uint64 f = 0; f < read; ++f) {
                const float* frame = chunk.data() + f * static_cast<ma_uint64>(channels);
                float peak = 0.0f;
                for (int c = 0; c < channels; ++c) {
                    peak = std::max(peak, std::fabs(frame[c]));
                }
                std::size_t bin = static_cast<std::size_t>(
                    (frame_index + f) * kPeakCount / total_frames);
                if (bin >= kPeakCount) bin = kPeakCount - 1;
                peaks[bin] = std::max(peaks[bin], peak);
            }
            frame_index += read;
            if (rr == MA_AT_END) break;
        }
        if (frame_index == 0) {
            ma_decoder_uninit(&decoder);
            *err = "音频不含可解码的采样";
            return false;
        }
        total_frames = frame_index;
    } else {
        // 长度未知（部分流式 MP3）：先按帧峰值收进单声道，再按总帧数分桶。
        std::vector<float> mono;
        for (;;) {
            ma_uint64 read = 0;
            const ma_result rr = ma_decoder_read_pcm_frames(
                &decoder, chunk.data(), kDecodeChunkFrames, &read);
            if (rr != MA_SUCCESS && rr != MA_AT_END) {
                ma_decoder_uninit(&decoder);
                *err = "解码音频失败：" + result_text(rr);
                return false;
            }
            if (read == 0) break;
            mono.reserve(mono.size() + static_cast<std::size_t>(read));
            for (ma_uint64 f = 0; f < read; ++f) {
                const float* frame = chunk.data() + f * static_cast<ma_uint64>(channels);
                float peak = 0.0f;
                for (int c = 0; c < channels; ++c) {
                    peak = std::max(peak, std::fabs(frame[c]));
                }
                mono.push_back(peak);
            }
            if (rr == MA_AT_END) break;
        }
        total_frames = static_cast<ma_uint64>(mono.size());
        if (total_frames > 0) {
            for (std::size_t bin = 0; bin < kPeakCount; ++bin) {
                const std::size_t begin =
                    bin * mono.size() / kPeakCount;
                std::size_t end = (bin + 1) * mono.size() / kPeakCount;
                if (end > mono.size()) end = mono.size();
                float peak = 0.0f;
                for (std::size_t i = begin; i < end; ++i) {
                    peak = std::max(peak, mono[i]);
                }
                peaks[bin] = peak;
            }
        }
    }
    ma_decoder_uninit(&decoder);

    if (total_frames == 0) {
        *err = "音频不含可解码的采样";
        return false;
    }

    for (float& value : peaks) {
        value = std::min(1.0f, std::max(0.0f, value));
    }

#ifdef _WIN32
    result = ma_engine_init(nullptr, &impl_->engine);
    if (result != MA_SUCCESS) {
        *err = "音频设备初始化失败：" + result_text(result);
        return false;
    }
    impl_->engine_ready = true;

    const std::wstring sound_path = utf8_to_wide(path);
    result = ma_sound_init_from_file_w(&impl_->engine, sound_path.c_str(),
                                       MA_SOUND_FLAG_DECODE, nullptr, nullptr,
                                       &impl_->sound);
    if (result != MA_SUCCESS) {
        ma_engine_uninit(&impl_->engine);
        impl_->engine_ready = false;
        *err = "无法加载音频播放源：" + result_text(result);
        return false;
    }
    impl_->sound_ready = true;
#endif

    impl_->duration =
        static_cast<double>(total_frames) / static_cast<double>(sample_rate);
    impl_->sample_rate = sample_rate;
    impl_->channels = channels;
    impl_->peaks = std::move(peaks);
    impl_->playing = false;
    impl_->position = 0.0;
    impl_->play_offset = 0.0;
    impl_->loaded = true;
    return true;
}

void AudioTrack::unload() {
    if (impl_ == nullptr) return;
#ifdef _WIN32
    if (impl_->sound_ready) {
        ma_sound_stop(&impl_->sound);
        ma_sound_uninit(&impl_->sound);
        impl_->sound_ready = false;
    }
    if (impl_->engine_ready) {
        ma_engine_uninit(&impl_->engine);
        impl_->engine_ready = false;
    }
#endif
    impl_->loaded = false;
    impl_->duration = 0.0;
    impl_->sample_rate = 0;
    impl_->channels = 0;
    impl_->peaks.clear();
    impl_->playing = false;
    impl_->position = 0.0;
    impl_->play_offset = 0.0;
}

bool AudioTrack::loaded() const { return impl_ != nullptr && impl_->loaded; }

double AudioTrack::duration() const {
    return impl_ != nullptr ? impl_->duration : 0.0;
}

int AudioTrack::sample_rate() const {
    return impl_ != nullptr ? impl_->sample_rate : 0;
}

int AudioTrack::channels() const {
    return impl_ != nullptr ? impl_->channels : 0;
}

void AudioTrack::play(double offset_seconds) {
    if (impl_ == nullptr || !impl_->loaded) return;

    double start = offset_seconds;
    if (start < 0.0) start = 0.0;
    if (start > impl_->duration) start = impl_->duration;

#ifdef _WIN32
    if (!impl_->sound_ready) return;
    ma_sound_stop(&impl_->sound);
    ma_sound_seek_to_second(&impl_->sound, static_cast<float>(start));
    ma_sound_start(&impl_->sound);
    impl_->position = start;
    impl_->playing = start < impl_->duration;
#else
    impl_->play_offset = start;
    impl_->position = start;
    impl_->play_start = std::chrono::steady_clock::now();
    impl_->playing = start < impl_->duration;
#endif
}

void AudioTrack::stop() {
    if (impl_ == nullptr) return;
#ifdef _WIN32
    if (impl_->sound_ready) {
        ma_sound_stop(&impl_->sound);
        ma_sound_seek_to_pcm_frame(&impl_->sound, 0);
    }
#endif
    impl_->playing = false;
    impl_->position = 0.0;
    impl_->play_offset = 0.0;
}

bool AudioTrack::playing() const {
    if (impl_ == nullptr || !impl_->loaded) return false;
    impl_->refresh();
    return impl_->playing;
}

double AudioTrack::position() const {
    if (impl_ == nullptr || !impl_->loaded) return 0.0;
    impl_->refresh();
    return impl_->position;
}

const std::vector<float>& AudioTrack::peaks() const {
    static const std::vector<float> kEmpty;
    return impl_ != nullptr ? impl_->peaks : kEmpty;
}

}  // namespace pmx
