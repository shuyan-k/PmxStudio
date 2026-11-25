#include "app/plugin.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <utility>

#include "core/fs.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace {

#ifdef _WIN32
using LibHandle = HMODULE;
#else
using LibHandle = void*;
#endif

LibHandle open_library(const std::string& path) {
#ifdef _WIN32
    return LoadLibraryW(pmx::utf8_path(path).c_str());
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void close_library(LibHandle handle) {
#ifdef _WIN32
    FreeLibrary(handle);
#else
    dlclose(handle);
#endif
}

std::string library_error() {
#ifdef _WIN32
    return "错误码 " + std::to_string(static_cast<unsigned long>(GetLastError()));
#else
    const char* message = dlerror();
    return message ? message : "未知错误";
#endif
}

template <typename Fn>
Fn load_symbol(LibHandle handle, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<Fn>(GetProcAddress(handle, name));
#else
    return reinterpret_cast<Fn>(dlsym(handle, name));
#endif
}

bool copy_out(const std::string& text, char* out, int out_size) {
    if (!out || out_size <= 0) return false;
    const std::size_t limit = static_cast<std::size_t>(out_size) - 1;
    const std::size_t n = std::min(text.size(), limit);
    std::memcpy(out, text.data(), n);
    out[n] = '\0';
    return n == text.size();
}

std::string executable_dir() {
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    while (true) {
        const DWORD len =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (len == 0) break;
        if (len < buffer.size()) {
            buffer.resize(len);
            return pmx::utf8_string(std::filesystem::path(buffer).parent_path());
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    char buffer[4096];
    std::uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) == 0) {
        return pmx::utf8_string(std::filesystem::path(buffer).parent_path());
    }
#else
    char buffer[4096];
    const ssize_t len = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len > 0) {
        buffer[len] = '\0';
        return pmx::utf8_string(std::filesystem::path(buffer).parent_path());
    }
#endif
    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    return ec ? std::string(".") : pmx::utf8_string(cwd);
}

std::filesystem::path canonical_or_self(const std::filesystem::path& path) {
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(path, ec);
    return ec ? path : canonical;
}

bool is_plugin_file(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(c | 0x20); });
#ifdef _WIN32
    return ext == ".dll";
#elif defined(__APPLE__)
    return ext == ".dylib" || ext == ".so";
#else
    return ext == ".so";
#endif
}

}  // namespace

struct PluginManager::LoadedPlugin {
    PluginManager* manager = nullptr;
    std::string name;
    std::string path;
    LibHandle handle = nullptr;
    PmxHostV1 host{};
    PmxPluginShutdownFn shutdown = nullptr;
    bool initialized = false;
};

PluginManager::PluginManager() = default;

PluginManager::~PluginManager() { unload_all(); }

void PluginManager::set_host_invoke(HostInvoke invoke) { host_invoke_ = std::move(invoke); }

std::vector<std::string> PluginManager::search_paths() const {
    std::vector<std::filesystem::path> dirs;
    // executable_dir() 是 UTF-8；直接构造 path 会按 Windows ANSI 代码页转换，
    // 安装在中文/日文目录时会抛 filesystem_error。
    dirs.emplace_back(pmx::utf8_path(executable_dir()) / "plugins");
    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    if (!ec) dirs.emplace_back(cwd / "plugins");
#ifdef _WIN32
    constexpr char kSeparator = ';';
#else
    constexpr char kSeparator = ':';
#endif
    if (const char* extra = std::getenv("PMX_PLUGIN_PATH")) {
        std::string text(extra);
        std::size_t start = 0;
        while (start <= text.size()) {
            const std::size_t end = text.find(kSeparator, start);
            const std::string item =
                text.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!item.empty()) dirs.emplace_back(pmx::utf8_path(item));
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }

    std::vector<std::string> out;
    for (const auto& dir : dirs) {
        const std::string text = pmx::utf8_string(canonical_or_self(dir));
        if (std::find(out.begin(), out.end(), text) == out.end()) out.push_back(text);
    }
    return out;
}

int PluginManager::load_all() {
    int loaded = 0;
    std::vector<std::string> seen;
    for (const std::string& dir : search_paths()) {
        std::error_code ec;
        if (!std::filesystem::is_directory(pmx::utf8_path(dir), ec)) continue;
        std::vector<std::filesystem::path> files;
        for (std::filesystem::directory_iterator it(pmx::utf8_path(dir), ec), end; !ec && it != end;
             it.increment(ec)) {
            const auto& path = it->path();
            if (!path.has_extension() || !is_plugin_file(path)) continue;
            files.push_back(path);
        }
        std::sort(files.begin(), files.end());
        for (const auto& path : files) {
            const std::string full = pmx::utf8_string(path);
            const std::string key = pmx::utf8_string(canonical_or_self(path));
            if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
            seen.push_back(key);
            std::string error;
            if (load_plugin(full, &error)) {
                ++loaded;
            } else {
                const std::string line = full + "：" + error;
                errors_.push_back(line);
                log(2, line);
            }
        }
    }
    return loaded;
}

bool PluginManager::load_plugin(const std::string& path, std::string* error) {
    const std::string name = pmx::utf8_string(pmx::utf8_path(path).stem());
    if (std::find(plugin_names_.begin(), plugin_names_.end(), name) != plugin_names_.end()) {
        if (error) *error = "同名插件已加载";
        return false;
    }

    LibHandle handle = open_library(path);
    if (!handle) {
        if (error) *error = "无法加载动态库（" + library_error() + "）";
        return false;
    }
    auto init = load_symbol<PmxPluginInitFn>(handle, "pmx_plugin_init");
    if (!init) {
        close_library(handle);
        if (error) *error = "没有导出 pmx_plugin_init";
        return false;
    }

    auto plugin = std::make_unique<LoadedPlugin>();
    plugin->manager = this;
    plugin->name = name;
    plugin->path = path;
    plugin->handle = handle;
    plugin->host.abi_version = PMX_PLUGIN_ABI_VERSION;
    plugin->host.user = plugin.get();
    plugin->host.log = &PluginManager::host_log;
    plugin->host.register_command = &PluginManager::host_register;
    plugin->host.invoke = &PluginManager::host_invoke_cb;

    const int rc = init(&plugin->host);
    if (rc != 0) {
        remove_plugin_commands(name);
        close_library(handle);
        if (error) *error = "初始化失败（pmx_plugin_init 返回 " + std::to_string(rc) + "）";
        return false;
    }

    plugin->shutdown = load_symbol<PmxPluginShutdownFn>(handle, "pmx_plugin_shutdown");
    plugin->initialized = true;
    plugin_names_.push_back(name);
    loaded_.push_back(std::move(plugin));
    return true;
}

void PluginManager::unload_all() {
    for (auto it = loaded_.rbegin(); it != loaded_.rend(); ++it) {
        LoadedPlugin& plugin = **it;
        if (plugin.shutdown) plugin.shutdown();
        remove_plugin_commands(plugin.name);
        if (plugin.handle) close_library(plugin.handle);
        plugin.handle = nullptr;
        plugin.initialized = false;
    }
    loaded_.clear();
    plugin_names_.clear();
    commands_.clear();
}

void PluginManager::remove_plugin_commands(const std::string& plugin) {
    commands_.erase(std::remove_if(commands_.begin(), commands_.end(),
                                   [&plugin](const PluginCommand& command) {
                                       return command.plugin == plugin;
                                   }),
                    commands_.end());
}

bool PluginManager::register_command(const std::string& plugin, const char* name,
                                     const char* summary, PmxCommandFn fn, void* command_user) {
    if (!name || !*name || !fn) return false;
    const std::string command_name(name);
    if (command_name.rfind("plugin.", 0) != 0) {
        log(2, "插件 " + plugin + " 注册的命令名必须以 plugin. 开头：" + command_name);
        return false;
    }
    if (command_name.size() > 128) {
        log(2, "插件 " + plugin + " 的命令名过长：" + command_name);
        return false;
    }
    for (const PluginCommand& command : commands_) {
        if (command.name == command_name) {
            log(2, "插件 " + plugin + " 注册了重复命令：" + command_name);
            return false;
        }
    }
    PluginCommand command;
    command.name = command_name;
    command.summary = summary ? summary : "";
    command.plugin = plugin;
    command.fn = fn;
    command.user = command_user;
    commands_.push_back(std::move(command));
    return true;
}

bool PluginManager::invoke(const std::string& name, const std::string& params_json,
                           std::string& result_json, std::string& error) {
    const PluginCommand* command = nullptr;
    for (const PluginCommand& item : commands_) {
        if (item.name == name) {
            command = &item;
            break;
        }
    }
    if (!command) {
        error = "未知插件命令 \"" + name + "\"";
        return false;
    }

    std::vector<char> buffer(PMX_PLUGIN_MAX_OUTPUT, '\0');
    const int rc = command->fn(command->user, params_json.c_str(), buffer.data(),
                               static_cast<int>(buffer.size()));
    const std::string text(buffer.data(), std::strlen(buffer.data()));
    if (rc != 0) {
        error = text.empty() ? "插件命令失败（返回 " + std::to_string(rc) + "）" : text;
        return false;
    }
    if (text.empty()) {
        result_json = "null";
        return true;
    }
    try {
        (void)pmx::parse_json(text);
    } catch (const pmx::ParseError& parse_error) {
        error = "插件返回了非法 JSON（第 " + std::to_string(parse_error.pos) +
                " 字节）：" + parse_error.message;
        return false;
    }
    result_json = text;
    return true;
}

pmx::JsonValue PluginManager::describe() const {
    pmx::JsonValue root = pmx::json_object();

    pmx::JsonValue paths = pmx::json_array();
    for (const std::string& path : search_paths()) pmx::json_push(paths, pmx::json_string(path));
    pmx::json_set(root, "search_paths", std::move(paths));

    pmx::JsonValue plugins = pmx::json_array();
    for (const auto& plugin : loaded_) {
        pmx::JsonValue item = pmx::json_object();
        pmx::json_set(item, "name", pmx::json_string(plugin->name));
        pmx::json_set(item, "path", pmx::json_string(plugin->path));
        pmx::json_set(item, "initialized", pmx::json_bool(plugin->initialized));
        pmx::JsonValue names = pmx::json_array();
        for (const PluginCommand& command : commands_) {
            if (command.plugin == plugin->name) pmx::json_push(names, pmx::json_string(command.name));
        }
        pmx::json_set(item, "commands", std::move(names));
        pmx::json_push(plugins, std::move(item));
    }
    pmx::json_set(root, "plugins", std::move(plugins));

    pmx::JsonValue commands = pmx::json_array();
    for (const PluginCommand& command : commands_) {
        pmx::JsonValue item = pmx::json_object();
        pmx::json_set(item, "name", pmx::json_string(command.name));
        pmx::json_set(item, "summary", pmx::json_string(command.summary));
        pmx::json_set(item, "plugin", pmx::json_string(command.plugin));
        pmx::json_push(commands, std::move(item));
    }
    pmx::json_set(root, "commands", std::move(commands));

    pmx::JsonValue errors = pmx::json_array();
    for (const std::string& error : errors_) pmx::json_push(errors, pmx::json_string(error));
    pmx::json_set(root, "errors", std::move(errors));

    pmx::JsonValue logs = pmx::json_array();
    for (const std::string& line : log_lines_) pmx::json_push(logs, pmx::json_string(line));
    pmx::json_set(root, "log", std::move(logs));

    return root;
}

void PluginManager::log(int level, const std::string& message) {
    const char* tag = level == 2 ? "错误" : (level == 1 ? "警告" : "信息");
    const std::string line = std::string("[插件/") + tag + "] " + message;
    log_lines_.push_back(line);
    if (log_lines_.size() > 200) log_lines_.erase(log_lines_.begin());
    std::fprintf(stderr, "%s\n", line.c_str());
}

void PMX_PLUGIN_CALL PluginManager::host_log(void* user, int level, const char* message) {
    auto* plugin = static_cast<LoadedPlugin*>(user);
    if (!plugin || !plugin->manager) return;
    plugin->manager->log(level, plugin->name + "：" + (message ? message : ""));
}

int PMX_PLUGIN_CALL PluginManager::host_register(void* user, const char* name, const char* summary,
                                                 PmxCommandFn fn, void* command_user) {
    auto* plugin = static_cast<LoadedPlugin*>(user);
    if (!plugin || !plugin->manager) return -1;
    return plugin->manager->register_command(plugin->name, name, summary, fn, command_user) ? 0
                                                                                          : -1;
}

int PMX_PLUGIN_CALL PluginManager::host_invoke_cb(void* user, const char* method,
                                                  const char* params_json, char* out,
                                                  int out_size) {
    auto* plugin = static_cast<LoadedPlugin*>(user);
    if (!plugin || !plugin->manager || !method) return -1;
    const std::string name(method);
    // 插件不能调用插件命令：避免插件之间递归调用与加载顺序依赖。
    if (name.rfind("plugin.", 0) == 0) {
        copy_out("host.invoke 不允许调用 plugin.* 命令", out, out_size);
        return -1;
    }
    if (!plugin->manager->host_invoke_) {
        copy_out("宿主未提供内置命令分发", out, out_size);
        return -1;
    }

    std::string result;
    std::string error;
    if (!plugin->manager->host_invoke_(name, params_json ? params_json : "{}", result, error)) {
        copy_out(error.empty() ? "宿主命令失败" : error, out, out_size);
        return -1;
    }
    return copy_out(result, out, out_size) ? 0 : -1;
}
