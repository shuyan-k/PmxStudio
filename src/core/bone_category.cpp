#include "core/bone_category.hpp"

#include <cctype>
#include <initializer_list>
#include <string>

namespace pmx {
namespace {

bool contains(const std::string& text, const char* key) {
    return !text.empty() && text.find(key) != std::string::npos;
}

// 两个名字（原文 / 英文）任一命中关键词即可。
bool has_any(const std::string& name, const std::string& name_en,
             std::initializer_list<const char*> keys) {
    for (const char* key : keys)
        if (contains(name, key) || contains(name_en, key)) return true;
    return false;
}

// 只折叠 ASCII 大小写，保留日文 / 中文原字符。
std::string ascii_lower(std::string text) {
    for (char& c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

}  // namespace

const char* bone_category_name(BoneCategory category) {
    switch (category) {
        case BoneCategory::Torso: return "体干";
        case BoneCategory::Limbs: return "四肢";
        case BoneCategory::Hair: return "头发";
        case BoneCategory::Clothing: return "衣服";
        case BoneCategory::Accessory: return "饰品";
        case BoneCategory::Prop: return "道具";
        default: return "其他";
    }
}

const char* bone_category_id(BoneCategory category) {
    switch (category) {
        case BoneCategory::Torso: return "torso";
        case BoneCategory::Limbs: return "limbs";
        case BoneCategory::Hair: return "hair";
        case BoneCategory::Clothing: return "clothing";
        case BoneCategory::Accessory: return "accessory";
        case BoneCategory::Prop: return "prop";
        default: return "other";
    }
}

BoneCategory classify_bone(const std::string& name_orig, const std::string& name_en) {
    // 主名字也可能直接是英文（如 DownTeeth），统一折叠大小写后再匹配。
    const std::string name = ascii_lower(name_orig);
    const std::string en = ascii_lower(name_en);

    // 顺序即优先级：先判断语义更具体的头发 / 衣服 / 饰品 / 道具，
    // 再判断四肢与体干，避免「腕袖」被算成四肢、「耳環」被算成体干。
    if (has_any(name, en,
                {"髪", "髮", "头发", "刘海", "劉海", "長髮", "长发", "马尾", "辮", "辫",
                 "鬓", "鬢", "アホ毛", "もみあげ", "まつ毛", "睫毛", "髭", "ひげ",
                 "hair", "bangs", "fringe", "ponytail", "twintail", "braid", "ahoge",
                 "beard"}))
        return BoneCategory::Hair;

    if (has_any(name, en,
                {"袖", "裙", "服", "衣", "裾", "襟", "領", "领", "紐", "纽", "帯", "帶", "布",
                 "足袋", "靴下", "靴", "手袋", "袜", "鞋", "手套", "衣领", "蝴蝶结",
                 "スカート", "ズボン", "パンツ", "リボン", "ネクタイ", "マフラー",
                 "スカーフ", "ケープ", "マント", "フリル", "レース", "ベルト",
                 "ボタン", "チャック", "ポケット", "フード", "水着", "制服",
                 "セーラー", "ジャケット", "コート", "ソックス", "ストッキング",
                 "タイツ", "sleeve", "skirt", "cloth", "clothes", "dress", "coat",
                 "jacket", "shirt", "blouse", "uniform", "collar", "hem", "ribbon",
                 "necktie", "scarf", "muffler", "cape", "frill", "belt", "boot",
                 "shoe", "sock", "stocking", "glove", "hood", "button", "zipper",
                 "pocket", "swimsuit", "armor"}))
        return BoneCategory::Clothing;

    if (has_any(name, en,
                {"耳環", "耳环", "飾", "饰", "頭飾", "头饰", "項鏈", "项链", "戒指",
                 "王冠", "帽子", "眼鏡", "眼镜", "メガネ", "マスク", "仮面", "假面",
                 "面具", "面罩", "角", "翼", "羽", "しっぽ", "尻尾",
                 "アクセ", "イヤリング", "ピアス", "ネックレス", "ティアラ",
                 "accessor", "earring", "piercing", "necklace", "bracelet", "crown",
                 "tiara", "hat", "helmet", "glasses", "goggles", "mask", "horn",
                 "wing", "pendant", "choker", "badge", "jewel", "ornament",
                 "headphone"}))
        return BoneCategory::Accessory;

    if (has_any(name, en,
                {"武器", "剣", "剑", "刀", "銃", "枪", "槍", "杖", "盾", "弓", "矢",
                 "小物", "道具", "アイテム",
                 "weapon", "sword", "dagger", "gun", "rifle", "spear", "shield",
                 "staff", "wand", "arrow", "prop", "item"}))
        return BoneCategory::Prop;

    if (has_any(name, en,
                {"肩", "腕", "臂", "ひじ", "肘", "手首", "手先", "手", "捩", "指", "親指",
                 "人差指", "人指", "中指", "薬指", "药指", "小指", "脚", "足",
                 "腿", "ひざ", "膝", "つま先", "爪先", "太もも", "もも", "ダミー",
                 "arm", "elbow", "wrist", "hand", "finger", "thumb", "index",
                 "middle", "pinky", "leg", "knee", "foot", "feet", "ankle",
                 "toe", "thigh", "shoulder", "dummy"}))
        return BoneCategory::Limbs;

    if (has_any(name, en,
                {"センター", "グルーブ", "腰", "上半身", "下半身", "首", "頭", "头",
                 "胸", "腹", "背", "胴", "骨盤", "全ての親", "操作中心", "親", "目",
                 "瞳", "舌", "歯", "齿", "口", "眉", "鼻", "顔", "脸", "耳",
                 "center", "groove", "waist", "upperbody", "lowerbody", "upper body",
                 "lower body", "spine", "chest", "breast", "bust", "neck", "head",
                 "hip", "pelvis", "root", "body", "torso", "eye", "pupil", "tongue",
                 "teeth", "tooth", "mouth", "brow", "nose", "face", "ear", "jaw",
                 "chin"}))
        return BoneCategory::Torso;

    return BoneCategory::Other;
}

std::vector<BoneCategory> classify_bones(const Model& model) {
    std::vector<BoneCategory> out;
    out.reserve(model.bones.size());
    for (const Bone& bone : model.bones)
        out.push_back(classify_bone(bone.name, bone.name_en));
    return out;
}

}  // namespace pmx
