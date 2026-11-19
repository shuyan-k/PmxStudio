// PmxStudio 示例插件：演示命令注册、日志与调用宿主内置命令。
//
// 构建（独立 CMake 工程）：
//   cmake -S examples/plugins/demo -B build-demo -DCMAKE_BUILD_TYPE=Release
//   cmake --build build-demo
// 生成 build-demo/demo.so（Windows 为 demo.dll），复制到
//   <可执行文件目录>/plugins/
// 或把所在目录加入 PMX_PLUGIN_PATH 即可被宿主扫描。
#include "pmxstudio/plugin_api.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

const PmxHostV1* g_host = nullptr;

int write_out(const std::string& text, char* out, int out_size) {
    if (!out || out_size <= 0) return 1;
    const std::size_t limit = static_cast<std::size_t>(out_size) - 1;
    const std::size_t n = text.size() < limit ? text.size() : limit;
    std::memcpy(out, text.data(), n);
    out[n] = '\0';
    return n == text.size() ? 0 : 1;
}

std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                                  static_cast<unsigned int>(static_cast<unsigned char>(c)));
                    out += buffer;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

// 极简演示用取值：只支持 {"key": "字符串"} 这一种形式，正式插件应使用
// 自己的 JSON 库或调用宿主命令做解析。
bool find_string(const char* json, const char* key, std::string& value) {
    if (!json) return false;
    const std::string needle = std::string("\"") + key + "\"";
    const char* pos = std::strstr(json, needle.c_str());
    if (!pos) return false;
    pos += needle.size();
    while (*pos == ' ' || *pos == '\t' || *pos == '\r' || *pos == '\n') ++pos;
    if (*pos != ':') return false;
    ++pos;
    while (*pos == ' ' || *pos == '\t' || *pos == '\r' || *pos == '\n') ++pos;
    if (*pos != '"') return false;
    ++pos;
    value.clear();
    while (*pos && *pos != '"') {
        if (*pos == '\\' && pos[1]) {
            ++pos;
            switch (*pos) {
                case 'n': value.push_back('\n'); break;
                case 'r': value.push_back('\r'); break;
                case 't': value.push_back('\t'); break;
                case 'b': value.push_back('\b'); break;
                case 'f': value.push_back('\f'); break;
                default: value.push_back(*pos); break;
            }
            ++pos;
        } else {
            value.push_back(*pos++);
        }
    }
    return *pos == '"';
}

void log_info(const std::string& message) {
    if (g_host && g_host->log) g_host->log(g_host->user, 0, message.c_str());
}

// plugin.demo.echo {message} -> {"echo": "..."}
int PMX_PLUGIN_CALL cmd_echo(void*, const char* params, char* out, int out_size) {
    std::string message;
    if (!find_string(params, "message", message)) {
        std::snprintf(out, static_cast<std::size_t>(out_size),
                      "plugin.demo.echo 需要字符串参数 message");
        return 1;
    }
    return write_out("{\"echo\":\"" + json_escape(message) + "\"}", out, out_size);
}

// plugin.demo.model_summary -> 与 Agent API 的 model.info 同一份结果。
int PMX_PLUGIN_CALL cmd_model_summary(void*, const char*, char* out, int out_size) {
    if (!g_host || !g_host->invoke) {
        std::snprintf(out, static_cast<std::size_t>(out_size), "宿主未提供 invoke");
        return 1;
    }
    return g_host->invoke(g_host->user, "model.info", "{}", out, out_size);
}

// plugin.demo.rotate_bone {index|bone, euler:[x,y,z]} -> 直接转交 bone.pose。
int PMX_PLUGIN_CALL cmd_rotate_bone(void*, const char* params, char* out, int out_size) {
    if (!g_host || !g_host->invoke) {
        std::snprintf(out, static_cast<std::size_t>(out_size), "宿主未提供 invoke");
        return 1;
    }
    return g_host->invoke(g_host->user, "bone.pose", params ? params : "{}", out, out_size);
}

// plugin.demo.log {message} -> 写入宿主日志并返回 {"logged": true}
int PMX_PLUGIN_CALL cmd_log(void*, const char* params, char* out, int out_size) {
    std::string message;
    if (!find_string(params, "message", message)) {
        std::snprintf(out, static_cast<std::size_t>(out_size),
                      "plugin.demo.log 需要字符串参数 message");
        return 1;
    }
    log_info(message);
    return write_out("{\"logged\":true}", out, out_size);
}

}  // namespace

extern "C" PMX_PLUGIN_EXPORT int PMX_PLUGIN_CALL pmx_plugin_init(const PmxHostV1* host) {
    if (!host || host->abi_version != PMX_PLUGIN_ABI_VERSION) return 1;
    g_host = host;
    if (host->register_command(host->user, "plugin.demo.echo", "回显 message 字符串", &cmd_echo,
                               nullptr) != 0)
        return 2;
    if (host->register_command(host->user, "plugin.demo.model_summary",
                               "返回当前模型的统计信息", &cmd_model_summary, nullptr) != 0)
        return 3;
    if (host->register_command(host->user, "plugin.demo.rotate_bone",
                               "按欧拉角旋转骨骼（转交 bone.pose）", &cmd_rotate_bone,
                               nullptr) != 0)
        return 4;
    if (host->register_command(host->user, "plugin.demo.log", "写入 PmxStudio 插件日志",
                               &cmd_log, nullptr) != 0)
        return 5;
    log_info("示例插件已加载");
    return 0;
}

extern "C" PMX_PLUGIN_EXPORT void PMX_PLUGIN_CALL pmx_plugin_shutdown(void) {
    if (g_host && g_host->log) g_host->log(g_host->user, 0, "示例插件已卸载");
    g_host = nullptr;
}
