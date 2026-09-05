// cli.cppm — clashflux.cli：完整命令行入口（无 GUI 依赖）。
//
// 主入口分流：argv 无参数 → GUI；有参数 → cli::run。子命令围绕「内核 /
// 订阅 / 代理」三块，全部复用领域 store（与 GUI 同一份数据目录与设置）：
//
//   clash-flux version
//   clash-flux service install|uninstall|status|run   服务模式（root 托管内核）
//   clash-flux core start|stop|restart|status         内核生命周期
//   clash-flux mode [rule|global|direct]              查/切出站模式
//   clash-flux tun on|off                             TUN 开关
//   clash-flux proxy on|off|status                    系统代理开关
//   clash-flux profile list                           订阅列表
//   clash-flux profile import <url> [名称]            导入订阅
//   clash-flux profile use <id>                       启用订阅（重启内核生效）
//   clash-flux profile update <id>                    更新订阅
//   clash-flux profile remove <id>                    删除订阅
//
// 与 GUI 的关系：CLI 启动的内核若为直连 spawn 则以 setsid 脱离会话驻留
// （pid 写 core/mihomo.pid）；GUI 启动时先探测控制器，已有内核在跑则直接
// 接管（不重复 spawn）。装了服务模式后 start/stop 一律经 root 服务。
module;

// stderr 是宏，模块不导出宏：println(stderr, ...) 需要本 TU 文本可见。
#include <cstdio>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

export module clashflux.cli;

import std;

import clashflux.config;
import clashflux.db;
import clashflux.core;
import clashflux.sysproxy;
import clashflux.service;
import clashflux.store.core;
import clashflux.store.profiles;

namespace cli {

namespace {

void printUsage() {
    std::println(
        "Clash-Flux v{} — mihomo 代理客户端\n"
        "\n"
        "用法：clash-flux [命令] [参数]    （无参数 = 启动 GUI）\n"
        "\n"
        "  version                          版本信息\n"
        "  service install|uninstall|status 安装/卸载/查看 root 内核服务\n"
        "  core start|stop|restart|status   内核生命周期\n"
        "  mode [rule|global|direct]        查/切出站模式\n"
        "  tun on|off                       TUN 透明代理开关\n"
        "  proxy on|off|status              系统代理开关\n"
        "  profile list                     订阅列表\n"
        "  profile import <url> [名称]      导入订阅\n"
        "  profile use <id>                 启用订阅\n"
        "  profile update <id>              更新订阅\n"
        "  profile remove <id>              删除订阅\n",
        CLASHFLUX_VERSION);
}

int cmdService(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::println(stderr, "service 需要子命令：install|uninstall|status");
        return 2;
    }
    if (args[0] == "install") return service::install();
    if (args[0] == "uninstall") return service::uninstall();
    if (args[0] == "run") return service::run();  // systemd ExecStart 专用
    if (args[0] == "status") {
        std::println("服务：{}；内核：{}",
                     service::installed()
                         ? (service::available() ? "已安装且在运行"
                                                 : "已安装但未运行")
                         : "未安装",
                     service::coreRunning() ? "运行中（服务托管）" : "未运行");
        return 0;
    }
    std::println(stderr, "未知 service 子命令：{}", args[0]);
    return 2;
}

int cmdCore(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::println(stderr, "core 需要子命令：start|stop|restart|status");
        return 2;
    }
    auto& core = store::coreStore();
    core.init();
    if (args[0] == "status") {
        // 先探控制器：CLI 是新进程，本地快照不知道别的进程（GUI/detached/
        // 服务）拉起的内核。
        if (const auto r = core.api().version(); r.ok) {
            std::println("内核：运行中{}", service::coreRunning() ? "（服务托管）" : "");
            return 0;
        }
        const auto s = core.snapshot();
        std::println("内核：{}{}", core::stateName(s.state),
                     s.version.empty() ? "" : " · " + s.version);
        if (!s.lastError.empty()) std::println("最近错误：{}", s.lastError);
        return 0;
    }
    if (args[0] == "start" || args[0] == "restart") {
        if (args[0] == "restart") core.stopCore();
        // detached：CLI 退出后内核驻留（setsid + 日志文件 + pidfile）。
        core.startCore(store::profilesStore().selectedYaml(), /*detached=*/true);
        const auto s = core.snapshot();
        if (s.state != core::CoreState::Running) {
            std::println(stderr, "启动失败：{}", s.lastError);
            return 1;
        }
        std::println("内核已启动 · {}", s.version);
        return 0;
    }
    if (args[0] == "stop") {
        core.stopCore();
        std::println("内核已停止");
        return 0;
    }
    std::println(stderr, "未知 core 子命令：{}", args[0]);
    return 2;
}

int cmdMode(const std::vector<std::string>& args) {
    auto& core = store::coreStore();
    core.init();
    if (args.empty()) {
        std::println("{}", core.mode());
        return 0;
    }
    if (args[0] != "rule" && args[0] != "global" && args[0] != "direct") {
        std::println(stderr, "mode 只接受 rule|global|direct");
        return 2;
    }
    if (core.snapshot().state == core::CoreState::Running) {
        if (!core.applyMode(args[0])) {
            std::println(stderr, "切换失败：{}", core.snapshot().lastError);
            return 1;
        }
    } else {
        core.setSetting("core.mode", args[0]);
    }
    std::println("出站模式：{}", args[0]);
    return 0;
}

int cmdTun(const std::vector<std::string>& args) {
    if (args.empty() || (args[0] != "on" && args[0] != "off")) {
        std::println(stderr, "用法：clash-flux tun on|off");
        return 2;
    }
    auto& core = store::coreStore();
    core.init();
    const bool on = args[0] == "on";
    if (!core.applyTun(on)) {
        std::println(stderr, "{}", core.snapshot().lastError);
        return 1;
    }
    std::println("TUN：{}{}", on ? "开" : "关",
                 core.snapshot().state == core::CoreState::Running
                     ? "（已生效）"
                     : "（下次启动内核生效）");
    return 0;
}

int cmdProxy(const std::vector<std::string>& args) {
    auto& core = store::coreStore();
    core.init();
    if (!args.empty() && args[0] == "status") {
        std::println("系统代理：设置={}，桌面当前={}",
                     core.systemProxyEnabled() ? "开" : "关",
                     sysproxy::enabled() ? "手动模式" : "非手动/关闭");
        return 0;
    }
    if (args.empty() || (args[0] != "on" && args[0] != "off")) {
        std::println(stderr, "用法：clash-flux proxy on|off|status");
        return 2;
    }
    const bool on = args[0] == "on";
    if (!core.applySystemProxy(on)) {
        std::println(stderr, "{}", core.snapshot().lastError);
        return 1;
    }
    std::println("系统代理：{}", on ? "开" : "关");
    return 0;
}

int cmdProfile(const std::vector<std::string>& args) {
    auto& ps = store::profilesStore();
    store::coreStore().init();
    if (args.empty() || args[0] == "list") {
        const auto list = ps.list();
        if (list.empty()) {
            std::println("（无订阅，用 profile import <url> 导入）");
            return 0;
        }
        for (const auto& p : list) {
            std::println("{}{} {:<20} {}{}", p.selected ? "*" : " ", p.id,
                         p.name, p.url.empty() ? "本地导入" : p.url,
                         p.error.empty() ? "" : "  [错误: " + p.error + "]");
        }
        return 0;
    }
    if (args[0] == "import") {
        if (args.size() < 2) {
            std::println(stderr, "用法：profile import <url> [名称]");
            return 2;
        }
        const std::int64_t id = ps.importUrl(args.size() > 2 ? args[2] : "",
                                             args[1]);
        if (id == 0) {
            std::println(stderr, "导入失败：{}", ps.lastError());
            return 1;
        }
        std::println("已导入（id={}）", id);
        return 0;
    }
    if (args.size() < 2 ||
        (args[0] != "use" && args[0] != "update" && args[0] != "remove")) {
        std::println(stderr, "用法：profile use|update|remove <id>");
        return 2;
    }
    std::int64_t id = 0;
    try {
        id = std::stoll(args[1]);
    } catch (...) {
        std::println(stderr, "id 无效：{}", args[1]);
        return 2;
    }
    if (args[0] == "use") {
        if (!ps.activate(id)) {
            std::println(stderr, "启用失败：{}", ps.lastError());
            return 1;
        }
        std::println("已启用订阅 {}", id);
        return 0;
    }
    if (args[0] == "update") {
        if (!ps.refresh(id)) {
            std::println(stderr, "更新失败：{}", ps.lastError());
            return 1;
        }
        std::println("已更新订阅 {}", id);
        return 0;
    }
    ps.remove(id);
    if (!ps.lastError().empty()) {
        std::println(stderr, "删除失败：{}", ps.lastError());
        return 1;
    }
    std::println("已删除订阅 {}", id);
    return 0;
}

} // namespace

export int run(const std::vector<std::string>& args) {
#ifdef _WIN32
    // 链接为 GUI 子系统（/SUBSYSTEM:WINDOWS）：从终端启动 CLI 子命令时没有
    // 控制台，附着父进程控制台并重接 stdio，println 才可见。
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        (void)std::freopen("CONOUT$", "w", stdout);
        (void)std::freopen("CONOUT$", "w", stderr);
        (void)std::freopen("CONIN$", "r", stdin);
    }
#endif
    if (args.empty()) {
        printUsage();
        return 0;
    }
    const std::string& cmd = args[0];
    const std::vector<std::string> rest(args.begin() + 1, args.end());
    if (cmd == "version" || cmd == "--version" || cmd == "-v") {
        std::println("Clash-Flux v{}（mihomo 客户端）", CLASHFLUX_VERSION);
        return 0;
    }
    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        printUsage();
        return 0;
    }
    if (cmd == "service") return cmdService(rest);
    if (cmd == "core") return cmdCore(rest);
    if (cmd == "mode") return cmdMode(rest);
    if (cmd == "tun") return cmdTun(rest);
    if (cmd == "proxy") return cmdProxy(rest);
    if (cmd == "profile") return cmdProfile(rest);
    std::println(stderr, "未知命令：{}（help 查看用法）", cmd);
    return 2;
}

} // namespace cli
