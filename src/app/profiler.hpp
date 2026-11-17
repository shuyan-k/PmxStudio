// 轻量区间计时：物理 / IK / 蒙皮 / UI / 单帧总耗时。
// 只做滑动平均，不分配、不加锁，可在每帧多次调用。
#pragma once

#include <cstddef>

namespace pmx {

enum class ProfileScope {
    Frame = 0,
    Physics,
    Ik,
    Skin,
    Ui,
    Io,
    Count,
};

class Profiler {
public:
    static Profiler& instance();

    void begin(ProfileScope scope);
    void end(ProfileScope scope);
    // 每帧末尾调用：推进滑动平均并清零当帧累加。
    void tick();

    float milliseconds(ProfileScope scope) const;
    const char* name(ProfileScope scope) const;

private:
    Profiler();
    struct Entry {
        double accumulated_ms = 0.0;
        double started_ms = 0.0;
        int depth = 0;
        float smoothed = 0.0f;
    };
    Entry entries_[static_cast<std::size_t>(ProfileScope::Count)];
};

// 作用域计时；同一作用域可嵌套，只有最外层累加。
class ProfileTimer {
public:
    explicit ProfileTimer(ProfileScope scope) : scope_(scope) {
        Profiler::instance().begin(scope_);
    }
    ~ProfileTimer() { Profiler::instance().end(scope_); }
    ProfileTimer(const ProfileTimer&) = delete;
    ProfileTimer& operator=(const ProfileTimer&) = delete;

private:
    ProfileScope scope_;
};

}  // namespace pmx
