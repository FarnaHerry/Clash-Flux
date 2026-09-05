// sysproxy.cppm — clashflux.sysproxy：系统代理写入（Linux / macOS / Windows）。
//
// 通道按平台分流：
//   Windows —— 注册表 HKCU\...\Internet Settings（ProxyEnable/ProxyServer，
//             WinINet 系应用读取），RUNDLL32 InternetSetOption 通知刷新；
//   macOS   —— networksetup 逐网络服务写 web/secureweb/socks 代理；
//   Linux   —— 按 XDG_CURRENT_DESKTOP 运行时探测 + 工具可用性兜底：
//             KDE    kwriteconfig6/5 写 ~/.config/kioslaverc [Proxy Settings]
//                    （ProxyType 0=关闭 1=手动），dbus-send 通知 KIO 重读；
//             GNOME  gsettings 写 org.gnome.system.proxy（mode manual/none，
//                    http/https/socks host+port）。
// 全部为阻塞 shell 调用：UI 必须经 RunOnTaskThread 使用。写的只是桌面/操作
// 系统的代理设置，真正生效依赖各应用读对应通道（KDE 应用读 kioslaverc，
// GTK/GLib 应用读 org.gnome.system.proxy，WinINet 应用读注册表）。
module;
#include <cstdio>   // ::popen / ::pclose（Windows 为 _popen/_pclose，读命令输出）
export module clashflux.sysproxy;

import std;

namespace sysproxy {

export enum class Desktop {
    Unsupported,
    Gnome,
    Kde,
    Windows,
    Macos,
};

namespace {

// 静默重定向按平台分叉（cmd.exe 没有 /dev/null，用 NUL）。
bool runOk(const std::string& cmd) {
#ifdef _WIN32
    return std::system((cmd + " >NUL 2>&1").c_str()) == 0;
#else
    return std::system((cmd + " >/dev/null 2>&1").c_str()) == 0;
#endif
}

std::string runCapture(const std::string& cmd) {
    std::string out;
#ifdef _WIN32
    FILE* fp = ::_popen((cmd + " 2>NUL").c_str(), "r");
#else
    FILE* fp = ::popen((cmd + " 2>/dev/null").c_str(), "r");
#endif
    if (fp) {
        char buf[256];
        while (std::fgets(buf, sizeof buf, fp)) out += buf;
#ifdef _WIN32
        ::_pclose(fp);
#else
        ::pclose(fp);
#endif
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    return out;
}

bool hasTool(const char* tool) {
    return runOk(std::string("command -v ") + tool);
}

Desktop detect() {
#ifdef _WIN32
    return Desktop::Windows;  // 注册表通道，无需探测
#elif defined(__APPLE__)
    return Desktop::Macos;    // networksetup 通道，无需探测
#else
    const char* env = std::getenv("XDG_CURRENT_DESKTOP");
    const std::string name = env ? env : "";
    const auto contains = [&name](std::string_view needle) {
        return name.find(needle) != std::string::npos;
    };
    if (contains("KDE") &&
        (hasTool("kwriteconfig6") || hasTool("kwriteconfig5"))) {
        return Desktop::Kde;
    }
    if ((contains("GNOME") || contains("Cinnamon") || contains("Budgie") ||
         contains("Pantheon") || contains("Unity")) &&
        hasTool("gsettings") &&
        runOk("gsettings list-schemas | grep -qx org.gnome.system.proxy")) {
        return Desktop::Gnome;
    }
    return Desktop::Unsupported;
#endif
}

// ---- Linux 通道：KDE / GNOME ---------------------------------------------
const char* kwriteTool() {
    return hasTool("kwriteconfig6") ? "kwriteconfig6" : "kwriteconfig5";
}

bool kdeSet(const std::string& key, const std::string& value) {
    return runOk(std::format(
        "{} --file kioslaverc --group \"Proxy Settings\" --key {} \"{}\"",
        kwriteTool(), key, value));
}

void kdeReload() {
    // 通知 KIO 重读代理配置（best effort）。
    runOk("dbus-send --type=signal /KIO/Scheduler "
          "org.kde.KIO.Scheduler.reparseSlaveConfiguration string:''");
}

bool gnomeSet(const std::string& key, const std::string& value) {
    return runOk(std::format("gsettings set org.gnome.system.proxy{} {}",
                             key, value));
}

// 读 kioslaverc [Proxy Settings] 的 ProxyType（不依赖 kreadconfig）。
bool kdeProxyManual() {
    const char* home = std::getenv("HOME");
    if (!home) return false;
    std::ifstream in(std::string(home) + "/.config/kioslaverc");
    if (!in) return false;
    bool inSection = false;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.front() == '[') {
            inSection = line == "[Proxy Settings]";
            continue;
        }
        if (inSection && line.starts_with("ProxyType=")) {
            return line.substr(10) == "1";
        }
    }
    return false;
}

// ---- Windows 通道：注册表 + WinINet 刷新 ----------------------------------
constexpr std::string_view kWinInetRegKey =
    R"(HKCU\Software\Microsoft\Windows\CurrentVersion\Internet Settings)";

// 通知 WinINet 重读代理设置（INTERNET_OPTION_SETTINGS_CHANGED = 39；
// best effort：失败只意味着已在跑的程序下次查询/重启才生效）。
void winNotifyRefresh() {
    runOk("RUNDLL32.EXE WININET.DLL,InternetSetOption 0 39 0 0");
}

// ---- macOS 通道：networksetup ---------------------------------------------
// 列出可用网络服务（带 * 前缀的是 disabled 服务，跳过；首行是说明文字）。
std::vector<std::string> macServices() {
    std::vector<std::string> services;
    std::istringstream in(runCapture("networksetup -listallnetworkservices"));
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line.front() == '*') continue;
        if (line.find("asterisk") != std::string::npos) continue;
        services.push_back(line);
    }
    return services;
}

bool macSetProxy(const std::string& svc, const std::string& host,
                 const std::string& port) {
    return runOk(std::format(R"(networksetup -setwebproxy "{}" {} {})",
                             svc, host, port)) &&
           runOk(std::format(R"(networksetup -setsecurewebproxy "{}" {} {})",
                             svc, host, port)) &&
           runOk(std::format(R"(networksetup -setsocksfirewallproxy "{}" {} {})",
                             svc, host, port));
}

bool macSetProxyState(const std::string& svc, std::string_view state) {
    return runOk(std::format(R"(networksetup -setwebproxystate "{}" {})",
                             svc, state)) &&
           runOk(std::format(R"(networksetup -setsecurewebproxystate "{}" {})",
                             svc, state)) &&
           runOk(std::format(R"(networksetup -setsocksfirewallproxystate "{}" {})",
                             svc, state));
}

} // namespace

// 当前桌面通道（进程内缓存一次探测结果）。
export Desktop desktop() {
    static const Desktop cached = detect();
    return cached;
}

export bool supported() {
    return desktop() != Desktop::Unsupported;
}

// 系统代理当前是否处于手动模式（不要求指向本机端口，仅作状态展示）。
export bool enabled() {
    switch (desktop()) {
        case Desktop::Kde: return kdeProxyManual();
        case Desktop::Gnome:
            return runCapture("gsettings get org.gnome.system.proxy mode") ==
                   "'manual'";
        case Desktop::Windows:
            // reg query 输出形如 "    ProxyEnable    REG_DWORD    0x1"。
            return runCapture(std::format(R"(reg query "{}" /v ProxyEnable)",
                                          kWinInetRegKey))
                       .find("0x1") != std::string::npos;
        case Desktop::Macos:
            // best effort：以 Wi-Fi 服务为代表（多数机器的主用服务）。
            return runCapture(R"(networksetup -getwebproxy "Wi-Fi")")
                       .find("Enabled: Yes") != std::string::npos;
        default: return false;
    }
}

// 打开系统代理，指向 host:port（本机 mihomo mixed 端口）。
export bool enable(const std::string& host, int port, std::string& err) {
    const std::string p = std::to_string(port);
    switch (desktop()) {
        case Desktop::Kde: {
            bool ok = kdeSet("httpProxy", std::format("http://{}:{}", host, p)) &&
                      kdeSet("httpsProxy", std::format("http://{}:{}", host, p)) &&
                      kdeSet("socksProxy", std::format("socks://{}:{}", host, p)) &&
                      kdeSet("ProxyType", "1");
            if (ok) kdeReload();
            if (!ok) err = "kwriteconfig 写入失败";
            return ok;
        }
        case Desktop::Gnome: {
            const bool ok =
                gnomeSet(".http", std::format("host '{}'", host)) &&
                gnomeSet(".http", std::format("port {}", p)) &&
                gnomeSet(".https", std::format("host '{}'", host)) &&
                gnomeSet(".https", std::format("port {}", p)) &&
                gnomeSet(".socks", std::format("host '{}'", host)) &&
                gnomeSet(".socks", std::format("port {}", p)) &&
                gnomeSet("", "mode 'manual'");
            if (!ok) err = "gsettings 写入失败";
            return ok;
        }
        case Desktop::Windows: {
            const bool ok = runOk(std::format(
                                R"(reg add "{}" /v ProxyServer /t REG_SZ /d "{}:{}" /f)",
                                kWinInetRegKey, host, p)) &&
                            runOk(std::format(
                                R"(reg add "{}" /v ProxyEnable /t REG_DWORD /d 1 /f)",
                                kWinInetRegKey));
            if (ok) winNotifyRefresh();
            if (!ok) err = "reg 写入失败";
            return ok;
        }
        case Desktop::Macos: {
            const auto services = macServices();
            if (services.empty()) {
                err = "networksetup 未列出可用网络服务";
                return false;
            }
            bool ok = true;
            for (const auto& svc : services) {
                ok = macSetProxy(svc, host, p) && ok;
            }
            if (!ok) err = "networksetup 写入失败";
            return ok;
        }
        default:
            err = "当前平台不支持系统代理通道";
            return false;
    }
}

// 关闭系统代理（保留已写入的 host/port，仅切模式/状态）。
export bool disable(std::string& err) {
    switch (desktop()) {
        case Desktop::Kde: {
            const bool ok = kdeSet("ProxyType", "0");
            if (ok) kdeReload();
            if (!ok) err = "kwriteconfig 写入失败";
            return ok;
        }
        case Desktop::Gnome: {
            const bool ok = gnomeSet("", "mode 'none'");
            if (!ok) err = "gsettings 写入失败";
            return ok;
        }
        case Desktop::Windows: {
            const bool ok = runOk(std::format(
                R"(reg add "{}" /v ProxyEnable /t REG_DWORD /d 0 /f)",
                kWinInetRegKey));
            if (ok) winNotifyRefresh();
            if (!ok) err = "reg 写入失败";
            return ok;
        }
        case Desktop::Macos: {
            bool ok = true;
            for (const auto& svc : macServices()) {
                ok = macSetProxyState(svc, "off") && ok;
            }
            if (!ok) err = "networksetup 写入失败";
            return ok;
        }
        default:
            err = "当前平台不支持系统代理通道";
            return false;
    }
}

} // namespace sysproxy
