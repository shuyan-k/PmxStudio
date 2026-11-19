// Agent API：本地 TCP 上的 JSON-RPC 2.0 服务（换行分隔的单行 JSON）。
//
// 默认只监听 127.0.0.1，供脚本 / Agent / 外部工具直接操作查看器：
// 载入模型、摆骨骼、设形态、调相机、渲染截图、导入动作、执行批处理脚本等。
// 命令实现集中在 api_execute()，插件通过 host->invoke 复用同一套命令。
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "core/json.hpp"

struct Viewer;

// 执行一条内置命令。成功返回 true 并写入 result；失败返回 false 并写 error。
// error_code 可选，返回 JSON-RPC 错误码（默认 -32000，未知方法 -32601）。
bool api_execute(Viewer& viewer, const std::string& method, const pmx::JsonValue& params,
                 pmx::JsonValue& result, std::string& error, int* error_code = nullptr);

class ApiServer {
public:
    using PluginListFn = std::function<pmx::JsonValue()>;
    // 插件命令分发：method 为 "plugin." 开头的方法名，params 为序列化后的 JSON。
    using PluginInvokeFn =
        std::function<bool(const std::string& method, const std::string& params_json,
                           std::string& result_json, std::string& error)>;

    ApiServer();
    ~ApiServer();
    ApiServer(const ApiServer&) = delete;
    ApiServer& operator=(const ApiServer&) = delete;

    // 监听 127.0.0.1:port（port=0 时由系统分配）。失败写 error。
    bool start(int port, std::string* error);
    void stop();
    bool running() const;
    int port() const;

    void set_plugin_hooks(PluginListFn list, PluginInvokeFn invoke);

    // 主线程每帧调用：接收并执行请求，写回响应。
    void poll(Viewer& viewer);

    int requests() const;
    std::string last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
