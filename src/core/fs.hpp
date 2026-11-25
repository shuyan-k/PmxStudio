// UTF-8 路径辅助。
// Windows 下 std::ifstream(const char*) / std::filesystem::path(const std::string&)
// 都按系统 ANSI 代码页解释，UTF-8 的中文/日文目录会直接打不开。所有文件访问
// 统一先转成宽字符 filesystem::path，路径字符串本身保持 UTF-8。
#pragma once

#include <filesystem>
#include <fstream>
#include <ios>
#include <string>

namespace pmx {

inline std::filesystem::path utf8_path(const std::string& utf8) {
#if defined(_WIN32) && defined(__cpp_lib_char8_t)
    try {
        return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
    } catch (const std::filesystem::filesystem_error&) {
        // 个别 PMX 的贴图名不是合法 UTF-8（或含孤立代理项），Windows 上
        // path 构造会抛 filesystem_error；异常冒进渲染循环会直接 terminate。
        // 这里退化为只保留 ASCII 字节，至少不会崩溃（该贴图会按缺失处理）。
        std::string ascii;
        ascii.reserve(utf8.size());
        for (unsigned char c : utf8)
            if (c < 0x80) ascii.push_back(static_cast<char>(c));
        return std::filesystem::path(std::u8string(ascii.begin(), ascii.end()));
    }
#elif defined(__cpp_lib_char8_t)
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
#else
    return std::filesystem::u8path(utf8);
#endif
}

inline std::string utf8_string(const std::filesystem::path& path) {
#if defined(__cpp_lib_char8_t)
    const std::u8string u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
#else
    return path.u8string();
#endif
}

inline std::ifstream open_ifstream(const std::string& path) {
    return std::ifstream(utf8_path(path), std::ios::binary);
}

inline std::ofstream open_ofstream(const std::string& path,
                                   std::ios::openmode mode = std::ios::binary |
                                                              std::ios::trunc) {
    return std::ofstream(utf8_path(path), mode);
}

inline bool path_exists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(utf8_path(path), ec);
}

}  // namespace pmx
