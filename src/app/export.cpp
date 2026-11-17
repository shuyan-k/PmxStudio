#include "app/export.hpp"

#include "core/fs.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace pmx {
namespace {

namespace fs = std::filesystem;

std::string* error_sink(std::string* error, std::string& scratch) {
    return error ? error : &scratch;
}

std::string frame_file_name(const std::string& prefix, int frame) {
    char digits[16];
    std::snprintf(digits, sizeof(digits), "%04d", frame);
    return prefix + digits + ".png";
}

fs::path frames_directory(const ExportSettings& settings) {
    return utf8_path(settings.directory.empty() ? std::string("frames")
                                                : settings.directory);
}

int frame_count(const ExportSettings& settings) {
    if (settings.end_frame < settings.begin_frame) return 0;
    const int step = settings.step > 0 ? settings.step : 1;
    return (settings.end_frame - settings.begin_frame) / step + 1;
}

void png_write_callback(void* context, void* data, int size) {
    auto* stream = static_cast<std::ofstream*>(context);
    if (stream != nullptr && data != nullptr && size > 0) {
        stream->write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    }
}

std::string read_text_file(const std::string& path) {
    std::ifstream in = open_ifstream(path);
    if (!in) return std::string();
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string log_tail(const std::string& text, std::size_t max_chars) {
    if (text.size() <= max_chars) return text;
    return text.substr(text.size() - max_chars);
}

std::string temp_log_path() {
    static std::atomic<unsigned> counter{0};
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec);
    if (ec) {
        ec.clear();
        dir = fs::current_path(ec);
    }
    std::string name = "pmxstudio_ffmpeg_";
#ifdef _WIN32
    name += std::to_string(static_cast<unsigned long>(::GetCurrentProcessId()));
#else
    name += std::to_string(static_cast<long long>(::getpid()));
#endif
    name += "_" + std::to_string(counter.fetch_add(1)) + ".log";
    return utf8_string(dir / utf8_path(name));
}

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

// 按 Windows 命令行规则给单个参数加引号（路径可含空格 / 反斜杠 / 引号）。
std::wstring quote_win_arg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring out = L"\"";
    for (std::size_t i = 0; i < arg.size(); ++i) {
        std::size_t slashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++i;
            ++slashes;
        }
        if (i == arg.size()) {
            out.append(slashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            out.append(slashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(slashes, L'\\');
            out.push_back(arg[i]);
        }
    }
    out.push_back(L'"');
    return out;
}
#else
std::string quote_shell_arg(const std::string& arg) {
    std::string out = "'";
    for (char c : arg) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += '\'';
    return out;
}
#endif

// 同步运行子进程，stdout/stderr 合并写入临时日志，读完即删。
bool run_process(const std::string& exe, const std::vector<std::string>& args,
                 std::string* output, int* exit_code, std::string* error) {
    const std::string log_path = temp_log_path();

#ifdef _WIN32
    const std::wstring exe_w = utf8_to_wide(exe);
    const std::wstring log_w = utf8_to_wide(log_path);
    if (exe_w.empty() || log_w.empty()) {
        if (error) *error = "无法转换 ffmpeg 路径";
        return false;
    }

    std::wstring command = quote_win_arg(exe_w);
    for (const std::string& arg : args) {
        command += L' ';
        command += quote_win_arg(utf8_to_wide(arg));
    }
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE log_file =
        ::CreateFileW(log_w.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log_file == INVALID_HANDLE_VALUE) {
        if (error) *error = "无法创建 ffmpeg 日志文件";
        return false;
    }
    HANDLE nul_input =
        ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = (nul_input != INVALID_HANDLE_VALUE) ? nul_input : nullptr;
    si.hStdOutput = log_file;
    si.hStdError = log_file;

    PROCESS_INFORMATION pi{};
    const BOOL started = ::CreateProcessW(
        exe_w.c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    const DWORD start_error = started ? 0 : ::GetLastError();

    ::CloseHandle(log_file);
    if (nul_input != INVALID_HANDLE_VALUE) ::CloseHandle(nul_input);

    if (!started) {
        if (error) {
            *error = "启动 ffmpeg 失败（GetLastError=" +
                     std::to_string(static_cast<unsigned long>(start_error)) + "）";
        }
        std::error_code ec;
        fs::remove(utf8_path(log_path), ec);
        return false;
    }

    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);

    if (output != nullptr) *output = read_text_file(log_path);
    if (exit_code != nullptr) *exit_code = static_cast<int>(code);
    std::error_code ec;
    fs::remove(utf8_path(log_path), ec);
    return true;
#else
    std::string command = quote_shell_arg(exe);
    for (const std::string& arg : args) {
        command += ' ';
        command += quote_shell_arg(arg);
    }
    command += " > " + quote_shell_arg(log_path) + " 2>&1";

    const int status = std::system(command.c_str());
    if (output != nullptr) *output = read_text_file(log_path);
    std::error_code ec;
    fs::remove(utf8_path(log_path), ec);

    if (status == -1) {
        if (error) *error = "无法启动 ffmpeg";
        return false;
    }
    int code = 0;
    if (WIFEXITED(status)) {
        code = WEXITSTATUS(status);
    } else {
        code = -1;
    }
    if (exit_code != nullptr) *exit_code = code;
    return true;
#endif
}

}  // namespace

int export_frames(const ExportSettings& settings, const RenderFrameFn& render,
                  ExportProgress* progress, std::string* error) {
    std::string scratch;
    std::string* err = error_sink(error, scratch);
    err->clear();

    if (settings.width <= 0 || settings.height <= 0) {
        *err = "导出分辨率必须为正";
        return -1;
    }
    if (!render) {
        *err = "缺少渲染回调";
        return -1;
    }
    if (settings.step <= 0) {
        *err = "帧步长必须大于 0";
        return -1;
    }

    const int total = frame_count(settings);
    if (total <= 0) {
        *err = "帧范围为空";
        return -1;
    }

    const fs::path dir = frames_directory(settings);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        *err = "无法创建序列帧目录：" + ec.message();
        return -1;
    }

    if (progress != nullptr) {
        progress->frame = 0;
        progress->total = total;
        progress->done = false;
        progress->message = "开始导出序列帧";
    }

    std::vector<std::uint8_t> rgba;
    int written = 0;
    for (int i = 0; i < total; ++i) {
        if (progress != nullptr && progress->cancel_requested) {
            progress->done = true;
            progress->message = "导出已取消";
            return written;
        }

        const int frame = settings.begin_frame + i * settings.step;
        if (progress != nullptr) {
            progress->frame = frame;
            progress->message = "渲染帧 " + std::to_string(frame);
        }

        rgba.clear();
        if (!render(frame, settings.width, settings.height, rgba, err)) {
            if (err->empty()) *err = "渲染第 " + std::to_string(frame) + " 帧失败";
            return -1;
        }
        const std::size_t need = static_cast<std::size_t>(settings.width) *
                                 static_cast<std::size_t>(settings.height) * 4;
        if (rgba.size() < need) {
            *err = "渲染回调返回的图像尺寸不符";
            return -1;
        }

        const fs::path file_path =
            dir / utf8_path(frame_file_name(settings.prefix, frame));
        std::ofstream out = open_ofstream(utf8_string(file_path));
        if (!out) {
            *err = "无法写入 " + utf8_string(file_path);
            return -1;
        }

        const int ok = stbi_write_png_to_func(
            png_write_callback, &out, settings.width, settings.height, 4, rgba.data(),
            static_cast<ptrdiff_t>(settings.width) * 4);
        out.flush();
        if (!out) {
            *err = "写入 PNG 失败：" + utf8_string(file_path);
            return -1;
        }
        if (ok == 0) {
            *err = "编码 PNG 失败：" + utf8_string(file_path);
            return -1;
        }

        ++written;
        if (progress != nullptr) {
            progress->message = "已写出 " + std::to_string(written) + " / " +
                                std::to_string(total) + " 帧";
        }
    }

    if (progress != nullptr) {
        progress->done = true;
        progress->message = "序列帧导出完成";
    }
    return written;
}

bool find_ffmpeg(std::string* full_path) {
    std::vector<std::string> candidates;

#ifdef _WIN32
    const char* path_env = std::getenv("PATH");
    if (path_env != nullptr) {
        std::string paths = path_env;
        std::size_t start = 0;
        while (start <= paths.size()) {
            const std::size_t end = paths.find(';', start);
            const std::string dir =
                paths.substr(start, end == std::string::npos ? std::string::npos
                                                             : end - start);
            if (!dir.empty()) {
                const char last = dir.back();
                const std::string sep = (last == '\\' || last == '/') ? "" : "\\";
                candidates.push_back(dir + sep + "ffmpeg.exe");
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }

    const char* program_files = std::getenv("ProgramFiles");
    if (program_files != nullptr) {
        candidates.push_back(std::string(program_files) + "\\ffmpeg\\bin\\ffmpeg.exe");
    }
    const char* program_files_x86 = std::getenv("ProgramFiles(x86)");
    if (program_files_x86 != nullptr) {
        candidates.push_back(std::string(program_files_x86) + "\\ffmpeg\\bin\\ffmpeg.exe");
    }
    const char* local_app_data = std::getenv("LOCALAPPDATA");
    if (local_app_data != nullptr) {
        candidates.push_back(std::string(local_app_data) +
                             "\\Microsoft\\WinGet\\Links\\ffmpeg.exe");
    }
    candidates.push_back("C:\\ffmpeg\\bin\\ffmpeg.exe");
    candidates.push_back("C:\\ProgramData\\chocolatey\\bin\\ffmpeg.exe");
#else
    const char* path_env = std::getenv("PATH");
    if (path_env != nullptr) {
        std::string paths = path_env;
        std::size_t start = 0;
        while (start <= paths.size()) {
            const std::size_t end = paths.find(':', start);
            const std::string dir =
                paths.substr(start, end == std::string::npos ? std::string::npos
                                                             : end - start);
            if (!dir.empty()) {
                const char last = dir.back();
                const std::string sep = last == '/' ? "" : "/";
                candidates.push_back(dir + sep + "ffmpeg");
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    candidates.push_back("/usr/bin/ffmpeg");
    candidates.push_back("/usr/local/bin/ffmpeg");
    candidates.push_back("/opt/homebrew/bin/ffmpeg");
    candidates.push_back("/snap/bin/ffmpeg");
#endif

    for (const std::string& candidate : candidates) {
        if (path_exists(candidate)) {
            if (full_path != nullptr) *full_path = candidate;
            return true;
        }
    }
    return false;
}

bool encode_video(const ExportSettings& settings, std::string* error, std::string* log) {
    std::string scratch;
    std::string* err = error_sink(error, scratch);
    err->clear();
    if (log != nullptr) log->clear();

    if (settings.video_path.empty()) {
        *err = "未指定视频输出路径";
        return false;
    }
    if (settings.fps <= 0) {
        *err = "视频帧率必须为正";
        return false;
    }
    if (!settings.audio_path.empty() && !path_exists(settings.audio_path)) {
        *err = "音频文件不存在：" + settings.audio_path;
        return false;
    }

    std::string ffmpeg;
    if (!find_ffmpeg(&ffmpeg)) {
        *err = "未找到 ffmpeg，请安装并加入 PATH";
        return false;
    }

    const fs::path dir = frames_directory(settings);
    const std::string pattern =
        utf8_string(dir / utf8_path(settings.prefix + "%04d.png"));

    std::vector<std::string> args;
    args.push_back("-y");
    args.push_back("-framerate");
    args.push_back(std::to_string(settings.fps));
    args.push_back("-start_number");
    args.push_back(std::to_string(settings.begin_frame));
    args.push_back("-i");
    args.push_back(pattern);
    if (!settings.audio_path.empty()) {
        args.push_back("-i");
        args.push_back(settings.audio_path);
    }
    args.push_back("-c:v");
    args.push_back("libx264");
    args.push_back("-crf");
    args.push_back(std::to_string(settings.crf));
    args.push_back("-pix_fmt");
    args.push_back("yuv420p");
    args.push_back("-vf");
    args.push_back("scale=out_color_matrix=bt709");
    if (!settings.audio_path.empty()) {
        args.push_back("-c:a");
        args.push_back("aac");
        args.push_back("-shortest");
    }
    args.push_back(settings.video_path);

    std::string output;
    int exit_code = 0;
    if (!run_process(ffmpeg, args, &output, &exit_code, err)) return false;
    if (log != nullptr) *log = output;

    if (exit_code != 0) {
        *err = "ffmpeg 编码失败（退出码 " + std::to_string(exit_code) + "）";
        const std::string tail = log_tail(output, 800);
        if (!tail.empty()) *err += "\n" + tail;
        return false;
    }
    if (!path_exists(settings.video_path)) {
        *err = "ffmpeg 已退出但未生成视频：" + settings.video_path;
        return false;
    }
    return true;
}

std::string frame_file_path(const ExportSettings& settings, int frame) {
    fs::path dir = settings.directory.empty() ? fs::path("frames")
                                              : utf8_path(settings.directory);
    return utf8_string(dir / utf8_path(frame_file_name(settings.prefix, frame)));
}

bool write_png(const std::string& path, int width, int height,
               const std::vector<std::uint8_t>& rgba, std::string* error) {
    try {
        const fs::path file_path = utf8_path(path);
        if (file_path.has_parent_path())
            fs::create_directories(file_path.parent_path());
        std::ofstream out = open_ofstream(path);
        if (!out) {
            if (error) *error = "无法写入 " + path;
            return false;
        }
        const int ok = stbi_write_png_to_func(png_write_callback, &out, width, height, 4,
                                             rgba.data(), width * 4);
        out.flush();
        if (!ok || !out) {
            if (error) *error = "写入 PNG 失败：" + path;
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

std::string ffmpeg_version() {
    std::string ffmpeg;
    if (!find_ffmpeg(&ffmpeg)) return std::string();

    std::string output;
    int exit_code = 0;
    std::string error;
    if (!run_process(ffmpeg, {"-version"}, &output, &exit_code, &error)) {
        return std::string();
    }
    if (exit_code != 0 || output.empty()) return std::string();

    const std::size_t end = output.find_first_of("\r\n");
    std::string line = output.substr(0, end);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    return line;
}

}  // namespace pmx
