// 可选材质扩展（法线 / 金属度）的贴图配对约定。
//
// PMX 材质本身只有漫反射、球面与 toon 贴图，没有法线 / 金属度通道（MMD 里
// 这些靠 MME 的 .fx 效果实现）。内置插件按漫反射贴图文件名自动配对：
//   body.png -> body_normal.png   切线空间法线贴图
//            -> body_orm.png      G=粗糙度，B=金属度（glTF ORM 约定）
//            -> body_metallic.png 灰度金属度（缺 ORM 时使用）
// 所有候选都不存在时返回空路径，渲染保持原样。
#pragma once

#include <string>
#include <vector>

namespace pmx {

struct MaterialExtCandidates {
    std::vector<std::string> normal;
    std::vector<std::string> orm;
    std::vector<std::string> metallic;
};

// 由漫反射贴图路径生成候选文件名（不检查磁盘，按优先级排列）。
MaterialExtCandidates material_ext_candidates(const std::string& diffuse_path);

struct MaterialExtFiles {
    std::string normal;
    std::string orm;
    std::string metallic;
    bool empty() const { return normal.empty() && orm.empty() && metallic.empty(); }
};

// 在候选列表中挑选第一个存在的文件；diffuse_path 为空时返回全空。
MaterialExtFiles find_material_ext(const std::string& diffuse_path);

// 缺少真实法线贴图时，从漫反射贴图亮度推导切线空间法线（DirectX 约定：G 对应 +v）。
// rgba 为 w*h 的 RGBA8 像素，返回同尺寸 RGBA8 法线图；尺寸不足时返回空。
// strength 缩放凹凸强度，1.0 为默认。注意：这是从画好的明暗里猜细节，
// 适合布料/皮革等纹理，脸部彩绘可能产生不自然的凹凸。
std::vector<unsigned char> generate_normal_map(const unsigned char* rgba, int w, int h,
                                               float strength = 1.0f);

}  // namespace pmx
