// pmxstudio-cli：无 GL 的 PMX 批处理入口。
//
// 用法：
//   pmxstudio-cli --model <in.pmx> --script <file.json> [--out <out.pmx>]
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "core/fs.hpp"
#include "core/pmx.hpp"
#include "core/script.hpp"
#include "core/utf8_args.hpp"

namespace {

void print_usage() {
    std::printf(
        "pmxstudio-cli - PMX 批处理工具\n"
        "\n"
        "用法：\n"
        "  pmxstudio-cli --model <in.pmx> --script <file.json> [--out <out.pmx>]\n"
        "  pmxstudio-cli --help\n"
        "\n"
        "参数：\n"
        "  --model <in.pmx>     输入 PMX 模型\n"
        "  --script <file.json> JSON 命令脚本\n"
        "  --out <out.pmx>      输出路径（默认 <in 去扩展名>.out.pmx）\n"
        "  --help, -h           显示本帮助\n");
}

std::string default_output_path(const std::string& input) {
    std::filesystem::path path = pmx::utf8_path(input);
    path.replace_extension(".out.pmx");
    return pmx::utf8_string(path);
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args = pmx::utf8_args(argc, argv);

    std::string model_path;
    std::string script_path;
    std::string out_path;

    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        }
        if (arg == "--model" || arg == "--script" || arg == "--out") {
            if (i + 1 >= args.size() || args[i + 1].empty()) {
                std::printf("错误：%s 缺少参数值\n\n", arg.c_str());
                print_usage();
                return 1;
            }
            const std::string& value = args[++i];
            if (arg == "--model")
                model_path = value;
            else if (arg == "--script")
                script_path = value;
            else
                out_path = value;
            continue;
        }
        std::printf("错误：未知参数 %s\n\n", arg.c_str());
        print_usage();
        return 1;
    }

    if (model_path.empty() || script_path.empty()) {
        std::printf("错误：必须同时指定 --model 与 --script\n\n");
        print_usage();
        return 1;
    }
    if (out_path.empty()) out_path = default_output_path(model_path);

    pmx::Model model;
    try {
        model = pmx::load_pmx(model_path);
    } catch (const std::exception& error) {
        std::printf("错误：加载模型失败：%s\n", error.what());
        return 1;
    }

    const pmx::ScriptResult result = pmx::run_script_file(model, script_path);
    if (!result.ok) {
        std::printf("错误：%s\n", result.message.c_str());
        return 1;
    }
    if (!result.message.empty()) std::printf("%s\n", result.message.c_str());

    try {
        pmx::save_pmx(model, out_path);
    } catch (const std::exception& error) {
        std::printf("错误：保存模型失败：%s\n", error.what());
        return 1;
    }

    std::printf("完成：执行 %d 条命令\n输入：%s\n输出：%s\n", result.commands,
                model_path.c_str(), out_path.c_str());
    return 0;
}
