// platform/linux/main.cpp — Linux 平台入口。
// 无参数 → HuxerUI GUI；有参数 → CLI 子命令（clashflux.cli，见 src/cli.cppm）。
//
// 单实例：GUI 与 CLI 都先抢实例锁。抢到的进程在自己的运行时里执行命令（伪 CLI，
// 窗口隐藏到托盘）；已有实例时把命令转发给它执行并回传输出/退出码，不再启动
// 第二个运行时。version/help 这类纯输出命令走无运行时快路径。
#include <huxerui/app.h>

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

import clashflux.cli;
import clashflux.cli_ipc;
import clashflux.instance;

int main(int argc, char** argv) {
    if (argc > 1) {
        std::vector<std::string> args(argv + 1, argv + argc);
        if (cli::isPureOutputCommand(args)) return cli::run(args);
        if (!clashflux::instance::acquireOrActivate(/*activate=*/false)) {
            int code = 1;
            if (clashflux::cli_ipc::tryForwardCommand(args, code)) return code;
            std::fprintf(stderr, "clash-flux: 已有实例在运行，但命令转发失败\n");
            return 1;
        }
        cli::setPendingCommand(std::move(args));
        const int runtimeCode = huxerui::RunApplication();
        const int code = cli::pendingExitCode();
        return code != 0 ? code : runtimeCode;
    }
    if (!clashflux::instance::acquireOrActivate()) return 0;
    return huxerui::RunApplication();
}
