// platform/linux/main.cpp — Linux 平台入口。
// 无参数 → HuxerUI GUI；有参数 → CLI 子命令（clashflux.cli，见 src/cli.cppm）。
#include <huxerui/app.h>

#include <string>
#include <vector>

import clashflux.cli;
import clashflux.instance;

int main(int argc, char** argv) {
    if (argc > 1) {
        return cli::run(std::vector<std::string>(argv + 1, argv + argc));
    }
    if (!clashflux::instance::acquireOrActivate()) return 0;
    return huxerui::RunApplication();
}
