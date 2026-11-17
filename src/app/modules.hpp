// 模块开关与工作区预设。
//
// 编译期用 CMake 的 PMX_MODULE_* 控制是否编入；运行期只控制 UI 可见性，
// 不改变数据。默认全部编入，保持单文件分发。
#pragma once

namespace pmx {

struct Modules {
#ifdef PMX_MODULE_TIMELINE
    bool timeline = true;
#else
    bool timeline = false;
#endif
#ifdef PMX_MODULE_BAKE
    bool bake = true;
#else
    bool bake = false;
#endif
#ifdef PMX_MODULE_RENDER_OUT
    bool render_output = true;
#else
    bool render_output = false;
#endif
#ifdef PMX_MODULE_AUDIO
    bool audio = true;
#else
    bool audio = false;
#endif
#ifdef PMX_MODULE_SCRIPT
    bool script = true;
#else
    bool script = false;
#endif
};

inline const Modules& modules() {
    static const Modules m;
    return m;
}

enum class Workspace { Edit, Animate, Output };

inline const char* workspace_name(Workspace w) {
    switch (w) {
        case Workspace::Edit: return "编辑";
        case Workspace::Animate: return "动画";
        case Workspace::Output: return "输出";
    }
    return "编辑";
}

}  // namespace pmx
