// openvpn.cpp — clashflux.openvpn 的桌面 OpenVPN CLI 实现。
module;

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#elif defined(__linux__) && !defined(__ANDROID__)
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <net/if.h>
#include <signal.h>
#include <spawn.h>
#include <cerrno>
#include <cstring>
#include <unistd.h>
extern char** environ;
#endif

module clashflux.openvpn;

import std;
import clashflux.service;
import clashflux.vpn;
import clashflux.vpn_compensation;

namespace openvpn {
namespace {

#if defined(__linux__) && !defined(__ANDROID__)

bool executableOnPath(std::string_view name) {
    const char* rawPath = std::getenv("PATH");
    if (rawPath == nullptr) return false;
    std::string path(rawPath);
    std::size_t begin = 0;
    while (begin <= path.size()) {
        const std::size_t end = path.find(':', begin);
        const std::filesystem::path directory =
            end == std::string::npos
                ? std::filesystem::path(path.substr(begin))
                : std::filesystem::path(path.substr(begin, end - begin));
        const auto candidate = (directory.empty() ? std::filesystem::path(".")
                                                   : directory) /
                               name;
        if (::access(candidate.c_str(), X_OK) == 0) return true;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return false;
}

bool linuxToolsAvailable() {
    return executableOnPath("openvpn") && executableOnPath("ip");
}

std::string errnoText(std::string_view prefix) {
    return std::format("{}: {}", prefix, std::strerror(errno));
}

bool writePrivateFile(const std::filesystem::path& path,
                      std::string_view content, mode_t mode = 0600) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    file.close();
    return static_cast<bool>(file) && ::chmod(path.c_str(), mode) == 0;
}

std::filesystem::path makePrivateDirectory(std::string& error) {
    const auto root = std::filesystem::temp_directory_path();
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 20; ++attempt) {
        const auto path = root / std::format("clash-flux-openvpn-{}-{}-{}",
                                             static_cast<long long>(::getpid()),
                                             static_cast<long long>(stamp), attempt);
        std::error_code ec;
        if (std::filesystem::create_directory(path, ec) && !ec) {
            if (::chmod(path.c_str(), 0700) != 0) {
                std::filesystem::remove(path, ec);
                error = errnoText("创建 OpenVPN 私有目录失败");
                return {};
            }
            return path;
        }
    }
    error = "无法创建 OpenVPN 临时目录";
    return {};
}

void removePrivateDirectory(const std::filesystem::path& path) {
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
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
    const std::size_t begin = lines.size() > 3 ? lines.size() - 3 : 0;
    std::string result;
    for (std::size_t i = begin; i < lines.size(); ++i) {
        if (!result.empty()) result += "；";
        result += lines[i];
    }
    if (result.size() > 512) result.resize(512);
    return result;
}

std::string shellQuote(std::string_view value) {
    std::string result{"'"};
    for (const char c : value) {
        if (c == '\'') result += "'\\''";
        else result.push_back(c);
    }
    result.push_back('\'');
    return result;
}

struct LinuxSession {
    pid_t pid = -1;
    pid_t processGroup = -1;
    unsigned int interfaceIndex = 0;
    std::filesystem::path directory;
    std::string interfaceName;
    std::string gateway;
    std::vector<vpn::compensation::TransportLease> transports;
    vpn::compensation::RouteLease boundInterface;
    std::vector<vpn::compensation::RouteLease> routes;
};

struct Runtime {
    std::mutex mutex;
    std::unordered_map<std::string, LinuxSession> sessions;
};

std::shared_ptr<Runtime> privilegedRuntime() {
    static const auto runtime = std::make_shared<Runtime>();
    return runtime;
}

bool childExited(pid_t pid, int& status) {
    const pid_t result = ::waitpid(pid, &status, WNOHANG);
    return result == pid || (result < 0 && errno == ECHILD);
}

void stopSession(LinuxSession session) {
    if (session.processGroup > 0) {
        // The dialer and its helpers share an owned process group. Keep the
        // physical bypass until all live helpers and the VPN interface are gone.
        ::kill(-session.processGroup, SIGTERM);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(3);
        int status = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            if (session.pid > 0 && childExited(session.pid, status)) session.pid = -1;
            if (::kill(-session.processGroup, 0) < 0 && errno == ESRCH) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        // pppd/OpenVPN may exit before one of their helper processes does.
        // Always kill remaining group members even if the parent was reaped.
        ::kill(-session.processGroup, SIGKILL);
        if (session.pid > 0) {
            while (::waitpid(session.pid, &status, 0) < 0 && errno == EINTR) {
            }
        }
    }
    // Closing the daemon's descriptors normally removes the interface; a killed
    // helper may still be exiting. Allow that teardown before dropping its bypass.
    const auto interfaceDeadline = std::chrono::steady_clock::now() +
                                   std::chrono::seconds(1);
    while (session.interfaceIndex != 0 &&
           ::if_nametoindex(session.interfaceName.c_str()) == session.interfaceIndex &&
           std::chrono::steady_clock::now() < interfaceDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    session.routes.clear();
    session.boundInterface.reset();
    session.transports.clear();
    removePrivateDirectory(session.directory);
}

bool connectPrivileged(const std::shared_ptr<Runtime>& runtime,
                       vpn::VpnConnection& connection, std::string& error) {
    if (::geteuid() != 0) {
        error = "OpenVPN 建隧道需要 root/CAP_NET_ADMIN；请通过 root 服务运行";
        return false;
    }
    if (!linuxToolsAvailable()) {
        error = "当前系统缺少 openvpn 或 ip 命令";
        return false;
    }
    std::string parseError;
    const auto config = ParseOpenVpnConfig(connection.nativeConfig, parseError);
    if (!config) {
        error = parseError;
        return false;
    }
    {
        std::lock_guard lock(runtime->mutex);
        if (runtime->sessions.contains(connection.id)) {
            error = "该 OpenVPN 连接已经建立";
            return false;
        }
    }

    std::vector<vpn::compensation::TransportLease> transports;
    std::string managedConfig = config->configText;
    // Replace backwards so offsets remain valid, including per-connection remotes.
    for (const auto& remote : config->remotes | std::views::reverse) {
        auto transport = vpn::compensation::PrepareTransport(remote.host, error);
        if (!transport) return false;
        managedConfig.replace(remote.offset, remote.length, transport->address);
        transports.push_back(std::move(*transport));
    }
    if (transports.empty()) {
        error = "托管 OpenVPN 配置需要至少一个 remote 服务器地址";
        return false;
    }

    const auto directory = makePrivateDirectory(error);
    if (directory.empty()) return false;
    const auto configPath = directory / "managed.ovpn";
    const auto routeUpPath = directory / "route-up";
    const auto interfacePath = directory / "interface";
    const auto gatewayPath = directory / "gateway";
    const auto logPath = directory / "openvpn.log";

    const std::string routeUp =
        "#!/bin/sh\n"
        "printf '%s\\n' \"${dev-}\" > " + shellQuote(interfacePath.string()) + "\n"
        "printf '%s\\n' \"${route_vpn_gateway-}\" > " +
        shellQuote(gatewayPath.string()) + "\n";
    if (!managedConfig.empty() && managedConfig.back() != '\n') managedConfig += '\n';
    managedConfig += "route-nopull\nroute-noexec\nscript-security 2\n";
    managedConfig += "route-up " + shellQuote(routeUpPath.string()) + "\n";
    managedConfig += "log " + shellQuote(logPath.string()) + "\n";

    if (!writePrivateFile(routeUpPath, routeUp, 0700) ||
        !writePrivateFile(configPath, managedConfig, 0600)) {
        error = errnoText("写入 OpenVPN 临时配置失败");
        removePrivateDirectory(directory);
        return false;
    }

    std::vector<std::string> command{"openvpn", "--config", configPath.string(),
                                     "--route-noexec", "--route-nopull",
                                     "--script-security", "2"};
    std::vector<char*> argv;
    argv.reserve(command.size() + 1);
    for (auto& arg : command) argv.push_back(arg.data());
    argv.push_back(nullptr);
    pid_t pid = -1;
    posix_spawnattr_t attributes;
    int spawnError = ::posix_spawnattr_init(&attributes);
    const bool attributesInitialized = spawnError == 0;
    if (spawnError == 0) {
        spawnError = ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    }
    if (spawnError == 0) {
        spawnError = ::posix_spawnattr_setpgroup(&attributes, 0);
    }
    if (spawnError == 0) {
        spawnError = ::posix_spawnp(&pid, "openvpn", nullptr, &attributes,
                                  argv.data(), environ);
    }
    if (attributesInitialized) ::posix_spawnattr_destroy(&attributes);
    if (spawnError != 0) {
        error = std::format("启动 openvpn 失败：{}", std::strerror(spawnError));
        removePrivateDirectory(directory);
        return false;
    }

    LinuxSession session{.pid = pid, .processGroup = pid, .directory = directory,
                         .transports = std::move(transports)};
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(config->connectTimeoutSecs);
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        session.interfaceName = readFirstLine(interfacePath);
        if (!session.interfaceName.empty()) {
            session.gateway = readFirstLine(gatewayPath);
            break;
        }
        if (childExited(pid, status)) {
            const int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            const std::string summary = readLogSummary(logPath);
            error = std::format("openvpn 提前退出（退出码 {}）{}", exitCode,
                                summary.empty() ? "" : " · " + summary);
            stopSession(std::move(session));
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (session.interfaceName.empty()) {
        error = "OpenVPN 连接超时，未得到 tun 接口";
        stopSession(std::move(session));
        return false;
    }
    session.interfaceIndex = ::if_nametoindex(session.interfaceName.c_str());
    if (session.interfaceIndex == 0) {
        error = "原生 VPN 接口在连接期间消失";
        stopSession(std::move(session));
        return false;
    }
    session.boundInterface = vpn::compensation::PrepareBoundInterface(
        session.interfaceName, error);
    if (!session.boundInterface) {
        stopSession(std::move(session));
        return false;
    }
    connection.interfaceName = session.interfaceName;
    connection.gateway = session.gateway;
    {
        std::lock_guard lock(runtime->mutex);
        runtime->sessions.emplace(connection.id, std::move(session));
    }
    return true;
}

bool applyRoutesPrivileged(const std::shared_ptr<Runtime>& runtime,
                           vpn::VpnConnection& connection,
                           std::span<const std::string> routes,
                           std::string& error) {
    std::lock_guard lock(runtime->mutex);
    const auto it = runtime->sessions.find(connection.id);
    if (it == runtime->sessions.end()) {
        error = "OpenVPN 会话不存在";
        return false;
    }
    auto& session = it->second;
    return vpn::compensation::ReplaceNativeRoutes(
        session.interfaceName, routes, session.routes, error);
}

void disconnectPrivileged(const std::shared_ptr<Runtime>& runtime,
                          vpn::VpnConnection& connection) {
    LinuxSession session;
    {
        std::lock_guard lock(runtime->mutex);
        const auto it = runtime->sessions.find(connection.id);
        if (it == runtime->sessions.end()) return;
        session = std::move(it->second);
        runtime->sessions.erase(it);
    }
    stopSession(std::move(session));
    connection.interfaceName.clear();
    connection.gateway.clear();
}

bool connectUser(const std::shared_ptr<Runtime>&,
                 vpn::VpnConnection& connection, std::string& error) {
    std::string serviceError;
    if (!service::ensureCompatible(serviceError)) {
        error = serviceError.empty()
                    ? "Clash-Flux root 服务未运行；请先在设置中安装/启动服务"
                    : serviceError;
        return false;
    }
    if (!service::openvpnAvailable()) {
        error = "root 服务侧缺少 openvpn 或 ip 命令";
        return false;
    }
    std::string parseError;
    if (!ParseOpenVpnConfig(connection.nativeConfig, parseError)) {
        error = parseError;
        return false;
    }
    service::OpenVpnSessionInfo session;
    if (!service::startOpenVpn(connection.id, connection.nativeConfig, {}, session,
                               error)) {
        return false;
    }
    connection.interfaceName = std::move(session.interfaceName);
    connection.gateway = std::move(session.gateway);
    return true;
}

bool applyUserRoutes(const std::shared_ptr<Runtime>&,
                     vpn::VpnConnection& connection,
                     std::span<const std::string> routes, std::string& error) {
    return service::applyOpenVpnRoutes(connection.id, routes, error);
}

void disconnectUser(const std::shared_ptr<Runtime>&,
                    vpn::VpnConnection& connection) {
    std::string ignored;
    service::stopOpenVpn(connection.id, ignored);
    connection.interfaceName.clear();
    connection.gateway.clear();
}

#elif defined(_WIN32)

struct WindowsRoute {
    MIB_IPFORWARDROW row{};
    bool owned = false;
};

struct WindowsAdapter {
    ULONG index = 0;
    std::string name;
};

struct WindowsSession {
    HANDLE process = nullptr;
    std::filesystem::path directory;
    WindowsAdapter adapter;
    std::vector<WindowsRoute> routes;
};

struct Runtime {
    std::mutex mutex;
    std::unordered_map<std::string, WindowsSession> sessions;
};

std::shared_ptr<Runtime> windowsRuntime() {
    static const auto runtime = std::make_shared<Runtime>();
    return runtime;
}

std::string windowsError(std::string_view prefix, DWORD code) {
    return std::format("{}（错误码 {}）", prefix, code);
}

std::optional<std::filesystem::path> openVpnExecutable() {
    std::vector<wchar_t> found(32768);
    if (SearchPathW(nullptr, L"openvpn.exe", nullptr,
                    static_cast<DWORD>(found.size()), found.data(), nullptr) > 0) {
        return std::filesystem::path(found.data());
    }
    for (const wchar_t* variable : {L"ProgramFiles", L"ProgramW6432"}) {
        std::vector<wchar_t> root(32768);
        const DWORD size = GetEnvironmentVariableW(variable, root.data(),
                                                    static_cast<DWORD>(root.size()));
        if (size == 0 || size >= root.size()) continue;
        const auto candidate = std::filesystem::path(root.data()) /
                               L"OpenVPN" / L"bin" / L"openvpn.exe";
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) return candidate;
    }
    return std::nullopt;
}

std::vector<WindowsAdapter> activeAdapters() {
    ULONG size = 0;
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr, nullptr,
                             &size) != ERROR_BUFFER_OVERFLOW || size == 0) return {};
    std::vector<unsigned char> buffer(size);
    auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr, adapter,
                             &size) != NO_ERROR) return {};
    std::vector<WindowsAdapter> result;
    for (; adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus == IfOperStatusUp && adapter->IfIndex != 0) {
            result.push_back({adapter->IfIndex,
                              adapter->AdapterName ? adapter->AdapterName : ""});
        }
    }
    return result;
}

std::wstring quoteWindowsArgument(std::wstring_view value) {
    std::wstring result{L"\""};
    std::size_t slashes = 0;
    for (const wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'\"') result.append(slashes * 2 + 1, L'\\');
        else result.append(slashes, L'\\');
        slashes = 0;
        result.push_back(c);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

std::string readWindowsLog(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(std::move(line));
    }
    std::string result;
    for (std::size_t i = lines.size() > 3 ? lines.size() - 3 : 0;
         i < lines.size(); ++i) {
        if (!result.empty()) result += "；";
        result += lines[i];
    }
    if (result.size() > 512) result.resize(512);
    return result;
}

void deleteWindowsRoutes(WindowsSession& session) {
    for (auto& route : session.routes) if (route.owned) DeleteIpForwardEntry(&route.row);
    session.routes.clear();
}

void stopWindowsSession(WindowsSession& session) {
    deleteWindowsRoutes(session);
    if (session.process != nullptr) {
        if (WaitForSingleObject(session.process, 0) == WAIT_TIMEOUT) {
            TerminateProcess(session.process, 1);
            WaitForSingleObject(session.process, 3000);
        }
        CloseHandle(session.process);
    }
    std::error_code ec;
    std::filesystem::remove_all(session.directory, ec);
}

bool connectWindows(const std::shared_ptr<Runtime>& runtime,
                    vpn::VpnConnection& connection, std::string& error) {
    const auto executable = openVpnExecutable();
    if (!executable) { error = "未找到 openvpn.exe；请安装 OpenVPN Community"; return false; }
    const auto config = ParseOpenVpnConfig(connection.nativeConfig, error);
    if (!config) return false;
    {
        std::lock_guard lock(runtime->mutex);
        if (runtime->sessions.contains(connection.id)) {
            error = "该 OpenVPN 连接已经建立";
            return false;
        }
    }
    const auto before = activeAdapters();
    const auto directory = std::filesystem::temp_directory_path() /
        std::format("clash-flux-openvpn-{}-{}", GetCurrentProcessId(), GetTickCount64());
    std::error_code ec;
    if (!std::filesystem::create_directory(directory, ec) || ec) {
        error = "无法创建 OpenVPN 临时目录";
        return false;
    }
    const auto configPath = directory / "managed.ovpn";
    const auto logPath = directory / "openvpn.log";
    std::string managed = config->configText;
    if (!managed.empty() && managed.back() != '\n') managed += '\n';
    managed += "route-nopull\nroute-noexec\n";
    {
        std::ofstream file(configPath, std::ios::binary | std::ios::trunc);
        file.write(managed.data(), static_cast<std::streamsize>(managed.size()));
        if (!file) {
            error = "写入 OpenVPN 临时配置失败";
            std::filesystem::remove_all(directory, ec);
            return false;
        }
    }
    std::wstring command = quoteWindowsArgument(executable->wstring()) + L" --config " +
        quoteWindowsArgument(configPath.wstring()) + L" --route-nopull --route-noexec --log " +
        quoteWindowsArgument(logPath.wstring());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable->c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &process)) {
        error = windowsError("启动 openvpn.exe 失败", GetLastError());
        std::filesystem::remove_all(directory, ec);
        return false;
    }
    CloseHandle(process.hThread);
    WindowsSession session{.process = process.hProcess, .directory = directory};
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(config->connectTimeoutSecs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (WaitForSingleObject(session.process, 0) != WAIT_TIMEOUT) {
            DWORD exitCode = 0;
            GetExitCodeProcess(session.process, &exitCode);
            error = std::format("openvpn.exe 提前退出（退出码 {}）", exitCode);
            const auto detail = readWindowsLog(logPath);
            if (!detail.empty()) error += " · " + detail;
            stopWindowsSession(session);
            return false;
        }
        for (const auto& adapter : activeAdapters()) {
            if (std::ranges::none_of(before, [&](const auto& old) {
                    return old.index == adapter.index;
                })) {
                session.adapter = adapter;
                break;
            }
        }
        if (session.adapter.index != 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (session.adapter.index == 0) {
        error = "OpenVPN 连接超时，未检测到新的活动网卡";
        const auto detail = readWindowsLog(logPath);
        if (!detail.empty()) error += " · " + detail;
        stopWindowsSession(session);
        return false;
    }
    connection.interfaceName = session.adapter.name;
    connection.gateway.clear();
    std::lock_guard lock(runtime->mutex);
    runtime->sessions.emplace(connection.id, std::move(session));
    return true;
}

bool applyWindowsRoutes(const std::shared_ptr<Runtime>& runtime,
                        vpn::VpnConnection& connection,
                        std::span<const std::string> routes, std::string& error) {
    std::lock_guard lock(runtime->mutex);
    const auto found = runtime->sessions.find(connection.id);
    if (found == runtime->sessions.end()) { error = "OpenVPN 会话不存在"; return false; }
    auto& session = found->second;
    std::vector<WindowsRoute> desired;
    std::vector<WindowsRoute> created;
    auto same = [](const WindowsRoute& a, const WindowsRoute& b) {
        return a.row.dwForwardDest == b.row.dwForwardDest &&
               a.row.dwForwardMask == b.row.dwForwardMask &&
               a.row.dwForwardIfIndex == b.row.dwForwardIfIndex;
    };
    auto rollback = [&] {
        for (auto& item : created) DeleteIpForwardEntry(&item.row);
    };
    for (const auto& route : routes) {
        const auto normalized = vpn::compensation::NormalizeIpv4Cidr(route);
        if (!normalized) {
            rollback();
            error = "OpenVPN 内网路由必须是非默认 IPv4 CIDR";
            return false;
        }
        const auto slash = normalized->find('/');
        IN_ADDR address{};
        if (InetPtonA(AF_INET, normalized->substr(0, slash).c_str(), &address) != 1) {
            rollback();
            error = "OpenVPN 内网路由地址无效";
            return false;
        }
        unsigned int prefix = 0;
        std::from_chars(normalized->data() + slash + 1,
                        normalized->data() + normalized->size(), prefix);
        WindowsRoute item;
        item.row.dwForwardMask = prefix == 0 ? 0 : htonl(0xffffffffu << (32 - prefix));
        item.row.dwForwardDest = address.S_un.S_addr & item.row.dwForwardMask;
        item.row.dwForwardIfIndex = session.adapter.index;
        item.row.dwForwardNextHop = INADDR_ANY;
        item.row.dwForwardType = MIB_IPROUTE_TYPE_DIRECT;
        item.row.dwForwardProto = MIB_IPPROTO_NETMGMT;
        item.row.dwForwardMetric1 = 1;
        if (std::ranges::any_of(desired, [&](const auto& current) { return same(item, current); })) continue;
        const auto old = std::ranges::find_if(session.routes,
            [&](const auto& current) { return same(item, current); });
        if (old != session.routes.end()) { desired.push_back(*old); continue; }
        const DWORD result = CreateIpForwardEntry(&item.row);
        if (result == NO_ERROR) {
            item.owned = true;
            created.push_back(item);
        }
        else if (result != ERROR_OBJECT_ALREADY_EXISTS && result != ERROR_ALREADY_EXISTS) {
            rollback();
            error = windowsError("安装 OpenVPN 内网路由失败", result);
            return false;
        }
        desired.push_back(item);
    }
    for (auto& old : session.routes) {
        if (old.owned && std::ranges::none_of(desired,
            [&](const auto& current) { return same(old, current); })) DeleteIpForwardEntry(&old.row);
    }
    session.routes = std::move(desired);
    return true;
}

void disconnectWindows(const std::shared_ptr<Runtime>& runtime,
                       vpn::VpnConnection& connection) {
    WindowsSession session;
    {
        std::lock_guard lock(runtime->mutex);
        const auto found = runtime->sessions.find(connection.id);
        if (found == runtime->sessions.end()) return;
        session = std::move(found->second);
        runtime->sessions.erase(found);
    }
    stopWindowsSession(session);
    connection.interfaceName.clear();
    connection.gateway.clear();
}

#endif

} // namespace

#if defined(__linux__) && !defined(__ANDROID__)

bool PrivilegedOpenVpnAvailable() {
    return ::geteuid() == 0 && linuxToolsAvailable();
}

bool PrivilegedOpenVpnSessionAlive(std::string_view connectionId) {
    const auto runtime = privilegedRuntime();
    std::lock_guard lock(runtime->mutex);
    const auto it = runtime->sessions.find(std::string(connectionId));
    if (it == runtime->sessions.end()) return false;
    auto& session = it->second;
    if (session.pid <= 0) return false;
    int status = 0;
    if (childExited(session.pid, status)) {
        session.pid = -1;
        return false;
    }
    return session.interfaceIndex != 0 &&
           ::if_nametoindex(session.interfaceName.c_str()) == session.interfaceIndex;
}

bool PrivilegedOpenVpnConnect(std::string_view connectionId,
                              std::string_view nativeConfig,
                              std::span<const std::string> routes,
                              std::string& interfaceName,
                              std::string& gateway,
                              std::string& error) {
    vpn::VpnConnection connection{.id = std::string(connectionId),
                                  .kind = vpn::ConnectionKind::OpenVpn,
                                  .enabled = true,
                                  .nativeConfig = std::string(nativeConfig)};
    const auto runtime = privilegedRuntime();
    if (!connectPrivileged(runtime, connection, error)) return false;
    if (!routes.empty() && !applyRoutesPrivileged(runtime, connection, routes, error)) {
        disconnectPrivileged(runtime, connection);
        return false;
    }
    interfaceName = std::move(connection.interfaceName);
    gateway = std::move(connection.gateway);
    return true;
}

bool PrivilegedOpenVpnApplyRoutes(std::string_view connectionId,
                                  std::span<const std::string> routes,
                                  std::string& error) {
    vpn::VpnConnection connection{.id = std::string(connectionId),
                                  .kind = vpn::ConnectionKind::OpenVpn};
    return applyRoutesPrivileged(privilegedRuntime(), connection, routes, error);
}

void PrivilegedOpenVpnDisconnect(std::string_view connectionId) {
    vpn::VpnConnection connection{.id = std::string(connectionId),
                                  .kind = vpn::ConnectionKind::OpenVpn};
    disconnectPrivileged(privilegedRuntime(), connection);
}

void PrivilegedOpenVpnShutdown() {
    const auto runtime = privilegedRuntime();
    std::vector<std::string> ids;
    {
        std::lock_guard lock(runtime->mutex);
        ids.reserve(runtime->sessions.size());
        for (const auto& [id, session] : runtime->sessions) ids.push_back(id);
    }
    for (const auto& id : ids) PrivilegedOpenVpnDisconnect(id);
}

#endif

bool OpenVpnSessionAlive(std::string_view connectionId) {
#if defined(__linux__) && !defined(__ANDROID__)
    return service::openvpnSessionAlive(connectionId);
#elif defined(_WIN32)
    const auto runtime = windowsRuntime();
    std::lock_guard lock(runtime->mutex);
    const auto found = runtime->sessions.find(std::string(connectionId));
    return found != runtime->sessions.end() && found->second.process != nullptr &&
           WaitForSingleObject(found->second.process, 0) == WAIT_TIMEOUT;
#else
    (void)connectionId;
    return false;
#endif
}

bool OpenVpnToolsAvailable() {
#if defined(__linux__) && !defined(__ANDROID__)
    return service::available() && service::openvpnAvailable();
#elif defined(_WIN32)
    return openVpnExecutable().has_value();
#else
    return false;
#endif
}

vpn::EngineAdapter MakeOpenVpnAdapter() {
    vpn::EngineAdapter adapter{
        .descriptor = vpn::EngineDescriptor{
            .kind = vpn::EngineKind::SystemOpenVpn,
            .priority = 78,
            .available = OpenVpnToolsAvailable(),
            .connectionKinds = {vpn::ConnectionKind::OpenVpn},
        },
    };
#if defined(__linux__) && !defined(__ANDROID__)
    const auto runtime = std::make_shared<Runtime>();
    adapter.connect = [runtime](vpn::VpnConnection& connection, std::string& error) {
        return connectUser(runtime, connection, error);
    };
    adapter.applyRoutes = [runtime](vpn::VpnConnection& connection,
                                    std::span<const std::string> routes,
                                    std::string& error) {
        return applyUserRoutes(runtime, connection, routes, error);
    };
    adapter.disconnect = [runtime](vpn::VpnConnection& connection) {
        disconnectUser(runtime, connection);
    };
#elif defined(_WIN32)
    const auto runtime = windowsRuntime();
    adapter.connect = [runtime](vpn::VpnConnection& connection, std::string& error) {
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
        error = "当前平台未实现 OpenVPN CLI 引擎";
        return false;
    };
    adapter.applyRoutes = {};
    adapter.disconnect = {};
#endif
    return adapter;
}

} // namespace openvpn
