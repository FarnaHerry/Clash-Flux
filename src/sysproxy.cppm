// sysproxy.cppm — clashflux.sysproxy：Linux 系统代理写入。
//
// 两个桌面通道（按 XDG_CURRENT_DESKTOP 探测 + 工具可用性兜底）：
//   KDE    —— kwriteconfig6/5 写 ~/.config/kioslaverc [Proxy Settings]
//             （ProxyType 0=关闭 1=手动），dbus-send 通知 KIO 重读；
//   GNOME  —— gsettings 写 org.gnome.system.proxy（mode manual/none，
//             http/https/socks host+port）。
// 全部为阻塞 shell 调用：UI 必须经 RunOnTaskThread 使用。写的只是桌面
// 环境的代理设置，真正生效依赖各应用读对应通道（KDE 应用读 kioslaverc，
// GTK/GLib 应用读 org.gnome.system.proxy）。
module;
#include <cstdio>   // ::popen / ::pclose（读 gsettings 输出）
export module clashflux.sysproxy;

import std;

namespace sysproxy {

export enum class Desktop {
    Unsupported,
    Gnome,
    Kde,
};

namespace {

bool runOk(const std::string& cmd) {
    return std::system((cmd + " >/dev/null 2>&1").c_str()) == 0;
}

std::string runCapture(const std::string& cmd) {
    std::string out;
    if (FILE* fp = ::popen((cmd + " 2>/dev/null").c_str(), "r")) {
        char buf[256];
        while (std::fgets(buf, sizeof buf, fp)) out += buf;
        ::pclose(fp);
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
}

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
        default:
            err = "当前桌面环境不支持（仅 KDE / GNOME 系）";
            return false;
    }
}

// 关闭系统代理（保留已写入的 host/port，仅切模式）。
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
        default:
            err = "当前桌面环境不支持（仅 KDE / GNOME 系）";
            return false;
    }
}

} // namespace sysproxy
