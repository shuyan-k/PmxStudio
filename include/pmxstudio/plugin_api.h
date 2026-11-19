// PmxStudio 插件 ABI v1。
//
// 插件是一个动态库（Windows .dll / Linux .so / macOS .dylib），导出
// `pmx_plugin_init`，可选导出 `pmx_plugin_shutdown`。宿主在启动时扫描
// 可执行文件旁的 plugins/ 目录与 PMX_PLUGIN_PATH，加载并调用 init。
//
// 设计约束：
//   * 只使用 C ABI 与固定缓冲区，字符串不跨模块释放，避免 CRT / 运行时不一致；
//   * 插件命令名必须以 "plugin." 开头，通过 Agent API（JSON-RPC）直接调用；
//   * 插件可通过 host->invoke 调用宿主内置命令（如 model.info / bone.pose），
//     借此操作 PmxStudio 的全部能力，而无需直接链宿主代码。
//
// 编译示例见 examples/plugins/demo。
#pragma once

#include <stddef.h>

#ifdef _WIN32
#define PMX_PLUGIN_EXPORT __declspec(dllexport)
#define PMX_PLUGIN_CALL __cdecl
#else
#define PMX_PLUGIN_EXPORT __attribute__((visibility("default")))
#define PMX_PLUGIN_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define PMX_PLUGIN_ABI_VERSION 1
// 命令输出缓冲区上限（含结尾 0）。宿主按此大小分配，插件不要假设更大。
// 超出时宿主按失败处理，返回 JSON 错误。
#define PMX_PLUGIN_MAX_OUTPUT 65536

// 日志级别：0 信息 / 1 警告 / 2 错误。
typedef void (PMX_PLUGIN_CALL *PmxLogFn)(void* host_user, int level, const char* message);

// 插件命令处理函数。
//   params_json：UTF-8 JSON 文本（至少是 "{}"）
//   out/out_size：写入 UTF-8 JSON 结果（需以 '\0' 结尾），上限 PMX_PLUGIN_MAX_OUTPUT
// 返回 0 表示成功；非 0 时宿主把 out 里的内容（或通用信息）当作错误消息。
typedef int (PMX_PLUGIN_CALL *PmxCommandFn)(void* command_user, const char* params_json,
                                            char* out, int out_size);

typedef struct PmxHostV1 {
    // 必须等于 PMX_PLUGIN_ABI_VERSION，插件应在 init 里校验。
    int abi_version;
    void* user;  // 宿主内部指针，传给下面的回调
    PmxLogFn log;
    // 注册命令。name 必须以 "plugin." 开头（例如 "plugin.demo.echo"）。
    // 返回 0 成功，非 0 失败（名称非法 / 重复 / 插件已卸载）。
    int (PMX_PLUGIN_CALL *register_command)(void* host_user, const char* name,
                                            const char* summary, PmxCommandFn fn,
                                            void* command_user);
    // 调用宿主内置命令（Agent API 的同一套方法）。
    // 禁止调用 "plugin." 前缀的命令，避免递归。返回 0 成功；结果写入 out。
    int (PMX_PLUGIN_CALL *invoke)(void* host_user, const char* method, const char* params_json,
                                  char* out, int out_size);
} PmxHostV1;

typedef int (PMX_PLUGIN_CALL *PmxPluginInitFn)(const PmxHostV1* host);
typedef void (PMX_PLUGIN_CALL *PmxPluginShutdownFn)(void);

#ifdef __cplusplus
}  // extern "C"
#endif
