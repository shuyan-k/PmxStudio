// 骨骼显示分类：按骨骼名（日文 / 中文 / 英文）自动归入体干、四肢、头发、衣服等，
// 供视口按类别显示或隐藏。参考 MMD Tools / Blender 的骨骼集合用法。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/pmx.hpp"

namespace pmx {

enum class BoneCategory : std::uint8_t {
    Torso,      // 体干：センター / 上半身 / 下半身 / 頭 / 首 / 目 / 舌 等
    Limbs,      // 四肢：腕 / ひじ / 手 / 指 / 脚 / ひざ / 足 等
    Hair,       // 头发：髪 / 刘海 / アホ毛 等
    Clothing,   // 衣服：袖 / 裙 / スカート / リボン / 靴 等
    Accessory,  // 饰品：耳環 / 帽子 / 仮面 / 角 等
    Prop,       // 道具：武器 / 剣 / 杖 等
    Other,      // 其他：无法识别或空名
    Count
};

constexpr int kBoneCategoryCount = static_cast<int>(BoneCategory::Count);

// 中文显示名（如「头发」）与稳定的英文 id（如 "hair"）。
const char* bone_category_name(BoneCategory category);
const char* bone_category_id(BoneCategory category);

// 自动分类单根骨骼；name 为日文/中文名，name_en 可为空。
BoneCategory classify_bone(const std::string& name, const std::string& name_en);

// 对整只模型逐骨分类。
std::vector<BoneCategory> classify_bones(const Model& model);

}  // namespace pmx
