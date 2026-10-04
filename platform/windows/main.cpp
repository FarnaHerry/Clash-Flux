// platform/windows/main.cpp — Windows 平台入口（HuxerUI CLI 生成格式）。
// 链接为 /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup（见顶层 CMakeLists），保留 main。
// 无参数 → GUI；有参数 → CLI 子命令（clashflux.cli，见 src/cli.cppm）。
//
// 单实例：GUI 与 CLI 都先抢实例锁。抢到的进程在自己的运行时里执行命令（伪 CLI，
// 窗口隐藏到托盘）；已有实例时把命令转发给它执行并回传输出/退出码，不再启动
// 第二个运行时。version/help 与 service 子命令走无运行时快路径（见 cli.cppm）。
#include <huxerui/app.h>
#include <huxerui/system.h>
#include "../../src/profile_link.h"

#include <cstdio>
#include <exception>
#include <string>
#include <utility>
#include <vector>

import clashflux.cli;
import clashflux.cli_ipc;
import clashflux.instance;

int main(int argc, char** argv) {
    // The public registration API preserves registrations owned by other apps.
    if (argc == 1 || (argc == 2 && clashflux::profile_link::IsSupportedScheme(
            std::string_view(argv[1]).substr(0, std::string_view(argv[1]).find(':'))))) {
        for (const char* scheme : {"clash-flux", "sing-box", "flclash", "clash", "clashmeta"}) {
            try { huxerui::windows::RegisterUrlScheme(scheme, "Clash-Flux"); }
            catch (const std::exception& error) { std::fprintf(stderr, "URL scheme %s: %s\n", scheme, error.what()); }
        }
    }
    if (argc == 2 && clashflux::profile_link::IsSupportedScheme(
            std::string_view(argv[1]).substr(0, std::string_view(argv[1]).find(':')))) {
        std::string error;
        auto profile = clashflux::profile_link::Parse(argv[1], error);
        if (!profile) { std::fprintf(stderr, "%s\n", error.c_str()); return 2; }
        if (!clashflux::instance::acquireOrActivate()) {
            int code = 1;
            return clashflux::cli_ipc::tryForwardCommand({"open-profile-link", argv[1]}, code) ? code : 1;
        }
        if (!clashflux::profile_link::Submit(std::move(*profile), error)) return 2;
        const int runtimeCode = huxerui::RunApplication();
        const int code = cli::pendingExitCode();
        return code != 0 ? code : runtimeCode;
    }
    if (argc > 1) {
        std::vector<std::string> args(argv + 1, argv + argc);
        if (cli::isDirectCommand(args)) return cli::run(args);
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
    const int runtimeCode = huxerui::RunApplication();
    const int code = cli::pendingExitCode();
    return code != 0 ? code : runtimeCode;
}
