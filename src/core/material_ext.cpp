#include "core/material_ext.hpp"

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
            out.push_back(utf8_string(dir / (stem + suffix + ext)));
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

}  // namespace pmx
