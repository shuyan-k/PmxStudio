// 第三方插件宿主：扫描动态库、维护命令表并分发调用。
//
// 插件 ABI 见 include/pmxstudio/plugin_api.h。插件注册的命令名必须以
// "plugin." 开头，Agent API 与（未来）UI 都通过同一张表调用，宿主不需要
// 了解插件内部实现。
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/json.hpp"
#include "pmxstudio/plugin_api.h"

struct PluginCommand {
    std::string name;
    std::string summary;
    std::string plugin;
    PmxCommandFn fn = nullptr;
    void* user = nullptr;
};

class PluginManager {
public:
    // 插件通过 host->invoke 调用宿主内置命令；由 main 注入 api_execute。
    using HostInvoke =
        std::function<bool(const std::string& method, const std::string& params_json,
                           std::string& result_json, std::string& error)>;

    PluginManager();
    ~PluginManager();
    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    void set_host_invoke(HostInvoke invoke);

    // 扫描可执行文件旁的 plugins/、当前目录的 plugins/ 与 PMX_PLUGIN_PATH。
    // 返回成功加载的插件数量（失败项记录在 errors()）。
    int load_all();
    bool load_plugin(const std::string& path, std::string* error);
    void unload_all();

    const std::vector<PluginCommand>& commands() const { return commands_; }
    const std::vector<std::string>& plugin_names() const { return plugin_names_; }
    const std::vector<std::string>& errors() const { return errors_; }
    const std::vector<std::string>& log_lines() const { return log_lines_; }
    std::vector<std::string> search_paths() const;

    bool invoke(const std::string& name, const std::string& params_json,
                std::string& result_json, std::string& error);

    // 供 system.info / plugin.list 返回的状态描述。
    pmx::JsonValue describe() const;

    void log(int level, const std::string& message);

private:
    struct LoadedPlugin;

    static void PMX_PLUGIN_CALL host_log(void* user, int level, const char* message);
    static int PMX_PLUGIN_CALL host_register(void* user, const char* name, const char* summary,
                                             PmxCommandFn fn, void* command_user);
    static int PMX_PLUGIN_CALL host_invoke_cb(void* user, const char* method,
                                              const char* params_json, char* out, int out_size);

    bool register_command(const std::string& plugin, const char* name, const char* summary,
                          PmxCommandFn fn, void* command_user);
    void remove_plugin_commands(const std::string& plugin);

    std::vector<std::unique_ptr<LoadedPlugin>> loaded_;
    std::vector<PluginCommand> commands_;
    std::vector<std::string> plugin_names_;
    std::vector<std::string> errors_;
    std::vector<std::string> log_lines_;
    HostInvoke host_invoke_;
};
