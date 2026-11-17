// 脚本层独立测试：构造内存模型，跑批处理命令并断言结果。
#include <cstdio>
#include <string>

#include "core/pmx.hpp"
#include "core/script.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "  [失败] %s\n", what);
    }
}

pmx::Model make_model() {
    pmx::Model model;
    model.name = "test";

    pmx::Bone center;
    center.name = "センター";
    center.name_en = "center";
    model.bones.push_back(center);

    pmx::Bone leg_l;
    leg_l.name = "左足";
    leg_l.name_en = "leg_L";
    model.bones.push_back(leg_l);

    pmx::Bone leg_r;
    leg_r.name = "右足";
    leg_r.name_en = "leg_R";
    model.bones.push_back(leg_r);

    pmx::Material material;
    material.name = "体";
    material.name_en = "body";
    model.materials.push_back(material);

    pmx::Morph morph;
    morph.name = "まばたき";
    morph.name_en = "blink";
    model.morphs.push_back(morph);

    pmx::RigidBody rigid;
    rigid.name = "スカート_0";
    rigid.name_en = "skirt_0";
    rigid.mass = 1.0f;
    rigid.friction = 0.5f;
    rigid.repel = 0.0f;
    rigid.mode = 0;
    model.rigid.push_back(rigid);

    return model;
}

const char* kScript = R"json(
[
  {"cmd": "rename_bone", "from": "センター", "to": "中心"},
  {"cmd": "rename_bone", "from": "leg_L", "to": "leg_left"},
  {"cmd": "rename_bone_prefix", "from": "右", "to": "right_"},
  {"cmd": "rename_material", "from": "\u4F53", "to": "body"},
  {"cmd": "rename_morph", "from": "まばたき", "to": "blink"},
  {"cmd": "set_rigid", "name": "skirt_0", "mass": 2.5, "friction": 0.8,
   "restitution": 0.35, "mode": 2},
  {"cmd": "count_bones"},
  {"cmd": "count_rigid"}
]
)json";

void test_commands() {
    std::printf("[1] 命令执行与改名\n");
    pmx::Model model = make_model();
    const pmx::ScriptResult result = pmx::run_script_json(model, kScript);

    check(result.ok, "脚本执行成功");
    check(result.commands == 8, "命令计数为 8");
    check(result.message == "骨骼数量：3\n刚体数量：1", "count 命令输出数量");

    check(model.bones.size() == 3, "骨骼数量不变");
    check(model.bones[0].name == "中心", "按日文名改名");
    check(model.bones[0].name_en == "center", "不改 name_en");
    check(model.bones[1].name == "leg_left", "按 name_en 改名");
    check(model.bones[1].name_en == "leg_L", "name_en 保持原值");
    check(model.bones[2].name == "right_足", "前缀替换");
    check(model.bones[2].name_en == "leg_R", "前缀替换不影响 name_en");

    check(model.materials.size() == 1 && model.materials[0].name == "body",
          "材质改名（\\u 转义解码）");
    check(model.materials[0].name_en == "body", "材质 name_en 不变");
    check(model.morphs.size() == 1 && model.morphs[0].name == "blink", "形态改名");

    check(model.rigid.size() == 1, "刚体数量不变");
    const pmx::RigidBody& rigid = model.rigid[0];
    check(rigid.mass > 2.49f && rigid.mass < 2.51f, "mass 已写入");
    check(rigid.friction > 0.79f && rigid.friction < 0.81f, "friction 已写入");
    check(rigid.repel > 0.34f && rigid.repel < 0.36f, "restitution 写入 repel");
    check(rigid.mode == 2, "mode 已写入");
}

void test_failures() {
    std::printf("[2] 错误处理\n");
    {
        pmx::Model model = make_model();
        const pmx::ScriptResult result = pmx::run_script_json(model, R"([{"cmd": "nope"}])");
        check(!result.ok, "未知命令失败");
        check(result.message.find("未知命令") != std::string::npos, "提示未知命令");
        check(result.commands == 0, "失败时不累计命令");
    }
    {
        pmx::Model model = make_model();
        const pmx::ScriptResult result =
            pmx::run_script_json(model, R"([{"cmd": "rename_bone", "from": "a"}])");
        check(!result.ok, "缺少参数失败");
        check(result.message.find("\"to\"") != std::string::npos, "提示缺少 to");
        check(model.bones[0].name == "センター", "失败前未改动模型");
    }
    {
        pmx::Model model = make_model();
        const pmx::ScriptResult result = pmx::run_script_json(model, R"([{"cmd": }])");
        check(!result.ok, "非法 JSON 失败");
        check(result.message.find("JSON 解析失败") != std::string::npos, "提示 JSON 错误");
        check(result.message.find("行") != std::string::npos, "错误带位置");
    }
    {
        pmx::Model model = make_model();
        const pmx::ScriptResult result =
            pmx::run_script_json(model, R"({"cmd": "count_bones"})");
        check(!result.ok, "根节点不是数组时失败");
    }
}

}  // namespace

int main() {
    test_commands();
    test_failures();

    if (g_failures == 0) {
        std::printf("全部通过（%d 项）\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "%d/%d 项失败\n", g_failures, g_checks);
    return 1;
}
