#include "app/profiler.hpp"

#include <chrono>

namespace pmx {

namespace {

double now_ms() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}

}  // namespace

Profiler::Profiler() = default;

Profiler& Profiler::instance() {
    static Profiler profiler;
    return profiler;
}

void Profiler::begin(ProfileScope scope) {
    Entry& e = entries_[static_cast<std::size_t>(scope)];
    if (e.depth++ == 0) e.started_ms = now_ms();
}

void Profiler::end(ProfileScope scope) {
    Entry& e = entries_[static_cast<std::size_t>(scope)];
    if (e.depth <= 0) return;
    if (--e.depth == 0) e.accumulated_ms += now_ms() - e.started_ms;
}

void Profiler::tick() {
    // 指数滑动平均：既能看清趋势，又不会被单帧抖动带偏。
    constexpr float kAlpha = 0.1f;
    for (std::size_t i = 0; i < static_cast<std::size_t>(ProfileScope::Count); ++i) {
        Entry& e = entries_[i];
        if (e.depth == 0) {
            e.smoothed += (static_cast<float>(e.accumulated_ms) - e.smoothed) * kAlpha;
            e.accumulated_ms = 0.0;
        }
    }
}

float Profiler::milliseconds(ProfileScope scope) const {
    return entries_[static_cast<std::size_t>(scope)].smoothed;
}

const char* Profiler::name(ProfileScope scope) const {
    switch (scope) {
        case ProfileScope::Frame: return "单帧";
        case ProfileScope::Physics: return "物理";
        case ProfileScope::Ik: return "IK";
        case ProfileScope::Skin: return "蒙皮";
        case ProfileScope::Ui: return "界面";
        case ProfileScope::Io: return "IO";
        default: return "?";
    }
}

}  // namespace pmx
