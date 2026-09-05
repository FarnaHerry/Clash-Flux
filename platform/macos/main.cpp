// platform/macos/main.cpp — macOS 平台入口（HuxerUI CLI 生成格式）。
// 无参数 → GUI；有参数 → CLI 子命令（clashflux.cli，见 src/cli.cppm）。
#include <huxerui/app.h>

#include <string>
#include <vector>

import clashflux.cli;

int main(int argc, char** argv) {
    if (argc > 1) {
        return cli::run(std::vector<std::string>(argv + 1, argv + argc));
    }
    return huxerui::RunApplication();
}
