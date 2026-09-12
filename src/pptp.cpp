// pptp.cpp — clashflux.pptp 的平台实现。
//
// Linux：pppd + pptp，pppd 不建立默认路由，只建立 PPP 接口；
// Windows：系统 RAS PPTP，RASEO_RemoteDefaultGateway 不启用，再通过 IP Helper
// API 安装声明的内网路由。
module;

#ifdef _WIN32
#ifndef _WIN32_WINNT
// MIB_IPFORWARD_ROW2 and the *IpForwardEntry2 APIs are Vista-era IP Helper
// APIs. Some Windows CI SDK/toolchain combinations default the target level
// low enough that netioapi.h hides these declarations.
#define _WIN32_WINNT 0x0600
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <ras.h>
#include <raserror.h>
#include <ws2tcpip.h>
#include <cwchar>
#include <limits.h>
#include <stdint.h>
#elif defined(__linux__) && !defined(__ANDROID__)
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>
#include <spawn.h>
#include <poll.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <unistd.h>
#endif

module clashflux.pptp;

import std;
import clashflux.service;
import clashflux.vpn;

namespace pptp {
namespace {

std::string errorText(std::string_view prefix, int code) {
    return std::format("{} ({})", prefix, code);
}

#if defined(__linux__) && !defined(__ANDROID__)

bool executableOnPath(std::string_view name) {
    const char* rawPath = std::getenv("PATH");
    if (rawPath == nullptr) return false;
    std::string path(rawPath);
    std::size_t begin = 0;
    while (begin <= path.size()) {
        const std::size_t end = path.find(':', begin);
        const std::string directory =
            path.substr(begin, end == std::string::npos ? std::string::npos
                                                         : end - begin);
        const std::filesystem::path candidate =
            (directory.empty() ? std::filesystem::path(".")
                               : std::filesystem::path(directory)) /
            name;
        if (::access(candidate.c_str(), X_OK) == 0) return true;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return false;
}

bool linuxToolsAvailable() {
    return executableOnPath("pppd") && executableOnPath("pptp") &&
           executableOnPath("ip");
}

bool validIpv4Cidr(std::string_view value) {
    const std::size_t slash = value.find('/');
    if (slash == std::string_view::npos || slash == 0 ||
        slash + 1 >= value.size() || value.find('/', slash + 1) !=
                                      std::string_view::npos) {
        return false;
    }
    std::size_t begin = 0;
    for (int part = 0; part < 4; ++part) {
        const std::size_t end = value.find('.', begin);
        const std::size_t limit = end == std::string_view::npos ? slash : end;
        if (limit <= begin || limit > slash) return false;
        unsigned int octet = 0;
        const auto [ptr, ec] = std::from_chars(value.data() + begin,
                                               value.data() + limit, octet);
        if (ec != std::errc() || ptr != value.data() + limit || octet > 255) {
            return false;
        }
        if (part == 3 && end != std::string_view::npos) return false;
        if (end == std::string_view::npos) {
            if (part != 3) return false;
            break;
        }
        if (end >= slash) return false;
        begin = end + 1;
    }
    unsigned int prefix = 0;
    const auto [ptr, ec] = std::from_chars(value.data() + slash + 1,
                                           value.data() + value.size(), prefix);
    return ec == std::errc() && ptr == value.data() + value.size() &&
           prefix <= 32;
}

std::string errnoText(std::string_view prefix) {
    return std::format("{}: {}", prefix, std::strerror(errno));
}

bool writePrivateFile(const std::filesystem::path& path,
                      std::string_view content) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    file.close();
    if (!file) return false;
    return ::chmod(path.c_str(), 0600) == 0;
}

std::filesystem::path makePrivateDirectory(std::string& error) {
    const std::filesystem::path root = std::filesystem::temp_directory_path();
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 20; ++attempt) {
        const auto path = root / std::format("clash-flux-pptp-{}-{}-{}",
                                             static_cast<long long>(::getpid()),
                                             static_cast<long long>(stamp),
                                             attempt);
        std::error_code ec;
        if (std::filesystem::create_directory(path, ec) && !ec) {
            if (::chmod(path.c_str(), 0700) != 0) {
                std::filesystem::remove(path, ec);
                error = errnoText("创建 PPTP 私有目录失败");
                return {};
            }
            return path;
        }
    }
    error = "无法创建 PPTP 临时目录";
    return {};
}

std::string escapedPppSecret(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const char c : value) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

std::string readFirstLine(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::string line;
    std::getline(file, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

std::string readLogSummary(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(std::move(line));
    }
    constexpr std::size_t kMaxLines = 3;
    const std::size_t begin = lines.size() > kMaxLines
                                   ? lines.size() - kMaxLines
                                   : 0;
    std::string summary;
    for (std::size_t index = begin; index < lines.size(); ++index) {
        if (!summary.empty()) summary += "；";
        summary += lines[index];
    }
    if (summary.size() > 512) summary.resize(512);
    return summary;
}

void removePrivateDirectory(const std::filesystem::path& path) {
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

bool spawnAndWait(const std::vector<std::string>& args, std::string& error) {
    if (args.empty()) {
        error = "内部错误：空命令";
        return false;
    }
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    pid_t pid = -1;
    const int spawnError = ::posix_spawnp(&pid, args.front().c_str(), nullptr,
                                          nullptr, argv.data(), environ);
    if (spawnError != 0) {
        error = errorText("启动 ip 失败", spawnError);
        return false;
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        error = errnoText("等待 ip 失败");
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        error = std::format("ip 命令失败（退出码 {}）",
                            WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return false;
    }
    return true;
}

struct LinuxSession {
    pid_t pid = -1;
    std::filesystem::path directory;
    std::string interfaceName;
    std::string gateway;
    std::vector<std::string> routes;
};

struct Runtime {
    std::mutex mutex;
    std::unordered_map<std::string, LinuxSession> linuxSessions;
};

bool childExited(pid_t pid, int& status) {
    const pid_t result = ::waitpid(pid, &status, WNOHANG);
    if (result == pid) return true;
    if (result < 0 && errno == ECHILD) return true;
    return false;
}

void stopLinuxSession(LinuxSession session) {
    for (const std::string& route : session.routes) {
        std::string ignored;
        spawnAndWait({"ip", "route", "del", route, "dev", session.interfaceName},
                     ignored);
    }
    if (session.pid > 0) {
        // pppd 及其 pptp/call-manager 子进程在独立进程组中。
        // 向整组发信号，避免超时后留下孤立的 pptp 进程。
        ::kill(-session.pid, SIGTERM);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(3);
        int status = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            if (childExited(session.pid, status)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!childExited(session.pid, status)) {
            ::kill(-session.pid, SIGKILL);
            while (::waitpid(session.pid, &status, 0) < 0 && errno == EINTR) {
            }
        }
    }
    removePrivateDirectory(session.directory);
}

bool connectLinuxPrivileged(const std::shared_ptr<Runtime>& runtime,
                            vpn::VpnConnection& connection,
                            std::string& error) {
    if (::geteuid() != 0) {
        error = "PPTP 建隧道需要 root/CAP_NET_ADMIN；请通过 root 服务运行";
        return false;
    }
    if (!linuxToolsAvailable()) {
        error = "当前系统缺少 pppd、pptp 或 ip 命令";
        return false;
    }
    const auto config = ParsePptpConfig(connection.nativeConfig, error);
    if (!config) return false;
    {
        std::lock_guard lock(runtime->mutex);
        if (runtime->linuxSessions.contains(connection.id)) {
            error = "该 PPTP 连接已经建立";
            return false;
        }
    }

    std::string directoryError;
    const std::filesystem::path directory = makePrivateDirectory(directoryError);
    if (directory.empty()) {
        error = directoryError;
        return false;
    }
    const auto optionsPath = directory / "pppd.options";
    const auto ipUpPath = directory / "ip-up";
    const auto interfacePath = directory / "interface";
    const auto gatewayPath = directory / "gateway";
    const auto logPath = directory / "pppd.log";

    const std::string escapedUser = escapedPppSecret(config->username);
    const std::string escapedPassword = escapedPppSecret(config->password);
    const std::string ipUp =
        "#!/bin/sh\n"
        "printf '%s\\n' \"$1\" > \"" + interfacePath.string() + "\"\n"
        "printf '%s\\n' \"${5-}\" > \"" + gatewayPath.string() + "\"\n";
    const std::string options =
        "pty \"pptp " + config->server + " --nolaunchpppd\"\n"
        "noauth\n"
        "nodetach\n"
        "lock\n"
        "noipdefault\n"
        "nodefaultroute\n"
        "user \"" + escapedUser + "\"\n"
        // 密码只写入 0600 的 options 文件，不出现在 pppd 命令行或日志中。
        "password \"" + escapedPassword + "\"\n"
        "name \"" + escapedUser + "\"\n"
        "remotename PPTP\n"
        "ip-up-script \"" + ipUpPath.string() + "\"\n"
        "logfile \"" + logPath.string() + "\"\n" +
        // MPPE 的密钥派生依赖 MS-CHAP/MS-CHAPv2。服务器若只用普通
        // CHAP，pppd 会在认证成功后仍以退出码 10 终止；启用 MPPE 时
        // 明确拒绝不兼容的认证方式，要求服务器走 MS-CHAPv2。
        (config->requireMppe
             ? "refuse-pap\nrefuse-eap\nrefuse-chap\nrefuse-mschap\n"
               "require-mschap-v2\nrequire-mppe-128\n"
             : "") +
        "mtu 1400\n"
        "mru 1400\n";

    if (!writePrivateFile(ipUpPath, ipUp) ||
        !writePrivateFile(optionsPath, options) ||
        ::chmod(ipUpPath.c_str(), 0700) != 0) {
        error = errnoText("写入 PPTP 临时配置失败");
        removePrivateDirectory(directory);
        return false;
    }

    std::vector<std::string> command{"pppd", "file", optionsPath.string()};
    std::vector<char*> argv;
    argv.reserve(command.size() + 1);
    for (std::string& arg : command) argv.push_back(arg.data());
    argv.push_back(nullptr);
    pid_t pid = -1;
    posix_spawnattr_t attributes;
    int spawnError = ::posix_spawnattr_init(&attributes);
    const bool attributesInitialized = spawnError == 0;
    if (spawnError == 0) {
        spawnError = ::posix_spawnattr_setflags(&attributes,
                                                POSIX_SPAWN_SETPGROUP);
    }
    if (spawnError == 0) {
        spawnError = ::posix_spawnattr_setpgroup(&attributes, 0);
    }
    if (spawnError == 0) {
        spawnError = ::posix_spawnp(&pid, "pppd", nullptr, &attributes,
                                    argv.data(), environ);
    }
    if (attributesInitialized) ::posix_spawnattr_destroy(&attributes);
    if (spawnError != 0) {
        error = errorText("启动 pppd 失败", spawnError);
        removePrivateDirectory(directory);
        return false;
    }

    LinuxSession session{.pid = pid, .directory = directory};
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(config->connectTimeoutSecs);
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const std::string interfaceName = readFirstLine(interfacePath);
        if (!interfaceName.empty()) {
            session.interfaceName = interfaceName;
            session.gateway = readFirstLine(gatewayPath);
            break;
        }
        if (childExited(pid, status)) {
            const int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            const std::string detail = readLogSummary(logPath);
            error = std::format("pppd 提前退出（退出码 {}：PPP 协商失败）{}",
                                exitCode,
                                detail.empty() ? "" : " · " + detail);
            stopLinuxSession(std::move(session));
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (session.interfaceName.empty()) {
        const std::string detail = readLogSummary(logPath);
        error = "PPTP 连接超时，PPP 协商未完成";
        if (!detail.empty()) error += " · " + detail;
        stopLinuxSession(std::move(session));
        return false;
    }
    connection.interfaceName = session.interfaceName;
    connection.gateway = session.gateway;
    {
        std::lock_guard lock(runtime->mutex);
        runtime->linuxSessions.emplace(connection.id, std::move(session));
    }
    return true;
}

bool applyLinuxRoutesPrivileged(const std::shared_ptr<Runtime>& runtime,
                                vpn::VpnConnection& connection,
                                std::span<const std::string> routes,
                                std::string& error) {
    for (const std::string& route : routes) {
        if (!route.empty() && !validIpv4Cidr(route)) {
            error = "PPTP 路由必须是 IPv4 CIDR";
            return false;
        }
    }
    std::lock_guard lock(runtime->mutex);
    const auto it = runtime->linuxSessions.find(connection.id);
    if (it == runtime->linuxSessions.end()) {
        error = "PPTP 会话不存在";
        return false;
    }
    LinuxSession& session = it->second;
    for (const std::string& route : routes) {
        if (route.empty() ||
            std::ranges::find(session.routes, route) != session.routes.end()) {
            continue;
        }
        if (!spawnAndWait({"ip", "route", "replace", route, "dev",
                           session.interfaceName}, error)) {
            for (const std::string& installed : session.routes) {
                std::string ignored;
                spawnAndWait({"ip", "route", "del", installed, "dev",
                              session.interfaceName}, ignored);
            }
            session.routes.clear();
            return false;
        }
        session.routes.push_back(route);
    }
    return true;
}

void disconnectLinuxPrivileged(const std::shared_ptr<Runtime>& runtime,
                               vpn::VpnConnection& connection) {
    LinuxSession session;
    {
        std::lock_guard lock(runtime->mutex);
        const auto it = runtime->linuxSessions.find(connection.id);
        if (it == runtime->linuxSessions.end()) return;
        session = std::move(it->second);
        runtime->linuxSessions.erase(it);
    }
    stopLinuxSession(std::move(session));
    connection.interfaceName.clear();
    connection.gateway.clear();
}

std::shared_ptr<Runtime> privilegedRuntime() {
    static const auto runtime = std::make_shared<Runtime>();
    return runtime;
}

bool connectLinux(const std::shared_ptr<Runtime>& runtime,
                  vpn::VpnConnection& connection, std::string& error) {
    if (!service::available()) {
        error = "Clash-Flux root 服务未运行；请先在设置中安装/启动服务";
        return false;
    }
    if (!service::pptpAvailable()) {
        error = "root 服务侧缺少 pppd、pptp 或 ip 命令";
        return false;
    }
    std::string parseError;
    if (!ParsePptpConfig(connection.nativeConfig, parseError)) {
        error = parseError;
        return false;
    }
    service::PptpSessionInfo session;
    if (!service::startPptp(connection.id, connection.nativeConfig, {}, session,
                             error)) {
        return false;
    }
    connection.interfaceName = std::move(session.interfaceName);
    connection.gateway = std::move(session.gateway);
    return true;
}

bool applyLinuxRoutes(const std::shared_ptr<Runtime>&,
                      vpn::VpnConnection& connection,
                      std::span<const std::string> routes,
                      std::string& error) {
    if (!service::applyPptpRoutes(connection.id, routes, error)) return false;
    return true;
}

void disconnectLinux(const std::shared_ptr<Runtime>&,
                     vpn::VpnConnection& connection) {
    std::string ignored;
    service::stopPptp(connection.id, ignored);
    connection.interfaceName.clear();
    connection.gateway.clear();
}

#elif defined(_WIN32)

struct WindowsRoute {
    MIB_IPFORWARD_ROW2 row{};
};

struct WindowsSession {
    HRASCONN connection = nullptr;
    std::wstring phonebook;
    std::wstring entry;
    ULONG interfaceIndex = 0;
    std::vector<WindowsRoute> routes;
};

struct Runtime {
    std::mutex mutex;
    std::unordered_map<std::string, WindowsSession> windowsSessions;
};

std::string windowsError(std::string_view prefix, DWORD code) {
    return std::format("{}（错误码 {}）", prefix, code);
}

std::optional<std::wstring> utf8ToWide(std::string_view value) {
    if (value.empty()) return std::wstring{};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         value.data(), static_cast<int>(value.size()),
                                         nullptr, 0);
    if (size <= 0) return std::nullopt;
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), out.data(), size) <= 0) {
        return std::nullopt;
    }
    return out;
}

template <std::size_t N>
bool copyWide(wchar_t (&destination)[N], std::wstring_view value) {
    if (value.size() >= N) return false;
    std::wmemcpy(destination, value.data(), value.size());
    destination[value.size()] = L'\0';
    return true;
}

std::vector<ULONG> pppInterfaces() {
    ULONG size = 0;
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr, nullptr,
                             &size) != ERROR_BUFFER_OVERFLOW ||
        size == 0) {
        return {};
    }
    std::vector<unsigned char> buffer(size);
    auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr, adapter,
                             &size) != NO_ERROR) {
        return {};
    }
    std::vector<ULONG> result;
    for (; adapter != nullptr; adapter = adapter->Next) {
        if (adapter->IfType == IF_TYPE_PPP &&
            adapter->OperStatus == IfOperStatusUp) {
            result.push_back(adapter->IfIndex);
        }
    }
    return result;
}

std::optional<ULONG> findPppInterface(const std::vector<ULONG>& before) {
    const auto after = pppInterfaces();
    for (const ULONG index : after) {
        if (std::ranges::find(before, index) == before.end()) return index;
    }
    if (!after.empty()) return after.front();
    return std::nullopt;
}

std::optional<std::pair<IN_ADDR, BYTE>> parseIpv4Cidr(std::string_view value) {
    const std::size_t slash = value.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 >= value.size()) {
        return std::nullopt;
    }
    const std::string address(value.substr(0, slash));
    IN_ADDR parsed{};
    if (InetPtonA(AF_INET, address.c_str(), &parsed) != 1) return std::nullopt;
    BYTE prefix = 0;
    const auto prefixText = value.substr(slash + 1);
    unsigned int parsedPrefix = 0;
    const auto [ptr, ec] = std::from_chars(prefixText.data(),
                                           prefixText.data() + prefixText.size(),
                                           parsedPrefix);
    if (ec != std::errc() || ptr != prefixText.data() + prefixText.size() ||
        parsedPrefix > 32) {
        return std::nullopt;
    }
    prefix = static_cast<BYTE>(parsedPrefix);
    return std::pair{parsed, prefix};
}

void deleteWindowsRoutes(WindowsSession& session) {
    for (const WindowsRoute& route : session.routes) {
        DeleteIpForwardEntry2(&route.row);
    }
    session.routes.clear();
}

bool connectWindows(const std::shared_ptr<Runtime>& runtime,
                    vpn::VpnConnection& connection, std::string& error) {
    const auto config = ParsePptpConfig(connection.nativeConfig, error);
    if (!config) return false;
    const auto server = utf8ToWide(config->server);
    const auto username = utf8ToWide(config->username);
    const auto password = utf8ToWide(config->password);
    if (!server || !username || !password) {
        error = "PPTP 配置不是合法 UTF-8";
        return false;
    }
    {
        std::lock_guard lock(runtime->mutex);
        if (runtime->windowsSessions.contains(connection.id)) {
            error = "该 PPTP 连接已经建立";
            return false;
        }
    }

    const auto before = pppInterfaces();
    const std::filesystem::path phonebookPath =
        std::filesystem::temp_directory_path() /
        std::format("clash-flux-pptp-{}-{}.pbk", GetCurrentProcessId(),
                    GetTickCount64());
    const std::wstring phonebook = phonebookPath.wstring();
    const std::wstring entry = std::format(L"ClashFluxPptp_{}_{}",
                                           GetCurrentProcessId(), GetTickCount64());
    {
        std::ofstream createPhonebook(phonebookPath, std::ios::binary);
        if (!createPhonebook) {
            error = "无法创建 Windows PPTP phonebook";
            return false;
        }
    }

    RASENTRYW rasEntry{};
    rasEntry.dwSize = sizeof(rasEntry);
    rasEntry.dwType = RASET_Vpn;
    rasEntry.dwVpnStrategy = VS_PptpOnly;
    rasEntry.dwFramingProtocol = RASFP_Ppp;
    rasEntry.dwfNetProtocols = RASNP_Ip;
    // 特意不设置 RASEO_RemoteDefaultGateway：公司内网路由由本适配器安装，
    // 默认流量继续交给主 VPN/TUN。
    rasEntry.dwfOptions = config->requireMppe ? RASEO_RequireDataEncryption : 0;
    if (!copyWide(rasEntry.szLocalPhoneNumber, *server)) {
        error = "PPTP server 名称过长";
        DeleteFileW(phonebook.c_str());
        return false;
    }
    const DWORD setResult = RasSetEntryPropertiesW(
        phonebook.c_str(), entry.c_str(), &rasEntry, sizeof(rasEntry), nullptr, 0);
    if (setResult != ERROR_SUCCESS) {
        error = windowsError("创建 Windows PPTP 条目失败", setResult);
        DeleteFileW(phonebook.c_str());
        return false;
    }

    RASDIALPARAMSW params{};
    params.dwSize = sizeof(params);
    if (!copyWide(params.szEntryName, entry) ||
        !copyWide(params.szUserName, *username) ||
        !copyWide(params.szPassword, *password)) {
        error = "PPTP 用户名或条目名过长";
        RasDeleteEntryW(phonebook.c_str(), entry.c_str());
        DeleteFileW(phonebook.c_str());
        return false;
    }

    HRASCONN rasConnection = nullptr;
    const DWORD dialResult = RasDialW(nullptr, phonebook.c_str(), &params, 0,
                                      nullptr, &rasConnection);
    if (dialResult != ERROR_SUCCESS) {
        error = windowsError("Windows PPTP 拨号失败", dialResult);
        if (rasConnection != nullptr) RasHangUp(rasConnection);
        RasDeleteEntryW(phonebook.c_str(), entry.c_str());
        DeleteFileW(phonebook.c_str());
        return false;
    }

    const auto interfaceIndex = findPppInterface(before);
    if (!interfaceIndex) {
        error = "PPTP 已拨号但未找到 PPP 网卡";
        RasHangUp(rasConnection);
        RasDeleteEntryW(phonebook.c_str(), entry.c_str());
        DeleteFileW(phonebook.c_str());
        return false;
    }
    WindowsSession session{.connection = rasConnection,
                           .phonebook = phonebook,
                           .entry = entry,
                           .interfaceIndex = *interfaceIndex};
    connection.interfaceName = std::to_string(*interfaceIndex);
    connection.gateway.clear();
    {
        std::lock_guard lock(runtime->mutex);
        runtime->windowsSessions.emplace(connection.id, std::move(session));
    }
    return true;
}

bool applyWindowsRoutes(const std::shared_ptr<Runtime>& runtime,
                        vpn::VpnConnection& connection,
                        std::span<const std::string> routes,
                        std::string& error) {
    std::lock_guard lock(runtime->mutex);
    const auto it = runtime->windowsSessions.find(connection.id);
    if (it == runtime->windowsSessions.end()) {
        error = "PPTP 会话不存在";
        return false;
    }
    WindowsSession& session = it->second;
    for (const std::string& route : routes) {
        if (std::ranges::find_if(session.routes, [&](const WindowsRoute& installed) {
                const auto& prefix = installed.row.DestinationPrefix;
                const auto parsed = parseIpv4Cidr(route);
                return parsed && prefix.PrefixLength == parsed->second &&
                       prefix.Prefix.Ipv4.sin_addr.S_un.S_addr ==
                           parsed->first.S_un.S_addr;
            }) != session.routes.end()) {
            continue;
        }
        const auto parsed = parseIpv4Cidr(route);
        if (!parsed) {
            error = std::format("PPTP 路由不是合法 IPv4 CIDR: {}", route);
            deleteWindowsRoutes(session);
            return false;
        }
        WindowsRoute nativeRoute;
        InitializeIpForwardEntry(&nativeRoute.row);
        nativeRoute.row.InterfaceIndex = session.interfaceIndex;
        nativeRoute.row.DestinationPrefix.Prefix.si_family = AF_INET;
        nativeRoute.row.DestinationPrefix.Prefix.Ipv4.sin_addr = parsed->first;
        nativeRoute.row.DestinationPrefix.PrefixLength = parsed->second;
        nativeRoute.row.NextHop.si_family = AF_INET;
        nativeRoute.row.NextHop.Ipv4.sin_addr.S_un.S_addr = INADDR_ANY;
        nativeRoute.row.Protocol = MIB_IPPROTO_NETMGMT;
        nativeRoute.row.Metric = 1;
        nativeRoute.row.ValidLifetime = UINT32_MAX;
        nativeRoute.row.PreferredLifetime = UINT32_MAX;
        const DWORD result = CreateIpForwardEntry2(&nativeRoute.row);
        if (result != NO_ERROR) {
            error = windowsError("安装 PPTP 内网路由失败", result);
            deleteWindowsRoutes(session);
            return false;
        }
        session.routes.push_back(nativeRoute);
    }
    return true;
}

void disconnectWindows(const std::shared_ptr<Runtime>& runtime,
                       vpn::VpnConnection& connection) {
    WindowsSession session;
    {
        std::lock_guard lock(runtime->mutex);
        const auto it = runtime->windowsSessions.find(connection.id);
        if (it == runtime->windowsSessions.end()) return;
        session = std::move(it->second);
        runtime->windowsSessions.erase(it);
    }
    deleteWindowsRoutes(session);
    if (session.connection != nullptr) RasHangUp(session.connection);
    RasDeleteEntryW(session.phonebook.c_str(), session.entry.c_str());
    DeleteFileW(session.phonebook.c_str());
    connection.interfaceName.clear();
    connection.gateway.clear();
}

#else

struct Runtime {};

#endif

} // namespace

#if defined(__linux__) && !defined(__ANDROID__)

bool PrivilegedPptpAvailable() {
    return ::geteuid() == 0 && linuxToolsAvailable();
}

bool PrivilegedPptpConnect(std::string_view connectionId,
                           std::string_view nativeConfig,
                           std::span<const std::string> routes,
                           std::string& interfaceName,
                           std::string& gateway,
                           std::string& error) {
    vpn::VpnConnection connection{
        .id = std::string(connectionId),
        .kind = vpn::ConnectionKind::Pptp,
        .enabled = true,
        .nativeConfig = std::string(nativeConfig),
    };
    const auto runtime = privilegedRuntime();
    if (!connectLinuxPrivileged(runtime, connection, error)) return false;
    if (!routes.empty() &&
        !applyLinuxRoutesPrivileged(runtime, connection, routes, error)) {
        disconnectLinuxPrivileged(runtime, connection);
        return false;
    }
    interfaceName = std::move(connection.interfaceName);
    gateway = std::move(connection.gateway);
    return true;
}

bool PrivilegedPptpApplyRoutes(std::string_view connectionId,
                               std::span<const std::string> routes,
                               std::string& error) {
    vpn::VpnConnection connection{.id = std::string(connectionId),
                                  .kind = vpn::ConnectionKind::Pptp};
    return applyLinuxRoutesPrivileged(privilegedRuntime(), connection, routes,
                                      error);
}

void PrivilegedPptpDisconnect(std::string_view connectionId) {
    vpn::VpnConnection connection{.id = std::string(connectionId),
                                  .kind = vpn::ConnectionKind::Pptp};
    disconnectLinuxPrivileged(privilegedRuntime(), connection);
}

void PrivilegedPptpShutdown() {
    const auto runtime = privilegedRuntime();
    std::vector<std::string> ids;
    {
        std::lock_guard lock(runtime->mutex);
        ids.reserve(runtime->linuxSessions.size());
        for (const auto& [id, session] : runtime->linuxSessions) {
            (void)session;
            ids.push_back(id);
        }
    }
    for (const std::string& id : ids) PrivilegedPptpDisconnect(id);
}

#endif

bool PptpToolsAvailable() {
#if defined(__linux__) && !defined(__ANDROID__)
    // 普通进程不直接探测/执行本地 pppd；能力以 root service 的环境为准。
    return service::available() && service::pptpAvailable();
#elif defined(_WIN32)
    return true;
#else
    return false;
#endif
}

vpn::EngineAdapter MakePptpAdapter() {
    const auto runtime = std::make_shared<Runtime>();
    vpn::EngineAdapter adapter{
        .descriptor = vpn::EngineDescriptor{
            .kind = vpn::EngineKind::SystemPptp,
            .priority = 80,
            .available = PptpToolsAvailable(),
            .connectionKinds = {vpn::ConnectionKind::Pptp},
        },
    };
#if defined(__linux__) && !defined(__ANDROID__)
    adapter.connect = [runtime](vpn::VpnConnection& connection,
                                 std::string& error) {
        return connectLinux(runtime, connection, error);
    };
    adapter.applyRoutes = [runtime](vpn::VpnConnection& connection,
                                    std::span<const std::string> routes,
                                    std::string& error) {
        return applyLinuxRoutes(runtime, connection, routes, error);
    };
    adapter.disconnect = [runtime](vpn::VpnConnection& connection) {
        disconnectLinux(runtime, connection);
    };
#elif defined(_WIN32)
    adapter.connect = [runtime](vpn::VpnConnection& connection,
                                std::string& error) {
        return connectWindows(runtime, connection, error);
    };
    adapter.applyRoutes = [runtime](vpn::VpnConnection& connection,
                                    std::span<const std::string> routes,
                                    std::string& error) {
        return applyWindowsRoutes(runtime, connection, routes, error);
    };
    adapter.disconnect = [runtime](vpn::VpnConnection& connection) {
        disconnectWindows(runtime, connection);
    };
#else
    adapter.connect = [](vpn::VpnConnection&, std::string& error) {
        error = "当前平台未实现 PPTP 引擎";
        return false;
    };
    adapter.applyRoutes = {};
    adapter.disconnect = {};
#endif
    return adapter;
}

} // namespace pptp
