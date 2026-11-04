// sysproxy.cppm — clashflux.sysproxy：系统代理写入（Linux / macOS / Windows）。
// iOS/Android 没有本模块管理的桌面系统代理通道。
//
// 通道按平台分流：
//   Windows —— 注册表 HKCU\...\Internet Settings（ProxyEnable/ProxyServer，
//             WinINet 系应用读取），直接调用 InternetSetOption 通知刷新；
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
#include "network_settings.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wininet.h>
#include "win32_raii.h"
#else
#include <cstdio>   // ::popen / ::pclose（读取桌面命令输出）
#endif
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

// POSIX 桌面环境用 shell 调用系统设置工具；Windows 始终走下面的注册表 API，
// 避免 system/_popen 拉起 cmd.exe 后闪出控制台窗口。
#ifdef _WIN32
bool runOk(const std::string&) { return false; }
std::string runCapture(const std::string&) { return {}; }
#else
bool runOk(const std::string& cmd) {
#if defined(CLASHFLUX_IOS)
    static_cast<void>(cmd);
    return false;
#else
    return std::system((cmd + " >/dev/null 2>&1").c_str()) == 0;
#endif
}

std::string runCapture(const std::string& cmd) {
#if defined(CLASHFLUX_IOS)
    static_cast<void>(cmd);
    return {};
#else
    std::string out;
    FILE* fp = ::popen((cmd + " 2>/dev/null").c_str(), "r");
    if (fp) {
        char buf[256];
        while (std::fgets(buf, sizeof buf, fp)) out += buf;
        ::pclose(fp);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    return out;
#endif
}
#endif

std::string shellQuote(std::string_view value) {
    std::string out = "'";
    for (const char c : value) { if (c == '\'') out += "'\\''"; else out += c; }
    return out + "'";
}

bool hasTool(const char* tool) {
    return runOk(std::string("command -v ") + tool);
}

Desktop detect() {
#if defined(__ANDROID__) || defined(CLASHFLUX_IOS)
    // Mobile platforms have no desktop proxy configuration channel. In
    // particular, never probe for shell tools: system() is unavailable on iOS
    // and Android does not provide a desktop shell environment.
    return Desktop::Unsupported;
#else
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
#endif
}

// ---- Linux 通道：KDE / GNOME ---------------------------------------------
#ifdef _WIN32
const char* kwriteTool() { return ""; }
bool kdeSet(const std::string&, const std::string&) { return false; }
void kdeReload() {}
bool gnomeSet(const std::string&, const std::string&) { return false; }
bool kdeProxyManual() { return false; }
#else
const char* kwriteTool() {
    return hasTool("kwriteconfig6") ? "kwriteconfig6" : "kwriteconfig5";
}

bool kdeSet(const std::string& key, const std::string& value) {
    return runOk(std::format(
        "{} --file kioslaverc --group \"Proxy Settings\" --key {} {}",
        kwriteTool(), shellQuote(key), shellQuote(value)));
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
#endif

// ---- Windows 通道：注册表 + WinINet 刷新 ----------------------------------
#ifdef _WIN32
constexpr wchar_t kWinInetRegKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";

bool winReadDword(const wchar_t* valueName, DWORD& value) {
    clashflux::win32::UniqueHkey key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kWinInetRegKey, 0, KEY_QUERY_VALUE,
                      key.put()) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD size = sizeof(value);
    const LONG status = RegQueryValueExW(
        key.get(), valueName, nullptr, &type, reinterpret_cast<BYTE*>(&value),
        &size);
    return status == ERROR_SUCCESS && type == REG_DWORD && size == sizeof(value);
}

std::wstring winUtf8ToWide(std::string_view text) {
    if (text.empty()) return {};
    const int sourceLength = static_cast<int>(text.size());
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           text.data(), sourceLength, nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            sourceLength, result.data(), length) != length) {
        return {};
    }
    return result;
}

std::string winWideToUtf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int sourceLength = static_cast<int>(text.size());
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), sourceLength,
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, text.data(), sourceLength,
                            result.data(), length, nullptr, nullptr) != length) {
        return {};
    }
    return result;
}

std::string winReadString(const wchar_t* valueName) {
    clashflux::win32::UniqueHkey key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kWinInetRegKey, 0, KEY_QUERY_VALUE,
                      key.put()) != ERROR_SUCCESS) {
        return {};
    }
    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key.get(), valueName, nullptr, &type, nullptr, &size) !=
            ERROR_SUCCESS ||
        type != REG_SZ || size < sizeof(wchar_t)) {
        return {};
    }
    std::vector<wchar_t> value(size / sizeof(wchar_t) + 1, L'\0');
    const LONG status = RegQueryValueExW(
        key.get(), valueName, nullptr, &type,
        reinterpret_cast<BYTE*>(value.data()), &size);
    if (status != ERROR_SUCCESS || type != REG_SZ) return {};
    std::size_t length = size / sizeof(wchar_t);
    if (length > 0 && value[length - 1] == L'\0') --length;
    return winWideToUtf8(std::wstring_view(value.data(), length));
}

bool winWriteDword(const wchar_t* valueName, DWORD value) {
    clashflux::win32::UniqueHkey key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kWinInetRegKey, 0, KEY_SET_VALUE,
                      key.put()) != ERROR_SUCCESS) {
        return false;
    }
    const LONG status = RegSetValueExW(
        key.get(), valueName, 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&value), sizeof(value));
    return status == ERROR_SUCCESS;
}

bool winWriteString(const wchar_t* valueName, std::string_view value) {
    const std::wstring wide = winUtf8ToWide(value);
    if (wide.empty() && !value.empty()) return false;
    clashflux::win32::UniqueHkey key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kWinInetRegKey, 0, KEY_SET_VALUE,
                      key.put()) != ERROR_SUCCESS) {
        return false;
    }
    const DWORD size = static_cast<DWORD>((wide.size() + 1) * sizeof(wchar_t));
    const LONG status = RegSetValueExW(
        key.get(), valueName, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(wide.c_str()), size);
    return status == ERROR_SUCCESS;
}

bool winProxyEnabled() {
    DWORD enabled = 0;
    return winReadDword(L"ProxyEnable", enabled) && enabled == 1;
}

// 通知 WinINet 重读代理设置（INTERNET_OPTION_SETTINGS_CHANGED = 39；
// best effort：失败只意味着已在跑的程序下次查询/重启才生效）。
void winNotifyRefresh() {
    InternetSetOptionW(nullptr, INTERNET_OPTION_SETTINGS_CHANGED, nullptr, 0);
    InternetSetOptionW(nullptr, INTERNET_OPTION_REFRESH, nullptr, 0);
}

std::optional<clashflux::network::ProxySnapshot> winCaptureConnection() {
    INTERNET_PER_CONN_OPTIONW options[4]{};
    options[0].dwOption = INTERNET_PER_CONN_FLAGS;
    options[1].dwOption = INTERNET_PER_CONN_PROXY_SERVER;
    options[2].dwOption = INTERNET_PER_CONN_PROXY_BYPASS;
    options[3].dwOption = INTERNET_PER_CONN_AUTOCONFIG_URL;
    INTERNET_PER_CONN_OPTION_LISTW list{};
    list.dwSize = sizeof(list); list.dwOptionCount = 4; list.pOptions = options;
    DWORD size = sizeof(list);
    if (!InternetQueryOptionW(nullptr, INTERNET_OPTION_PER_CONNECTION_OPTION, &list, &size)) return std::nullopt;
    const auto release = [](wchar_t* value) { if (value) GlobalFree(value); };
    std::unique_ptr<wchar_t, decltype(release)> server(options[1].Value.pszValue, release);
    std::unique_ptr<wchar_t, decltype(release)> bypass(options[2].Value.pszValue, release);
    std::unique_ptr<wchar_t, decltype(release)> pac(options[3].Value.pszValue, release);
    clashflux::network::ProxySnapshot snapshot;
    snapshot.desktop = "windows";
    snapshot.values = {{"", "Flags", std::to_string(options[0].Value.dwValue)},
        {"", "ProxyServer", server ? winWideToUtf8(server.get()) : ""},
        {"", "ProxyOverride", bypass ? winWideToUtf8(bypass.get()) : ""},
        {"", "AutoConfigURL", pac ? winWideToUtf8(pac.get()) : ""}};
    return snapshot;
}

bool winRestoreConnection(const clashflux::network::ProxySnapshot& snapshot) {
    INTERNET_PER_CONN_OPTIONW options[4]{};
    options[0].dwOption = INTERNET_PER_CONN_FLAGS;
    options[1].dwOption = INTERNET_PER_CONN_PROXY_SERVER;
    options[2].dwOption = INTERNET_PER_CONN_PROXY_BYPASS;
    options[3].dwOption = INTERNET_PER_CONN_AUTOCONFIG_URL;
    std::wstring server, bypass, pac;
    int flags = 0;
    for (const auto& item : snapshot.values) {
        if (item.key == "Flags" && !clashflux::network::Integer(item.value, flags, 0, 31)) return false;
        if (item.key == "ProxyServer") server = winUtf8ToWide(item.value);
        if (item.key == "ProxyOverride") bypass = winUtf8ToWide(item.value);
        if (item.key == "AutoConfigURL") pac = winUtf8ToWide(item.value);
    }
    options[0].Value.dwValue = static_cast<DWORD>(flags);
    options[1].Value.pszValue = server.data();
    options[2].Value.pszValue = bypass.data();
    options[3].Value.pszValue = pac.data();
    INTERNET_PER_CONN_OPTION_LISTW list{};
    list.dwSize = sizeof(list); list.dwOptionCount = 4; list.pOptions = options;
    const bool ok = InternetSetOptionW(nullptr, INTERNET_OPTION_PER_CONNECTION_OPTION, &list, sizeof(list));
    if (ok) winNotifyRefresh();
    return ok;
}
#else
bool winProxyEnabled() { return false; }
std::string winReadString(const wchar_t*) { return {}; }
bool winWriteDword(const wchar_t*, unsigned long) { return false; }
bool winWriteString(const wchar_t*, std::string_view) { return false; }
void winNotifyRefresh() {}
#endif

// ---- macOS 通道：networksetup ---------------------------------------------
#ifdef _WIN32
std::vector<std::string> macServices() { return {}; }
bool macSetProxy(const std::string&, const std::string&,
                 const std::string&) {
    return false;
}
bool macSetProxyState(const std::string&, std::string_view) { return false; }
#else
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
    return runOk(std::format("networksetup -setwebproxy {} {} {}",
                             shellQuote(svc), shellQuote(host), shellQuote(port))) &&
           runOk(std::format("networksetup -setsecurewebproxy {} {} {}",
                             shellQuote(svc), shellQuote(host), shellQuote(port))) &&
           runOk(std::format("networksetup -setsocksfirewallproxy {} {} {}",
                             shellQuote(svc), shellQuote(host), shellQuote(port)));
}

bool macSetProxyState(const std::string& svc, std::string_view state) {
    return runOk(std::format("networksetup -setwebproxystate {} {}",
                             shellQuote(svc), shellQuote(state))) &&
           runOk(std::format("networksetup -setsecurewebproxystate {} {}",
                             shellQuote(svc), shellQuote(state))) &&
           runOk(std::format("networksetup -setsocksfirewallproxystate {} {}",
                             shellQuote(svc), shellQuote(state)));
}
#endif

} // namespace

// 当前桌面通道（进程内缓存一次探测结果）。
export Desktop desktop() {
    static const Desktop cached = detect();
    return cached;
}

export bool supported() {
    return desktop() != Desktop::Unsupported;
}

export std::optional<clashflux::network::ProxySnapshot> capture(std::string& err);
export std::string currentProxy();
// Includes manual, PAC and automatic discovery channels.
export bool enabled() { return !currentProxy().empty(); }

// 当前系统代理指向（"host:port"；非手动模式、未启用或读取失败返回空）。
// 用于所有权判定：只有指向本应用写入的端口时才允许撤销，避免把其他代理
// 软件（clash-verge/FlClash 等）接管中的设置一并关闭。读不到精确指向的
// 通道（如 Windows 按协议分别配置的 ProxyServer）按原文返回，判定侧自然
// 视为"非本应用"。
export std::string currentProxy() {
    switch (desktop()) {
        case Desktop::Kde: {
            if (!kdeProxyManual()) {
                std::string error;
                if (auto saved = capture(error)) {
                    std::string mode, url;
                    for (const auto& field : saved->values) {
                        if (field.key == "ProxyType") mode = field.value;
                        if (field.key == "Proxy Config Script") url = field.value;
                    }
                    if (mode == "2") return "PAC " + url;
                    if (mode == "3" || mode == "4") return "Auto discovery";
                }
                return {};
            }
            const char* home = std::getenv("HOME");
            if (!home) return {};
            std::ifstream in(std::string(home) + "/.config/kioslaverc");
            if (!in) return {};
            bool inSection = false;
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.front() == '[') {
                    inSection = line == "[Proxy Settings]";
                    continue;
                }
                if (inSection && line.starts_with("httpProxy=")) {
                    std::string value = line.substr(10);
                    // 形如 http://host:port；剥掉 scheme。
                    if (const auto scheme = value.find("://");
                        scheme != std::string::npos) {
                        value = value.substr(scheme + 3);
                    }
                    return value;
                }
            }
            return {};
        }
        case Desktop::Gnome: {
            const auto mode = runCapture("gsettings get org.gnome.system.proxy mode");
            if (mode == "'auto'") return "PAC " + runCapture("gsettings get org.gnome.system.proxy autoconfig-url");
            if (mode != "'manual'") return {};
            // gsettings 输出带单引号：'127.0.0.1'。
            std::string host = runCapture(
                "gsettings get org.gnome.system.proxy.http host");
            const std::string port = runCapture(
                "gsettings get org.gnome.system.proxy.http port");
            host.erase(std::remove(host.begin(), host.end(), '\''),
                       host.end());
            if (host.empty() || port.empty()) return {};
            return host + ":" + port;
        }
        case Desktop::Windows: {
#ifdef _WIN32
            if (auto saved = winCaptureConnection()) {
                std::string flags, pac;
                for (const auto& field : saved->values) {
                    if (field.key == "Flags") flags = field.value;
                    if (field.key == "AutoConfigURL") pac = field.value;
                }
                const auto bits = std::stoul(flags);
                if (bits & PROXY_TYPE_AUTO_PROXY_URL) return "PAC " + pac;
                if (bits & PROXY_TYPE_AUTO_DETECT) return "Auto discovery";
                if (!(bits & PROXY_TYPE_PROXY)) return {};
            } else return {};
#endif
            return winReadString(L"ProxyServer");
        }
        case Desktop::Macos: {
            const std::string out = runCapture(
                R"(networksetup -getwebproxy "Wi-Fi")");
            if (out.find("Enabled: Yes") == std::string::npos) {
                const auto pac = runCapture(R"(networksetup -getautoproxyurl "Wi-Fi")");
                if (pac.find("Enabled: Yes") != std::string::npos) return "PAC " + pac;
                if (runCapture(R"(networksetup -getproxyautodiscovery "Wi-Fi")").ends_with("On")) return "Auto discovery";
                return {};
            }
            std::string host;
            std::string port;
            std::istringstream in(out);
            std::string line;
            while (std::getline(in, line)) {
                if (line.starts_with("Server: ")) host = line.substr(8);
                else if (line.starts_with("Port: ")) port = line.substr(6);
            }
            if (host.empty() || port.empty()) return {};
            return host + ":" + port;
        }
        default:
            return {};
    }
}

// 打开系统代理，指向 host:port（本机内核 mixed 入站端口）。
export bool enable(const std::string& host, int port, std::string& err, bool includeSocks = true) {
    const std::string p = std::to_string(port);
    const auto authority = host.find(':') == std::string::npos ? host : "[" + host + "]";
    switch (desktop()) {
        case Desktop::Kde: {
            bool ok = kdeSet("httpProxy", std::format("http://{}:{}", authority, p)) &&
                      kdeSet("httpsProxy", std::format("http://{}:{}", authority, p)) &&
                      kdeSet("socksProxy", includeSocks ? std::format("socks://{}:{}", authority, p) : "") &&
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
                gnomeSet(".socks", std::format("host '{}'", includeSocks ? host : "")) &&
                gnomeSet(".socks", std::format("port {}", includeSocks ? p : "0")) &&
                gnomeSet("", "mode 'manual'");
            if (!ok) err = "gsettings 写入失败";
            return ok;
        }
        case Desktop::Windows: {
#ifdef _WIN32
            auto saved = winCaptureConnection();
            if (!saved) { err = "无法读取 Windows 系统代理设置"; return false; }
            for (auto& value : saved->values) {
                if (value.key == "Flags") value.value = std::to_string(PROXY_TYPE_DIRECT | PROXY_TYPE_PROXY);
                if (value.key == "ProxyServer") value.value = includeSocks ? authority + ":" + p
                    : "http=" + authority + ":" + p + ";https=" + authority + ":" + p;
            }
            const bool ok = winRestoreConnection(*saved);
#else
            const bool ok = false;
#endif
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
                if (includeSocks) ok = macSetProxy(svc, host, p) && ok;
                else {
                    ok = runOk("networksetup -setwebproxy " + shellQuote(svc) + " " + shellQuote(host) + " " + p) && ok;
                    ok = runOk("networksetup -setsecurewebproxy " + shellQuote(svc) + " " + shellQuote(host) + " " + p) && ok;
                    ok = runOk("networksetup -setsocksfirewallproxystate " + shellQuote(svc) + " off") && ok;
                }
                ok = runOk("networksetup -setautoproxystate " + shellQuote(svc) + " off") && ok;
                ok = runOk("networksetup -setproxyautodiscovery " + shellQuote(svc) + " off") && ok;
            }
            if (!ok) err = "networksetup 写入失败";
            return ok;
        }
        default:
            err = "当前平台不支持系统代理通道";
            return false;
    }
}

// PAC is an application/session setting, not a fabricated sing-box option.
export bool enablePac(const std::string& url, std::string& err) {
    clashflux::network::Advanced validation;
    validation.proxy_mode = "pac_url"; validation.pac_url = url;
    if (!(err = clashflux::network::ValidateAdvanced(validation)).empty()) return false;
    bool ok = false;
    switch (desktop()) {
    case Desktop::Gnome:
        ok = runOk("gsettings set org.gnome.system.proxy autoconfig-url " + shellQuote(url)) &&
             gnomeSet("", "mode 'auto'");
        break;
    case Desktop::Kde:
        ok = kdeSet("Proxy Config Script", url) && kdeSet("ProxyType", "2");
        if (ok) kdeReload();
        break;
    case Desktop::Windows:
#ifdef _WIN32
        if (auto saved = winCaptureConnection()) {
            for (auto& value : saved->values) {
                if (value.key == "Flags") value.value = std::to_string(PROXY_TYPE_DIRECT | PROXY_TYPE_AUTO_PROXY_URL);
                if (value.key == "AutoConfigURL") value.value = url;
            }
            ok = winRestoreConnection(*saved);
            if (ok) winNotifyRefresh();
        }
#endif
        break;
    case Desktop::Macos: {
#ifdef __APPLE__
        const auto services = macServices();
        ok = !services.empty();
        for (const auto& service : services) {
            ok = macSetProxyState(service, "off") && ok;
            ok = runOk("networksetup -setproxyautodiscovery " + shellQuote(service) + " off") && ok;
            ok = runOk("networksetup -setautoproxyurl " + shellQuote(service) + " " + shellQuote(url)) && ok;
            ok = runOk("networksetup -setautoproxystate " + shellQuote(service) + " on") && ok;
        }
#endif
        break;
    }
    default: break;
    }
    if (!ok) err = "系统 PAC 设置写入失败";
    return ok;
}

// Snapshot only settings this module writes. The complete snapshot is also an
// ownership fingerprint, so a user's changed exception list prevents restore.
export std::optional<clashflux::network::ProxySnapshot> capture(std::string& err) {
    clashflux::network::ProxySnapshot result;
    const auto add = [&](std::string scope, std::string key, std::string value, bool present = true) {
        result.values.push_back({std::move(scope), std::move(key), std::move(value), present});
    };
    switch (desktop()) {
    case Desktop::Gnome:
        result.desktop = "gnome";
        for (const auto scope : {"", ".http", ".https", ".socks"}) {
            const std::vector<std::string> keys = std::string_view(scope).empty()
                ? std::vector<std::string>{"mode", "ignore-hosts", "autoconfig-url"} : std::vector<std::string>{"host", "port"};
            for (const auto& key : keys) {
                const auto value = runCapture(std::format("gsettings get org.gnome.system.proxy{} {}", scope, key));
                if (value.empty()) { err = "无法读取 GNOME 系统代理设置"; return std::nullopt; }
                add(scope, key, value);
            }
        }
        break;
    case Desktop::Kde: {
        result.desktop = "kde";
        const char* home = std::getenv("HOME");
        if (!home) { err = "无法定位 KDE 系统代理设置"; return std::nullopt; }
        const auto path = std::filesystem::path(home) / ".config/kioslaverc";
        std::ifstream input(path);
        if (std::filesystem::exists(path) && !input) { err = "无法读取 KDE 系统代理设置"; return std::nullopt; }
        std::map<std::string, std::string> values;
        bool section = false;
        std::string line;
        while (std::getline(input, line)) {
            if (line.starts_with('[')) { section = line == "[Proxy Settings]"; continue; }
            const auto equal = line.find('=');
            if (section && equal != std::string::npos) values[line.substr(0, equal)] = line.substr(equal + 1);
        }
        for (const auto key : {"ProxyType", "httpProxy", "httpsProxy", "socksProxy", "NoProxyFor", "ReversedException", "Proxy Config Script"}) {
            const auto found = values.find(key);
            add("", key, found == values.end() ? "" : found->second, found != values.end());
        }
        break;
    }
    case Desktop::Windows:
#ifdef _WIN32
        if (auto saved = winCaptureConnection()) return saved;
        err = "无法读取 Windows 系统代理设置"; return std::nullopt;
#else
        return std::nullopt;
#endif
    case Desktop::Macos:
        result.desktop = "macos";
        for (const auto& service : macServices()) {
            for (const auto kind : {"web", "secureweb", "socksfirewall"}) {
                const auto raw = runCapture(std::format("networksetup -get{}proxy {}", kind, shellQuote(service)));
                if (!raw.starts_with("Enabled:")) { err = "无法读取 macOS 系统代理设置"; return std::nullopt; }
                if (raw.find("Authenticated Proxy Enabled: 1") != std::string::npos) {
                    err = "原系统代理带有认证，无法完整保存，请先在系统中关闭"; return std::nullopt;
                }
                std::istringstream lines(raw);
                std::string line;
                for (const auto key : {"Enabled", "Server", "Port"}) {
                    lines.clear(); lines.seekg(0);
                    std::string value;
                    while (std::getline(lines, line)) if (line.starts_with(std::string(key) + ": ")) value = line.substr(std::string(key).size() + 2);
                    add(service, std::string(kind) + "." + key, value);
                }
            }
            const auto bypass = runCapture("networksetup -getproxybypassdomains " + shellQuote(service));
            if (bypass.empty()) { err = "无法读取 macOS 代理绕过列表"; return std::nullopt; }
            add(service, "bypass", bypass.starts_with("There aren't any") ? "" : bypass);
            const auto pac = runCapture("networksetup -getautoproxyurl " + shellQuote(service));
            const auto discovery = runCapture("networksetup -getproxyautodiscovery " + shellQuote(service));
            if (pac.find("Enabled:") == std::string::npos || discovery.find(":") == std::string::npos) {
                err = "无法读取 macOS 自动代理状态"; return std::nullopt;
            }
            add(service, "pac", pac);
            add(service, "discovery", discovery);
        }
        if (result.values.empty()) { err = "未找到可用网络服务"; return std::nullopt; }
        break;
    default:
        err = "当前平台不支持系统代理通道"; return std::nullopt;
    }
    return result;
}

export bool setBypass(const std::vector<std::string>& entries, std::string& err) {
    bool ok = true;
    std::string joined;
    for (const auto& entry : entries) { if (!joined.empty()) joined += ';'; joined += entry; }
    switch (desktop()) {
    case Desktop::Kde:
        // KConfig accepts comma-separated exceptions; shell quoting is separate.
        std::replace(joined.begin(), joined.end(), ';', ',');
        ok = runOk(std::format("{} --file kioslaverc --group 'Proxy Settings' --key NoProxyFor {}",
            kwriteTool(), shellQuote(joined))) && kdeSet("ReversedException", "false");
        if (ok) kdeReload();
        break;
    case Desktop::Gnome: {
        std::string array = "[";
        for (const auto& entry : entries) { if (array.size() > 1) array += ','; array += "'" + entry + "'"; }
        array += ']';
        ok = runOk("gsettings set org.gnome.system.proxy ignore-hosts " + shellQuote(array));
        break;
    }
    case Desktop::Windows:
#ifdef _WIN32
        if (auto saved = winCaptureConnection()) {
            for (auto& value : saved->values) if (value.key == "ProxyOverride") value.value = joined;
            ok = winRestoreConnection(*saved);
        } else ok = false;
        break;
#else
        return false;
#endif
    case Desktop::Macos:
        for (const auto& service : macServices()) {
            std::string command = "networksetup -setproxybypassdomains " + shellQuote(service);
            if (entries.empty()) command += " Empty";
            for (const auto& entry : entries) command += " " + shellQuote(entry);
            ok = runOk(command) && ok;
        }
        break;
    default: ok = false;
    }
    if (!ok) err = "系统代理绕过列表写入失败";
    return ok;
}

export bool restore(const clashflux::network::ProxySnapshot& saved, std::string& err) {
    const auto current = capture(err);
    if (!current || current->desktop != saved.desktop) { err = "系统代理平台已变化"; return false; }
    bool ok = true;
    // Only known fields and existing scopes may be replayed; saved strings
    // never become command text or select arbitrary registry/KConfig keys.
    std::set<std::pair<std::string, std::string>> unique;
    for (const auto& item : saved.values) {
        if (!unique.emplace(item.scope, item.key).second) { err = "系统代理快照字段重复"; return false; }
        const auto known = std::ranges::find_if(current->values, [&](const auto& value) {
            return value.scope == item.scope && value.key == item.key;
        });
        if (known == current->values.end()) { err = "系统代理快照字段已失效"; return false; }
    }
    auto fields = *current;
    if (saved.desktop == "kde" && std::ranges::none_of(saved.values, [](const auto& item) { return item.key == "Proxy Config Script"; }))
        std::erase_if(fields.values, [](const auto& item) { return item.key == "Proxy Config Script"; });
    if (saved.values.size() != fields.values.size()) { err = "系统网络服务已变化"; return false; }
    if (saved.desktop == "gnome") {
        // Restore mode last, after all hosts, ports and exceptions.
        for (const auto& item : saved.values) if (item.key != "mode")
            ok = runOk("gsettings set org.gnome.system.proxy" + item.scope + " " + item.key + " " + shellQuote(item.value)) && ok;
        for (const auto& item : saved.values) if (item.key == "mode")
            ok = runOk("gsettings set org.gnome.system.proxy mode " + shellQuote(item.value)) && ok;
    } else if (saved.desktop == "kde") {
        for (const auto& item : saved.values) {
            const auto command = std::format("{} --file kioslaverc --group 'Proxy Settings' --key {} {}",
                kwriteTool(), shellQuote(item.key), item.present ? shellQuote(item.value) : "--delete");
            ok = runOk(command) && ok;
        }
        if (ok) kdeReload();
    } else if (saved.desktop == "windows") {
#ifdef _WIN32
        ok = winRestoreConnection(saved);
#endif
    } else if (saved.desktop == "macos") {
        const auto value = [&](const std::string& scope, const std::string& key) {
            const auto found = std::ranges::find_if(saved.values, [&](const auto& item) { return item.scope == scope && item.key == key; });
            return found == saved.values.end() ? std::string{} : found->value;
        };
        for (const auto& service : macServices()) {
            for (const auto kind : {"web", "secureweb", "socksfirewall"}) {
                const std::string prefix = std::string(kind) + ".";
                const auto host = value(service, prefix + "Server"), port = value(service, prefix + "Port");
                if (!host.empty() && port != "0") ok = runOk(std::format("networksetup -set{}proxy {} {} {}",
                    kind, shellQuote(service), shellQuote(host), shellQuote(port))) && ok;
                ok = runOk(std::format("networksetup -set{}proxystate {} {}", kind, shellQuote(service),
                    value(service, prefix + "Enabled") == "Yes" ? "on" : "off")) && ok;
            }
            std::string command = "networksetup -setproxybypassdomains " + shellQuote(service);
            const auto originalPac = value(service, "pac");
            const auto urlStart = originalPac.find("URL: ");
            if (urlStart != std::string::npos) {
                const auto end = originalPac.find('\n', urlStart);
                const auto url = originalPac.substr(urlStart + 5, end == std::string::npos ? end : end - urlStart - 5);
                ok = runOk("networksetup -setautoproxyurl " + shellQuote(service) + " " + shellQuote(url)) && ok;
            }
            ok = runOk("networksetup -setautoproxystate " + shellQuote(service) +
                (value(service, "pac").find("Enabled: Yes") != std::string::npos ? " on" : " off")) && ok;
            ok = runOk("networksetup -setproxyautodiscovery " + shellQuote(service) +
                (value(service, "discovery").ends_with("On") ? " on" : " off")) && ok;
            const auto entries = clashflux::network::Lines(value(service, "bypass"));
            if (entries.empty()) command += " Empty";
            for (const auto& entry : entries) command += " " + shellQuote(entry);
            ok = runOk(command) && ok;
        }
    } else ok = false;
    if (!ok) err = "原系统代理设置恢复失败";
    return ok;
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
            bool ok = false;
#ifdef _WIN32
            if (auto saved = winCaptureConnection()) {
                for (auto& value : saved->values) if (value.key == "Flags") value.value = std::to_string(PROXY_TYPE_DIRECT);
                ok = winRestoreConnection(*saved);
            }
#endif
            if (ok) winNotifyRefresh();
            if (!ok) err = "reg 写入失败";
            return ok;
        }
        case Desktop::Macos: {
            bool ok = true;
            for (const auto& svc : macServices()) {
                ok = macSetProxyState(svc, "off") && ok;
                ok = runOk("networksetup -setautoproxystate " + shellQuote(svc) + " off") && ok;
                ok = runOk("networksetup -setproxyautodiscovery " + shellQuote(svc) + " off") && ok;
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
