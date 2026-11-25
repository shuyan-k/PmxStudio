#include "core/material_ext.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "core/fs.hpp"

namespace pmx {
namespace {

// stb_image 能直接解码的格式；大小写都试，Linux 下扩展名区分大小写。
constexpr const char* kImageExts[] = {".png",  ".PNG",  ".jpg",  ".JPG",  ".jpeg",
                                      ".JPEG", ".bmp",  ".BMP",  ".tga",  ".TGA",
                                      ".dds",  ".DDS"};
constexpr const char* kNormalSuffixes[] = {"_normal", "_Normal", "_n", "_N"};
constexpr const char* kOrmSuffixes[] = {"_orm", "_ORM", "_Orm"};
constexpr const char* kMetallicSuffixes[] = {"_metallic", "_Metallic", "_metal", "_Metal"};

template <std::size_t N>
void append_candidates(std::vector<std::string>& out, const std::filesystem::path& dir,
                       const std::string& stem, const char* const (&suffixes)[N]) {
    for (const char* suffix : suffixes) {
        for (const char* ext : kImageExts) {
            // 文件名必须先转成宽/原生路径再拼接：直接用 std::string 与 path 做 / 运算
            // 在 Windows 上会按 ANSI 代码页转换，非本机代码页的日文名会抛异常。
            out.push_back(utf8_string(dir / utf8_path(stem + suffix + ext)));
        }
    }
}

std::string first_existing(const std::vector<std::string>& candidates) {
    for (const std::string& path : candidates) {
        if (path_exists(path)) return path;
    }
    return std::string();
}

}  // namespace

MaterialExtCandidates material_ext_candidates(const std::string& diffuse_path) {
    MaterialExtCandidates out;
    if (diffuse_path.empty()) return out;
    const std::filesystem::path path = utf8_path(diffuse_path);
    const std::string stem = utf8_string(path.stem());
    if (stem.empty()) return out;
    const std::filesystem::path dir = path.parent_path();
    append_candidates(out.normal, dir, stem, kNormalSuffixes);
    append_candidates(out.orm, dir, stem, kOrmSuffixes);
    append_candidates(out.metallic, dir, stem, kMetallicSuffixes);
    return out;
}

MaterialExtFiles find_material_ext(const std::string& diffuse_path) {
    MaterialExtFiles out;
    const MaterialExtCandidates candidates = material_ext_candidates(diffuse_path);
    out.normal = first_existing(candidates.normal);
    out.orm = first_existing(candidates.orm);
    out.metallic = first_existing(candidates.metallic);
    return out;
}

std::vector<unsigned char> generate_normal_map(const unsigned char* rgba, int w, int h,
                                               float strength) {
    std::vector<unsigned char> out;
    if (!rgba || w < 3 || h < 3) return out;
    const std::size_t count = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);

    // 亮度当作高度场。做两遍 3x3 高斯平滑（等效约 5x5）：贴图里的噪点、
    // 描边锯齿如果直接求梯度，会变成一片细碎凹凸和边缘高光，而不是布料
    // 褶皱那样的宽缓起伏。
    std::vector<float> height(count);
    for (std::size_t i = 0; i < count; ++i) {
        height[i] = (0.299f * rgba[i * 4 + 0] + 0.587f * rgba[i * 4 + 1] +
                     0.114f * rgba[i * 4 + 2]) / 255.0f;
    }
    auto blur = [&](const std::vector<float>& src, std::vector<float>& dst) {
        auto sample = [&](int x, int y) {
            x = (x % w + w) % w;
            y = (y % h + h) % h;
            return src[static_cast<std::size_t>(y) * w + x];
        };
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const float v = 4.0f * sample(x, y) +
                                2.0f * (sample(x - 1, y) + sample(x + 1, y) +
                                        sample(x, y - 1) + sample(x, y + 1)) +
                                sample(x - 1, y - 1) + sample(x + 1, y - 1) +
                                sample(x - 1, y + 1) + sample(x + 1, y + 1);
                dst[static_cast<std::size_t>(y) * w + x] = v / 16.0f;
            }
        }
    };
    std::vector<float> smooth(count);
    blur(height, smooth);
    std::vector<float> blurred(count);
    blur(smooth, blurred);

    const float gain = 2.0f * std::clamp(strength, 0.0f, 4.0f);
    out.resize(count * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            auto s = [&](int sx, int sy) {
                sx = (sx % w + w) % w;
                sy = (sy % h + h) % h;
                return blurred[static_cast<std::size_t>(sy) * w + sx];
            };
            const float dx =
                (s(x + 1, y - 1) + 2.0f * s(x + 1, y) + s(x + 1, y + 1)) -
                (s(x - 1, y - 1) + 2.0f * s(x - 1, y) + s(x - 1, y + 1));
            const float dy =
                (s(x - 1, y + 1) + 2.0f * s(x, y + 1) + s(x + 1, y + 1)) -
                (s(x - 1, y - 1) + 2.0f * s(x, y - 1) + s(x + 1, y - 1));
            // 高度沿 +u/+v 升高时表面向反方向倾斜；Sobel 核的和为 8。
            float nx = -dx * 0.125f * gain;
            float ny = -dy * 0.125f * gain;
            const float inv = 1.0f / std::sqrt(nx * nx + ny * ny + 1.0f);
            nx *= inv;
            ny *= inv;
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
            out[i + 0] = static_cast<unsigned char>((nx * 0.5f + 0.5f) * 255.0f + 0.5f);
            out[i + 1] = static_cast<unsigned char>((ny * 0.5f + 0.5f) * 255.0f + 0.5f);
            out[i + 2] = static_cast<unsigned char>((inv * 0.5f + 0.5f) * 255.0f + 0.5f);
            out[i + 3] = 255;
        }
    }
    return out;
}

}  // namespace pmx
