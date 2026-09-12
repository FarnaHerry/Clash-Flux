// openvpn.cpp — clashflux.openvpn 的 Linux OpenVPN CLI 实现。
module;

#if defined(__linux__) && !defined(__ANDROID__)
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
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

bool validIpv4Cidr(std::string_view value) {
    const std::size_t slash = value.find('/');
    if (slash == std::string_view::npos || slash == 0 ||
        slash + 1 >= value.size() ||
        value.find('/', slash + 1) != std::string_view::npos) {
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
        if (end == std::string_view::npos) {
            if (part != 3) return false;
            break;
        }
        if (part == 3 || end >= slash) return false;
        begin = end + 1;
    }
    unsigned int prefix = 0;
    const auto [ptr, ec] = std::from_chars(value.data() + slash + 1,
                                           value.data() + value.size(), prefix);
    return ec == std::errc() && ptr == value.data() + value.size() &&
           prefix <= 32;
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

bool spawnAndWait(const std::vector<std::string>& args, std::string& error) {
    if (args.empty()) {
        error = "内部错误：空命令";
        return false;
    }
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    pid_t pid = -1;
    const int spawnError = ::posix_spawnp(&pid, args.front().c_str(), nullptr,
                                          nullptr, argv.data(), environ);
    if (spawnError != 0) {
        error = std::format("启动 ip 失败：{}", std::strerror(spawnError));
        return false;
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
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
    for (const auto& route : session.routes) {
        std::string ignored;
        spawnAndWait({"ip", "route", "del", route, "dev", session.interfaceName},
                     ignored);
    }
    if (session.pid > 0) {
        ::kill(session.pid, SIGTERM);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(3);
        int status = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            if (childExited(session.pid, status)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!childExited(session.pid, status)) {
            ::kill(session.pid, SIGKILL);
            while (::waitpid(session.pid, &status, 0) < 0 && errno == EINTR) {
            }
        }
    }
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
    std::string managedConfig = connection.nativeConfig;
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
    const int spawnError = ::posix_spawnp(&pid, "openvpn", nullptr, nullptr,
                                          argv.data(), environ);
    if (spawnError != 0) {
        error = std::format("启动 openvpn 失败：{}", std::strerror(spawnError));
        removePrivateDirectory(directory);
        return false;
    }

    LinuxSession session{.pid = pid, .directory = directory};
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
    for (const auto& route : routes) {
        if (!route.empty() && !validIpv4Cidr(route)) {
            error = "OpenVPN 路由必须是 IPv4 CIDR";
            return false;
        }
    }
    std::lock_guard lock(runtime->mutex);
    const auto it = runtime->sessions.find(connection.id);
    if (it == runtime->sessions.end()) {
        error = "OpenVPN 会话不存在";
        return false;
    }
    auto& session = it->second;
    for (const auto& route : routes) {
        if (route.empty() || std::ranges::find(session.routes, route) != session.routes.end()) {
            continue;
        }
        if (!spawnAndWait({"ip", "route", "replace", route, "dev",
                           session.interfaceName}, error)) {
            for (const auto& installed : session.routes) {
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
    if (!service::available()) {
        error = "Clash-Flux root 服务未运行；请先在设置中安装/启动服务";
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

#endif

} // namespace

#if defined(__linux__) && !defined(__ANDROID__)

bool PrivilegedOpenVpnAvailable() {
    return ::geteuid() == 0 && linuxToolsAvailable();
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

bool OpenVpnToolsAvailable() {
#if defined(__linux__) && !defined(__ANDROID__)
    return service::available() && service::openvpnAvailable();
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
