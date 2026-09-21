// service.cppm — clashflux.service：服务模式（接口即实现，单文件模块）。
//
// 对齐 Clash Verge Rev 的 service mode：TUN/PPTP 需要 root/CAP_NET_ADMIN，一次性
// 首次安装可通过 pkexec 执行 `clash-flux service install` 安装一个 systemd
// 服务；服务以 root 常驻（`clash-flux service run`），统一代替用户态 GUI/CLI
// 管理 sing-box 和系统 PPTP 连接。已安装服务的版本升级由 root 服务
// 自身处理，不再让客户端重复申请 pkexec。
//
// 进程间通道：unix socket /run/clash-flux/service.sock（安装用户 + root 可连）。
// 协议为每条连接一行命令（\n 结尾）、一行回复：
//   START <configPath>  → OK / ERR <原因>
//   STOP                → OK / ERR <原因>
//   STATUS              → RUNNING <pid> / STOPPED
//   VERSION             → clash-flux-service 1 app <Clash-Flux版本>
//   UPGRADE             → OK / ERR <原因>（从当前客户端自身升级 root 服务）
//   PPTP_AVAILABLE      → YES / NO
//   PPTP_STATUS <hex id> → CONNECTED / DISCONNECTED
//   PPTP_START <hex id> <hex config> [hex route ...] → OK <hex if> <hex gateway>
//   PPTP_ROUTES <hex id> [hex route ...]            → OK / ERR <原因>
//   PPTP_STOP <hex id>                              → OK / ERR <原因>
//
// 安全模型：安装时记录 pkexec/sudo 的原始用户 UID；socket 只允许该 UID 和
// root，daemon 还通过 SO_PEERCRED 二次校验。升级时只使用 SO_PEERCRED 得到的
// 客户端进程自身可执行文件和相邻的 sing-box，服务只 spawn 固定 sing-box 二进制；
// （<服务exe目录>/engines/sing-box → <服务exe目录>/sing-box，安装后形态即
// /usr/local/lib/clash-flux/engines/sing-box），参数固定为
// `run -c <config> -D <parent(config)>`，configPath 必须是不含 ".." 的 .json
// 绝对路径。PPTP 请求的字段经过十六进制编码，服务端只接受固定协议字段。
module;

// 服务模式仅 Linux（systemd + unix socket）；非 Linux 平台下方导出同签名 stub。
#if defined(__linux__) && !defined(__ANDROID__)
#include <unistd.h>     // fork, execv, setsid, geteuid, readlink, access, unlink, close
#include <sys/socket.h> // socket, bind, listen, accept, connect
#include <sys/un.h>     // sockaddr_un
#include <sys/stat.h>   // mkdir, chmod
#include <sys/wait.h>   // waitpid
#include <sys/poll.h>   // poll
#include <signal.h>     // sigaction, kill, SIGTERM ...
#include <fcntl.h>      // open, O_APPEND ...
#include <errno.h>
#include <stdio.h>      // fprintf, stderr, snprintf
#include <string.h>     // strerror
#include <stdlib.h>     // system
#include <limits.h>     // PATH_MAX
#endif

export module clashflux.service;

import std;
import clashflux.config;
import clashflux.pptp;

#ifndef CLASHFLUX_VERSION
#define CLASHFLUX_VERSION "unknown"
#endif

namespace service {

export constexpr std::string_view kUnitName = "clash-flux.service";
export constexpr std::string_view kSocketPath = "/run/clash-flux/service.sock";
export constexpr std::string_view kInstallDir = "/usr/local/lib/clash-flux";
export constexpr std::string_view kProtocolVersion = "1";

// VERSION 握手的结果。reachable 表示 socket 上确实有可识别的服务；
// compatible 还要求协议版本和应用版本都与当前客户端一致。
export struct ServiceInfo {
    bool reachable = false;
    std::string protocolVersion;
    std::string applicationVersion;
    std::string error;

    bool compatible() const { return reachable && error.empty(); }
};

// Returned by native VPN start calls on every platform.  The service backend
// is Linux-only, but the client API and its non-Linux stubs must share the
// same public signature so the Android legacy source can include this module.
export struct PptpSessionInfo {
    std::string interfaceName;
    std::string gateway;
};

#if defined(__linux__) && !defined(__ANDROID__)

namespace {

constexpr std::string_view kSocketDir = "/run/clash-flux";
constexpr std::string_view kOwnerUidPath = "/etc/clash-flux/owner.uid";

std::string versionReply() {
    return std::format("clash-flux-service {} app {}", kProtocolVersion,
                       CLASHFLUX_VERSION);
}

std::string errnoText(const char* what) {
    return std::format("{}: {}", what, ::strerror(errno));
}

std::string hexEncode(std::string_view value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(value.size() * 2);
    for (const unsigned char byte : value) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0f]);
    }
    return out;
}

std::optional<std::string> hexDecode(std::string_view value) {
    if (value.size() % 2 != 0) return std::nullopt;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.resize(value.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const int high = nibble(value[i * 2]);
        const int low = nibble(value[i * 2 + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        out[i] = static_cast<char>((high << 4) | low);
    }
    return out;
}

std::vector<std::string> splitWords(std::string_view value) {
    std::vector<std::string> words;
    std::size_t begin = 0;
    while (begin < value.size()) {
        while (begin < value.size() && value[begin] == ' ') ++begin;
        if (begin == value.size()) break;
        const std::size_t end = value.find(' ', begin);
        words.emplace_back(value.substr(
            begin, end == std::string_view::npos ? std::string_view::npos
                                                   : end - begin));
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return words;
}

ServiceInfo parseVersionReply(std::string_view reply) {
    ServiceInfo info;
    const auto fields = splitWords(reply);
    if (fields.size() < 2 || fields[0] != "clash-flux-service") {
        info.error = "root 服务返回了无效的版本信息";
        return info;
    }

    info.reachable = true;
    info.protocolVersion = fields[1];
    if (fields.size() != 4 || fields[2] != "app") {
        info.error = std::format(
            "root 服务过旧，未报告客户端版本（服务协议 {}；旧服务不支持自升级，需先安装一次当前服务）",
            info.protocolVersion);
        return info;
    }
    info.applicationVersion = fields[3];
    if (info.protocolVersion != kProtocolVersion) {
        info.error = std::format(
            "root 服务协议版本 {} 与客户端要求的 {} 不一致（将请求 root 服务自动升级）",
            info.protocolVersion, kProtocolVersion);
        return info;
    }
    if (info.applicationVersion != CLASHFLUX_VERSION) {
        info.error = std::format(
            "root 服务版本 {} 与当前软件 {} 不一致（将请求 root 服务自动升级）",
            info.applicationVersion, CLASHFLUX_VERSION);
    }
    return info;
}

// 自身可执行文件完整路径（/proc/self/exe）。
std::filesystem::path selfExe() {
    char buf[PATH_MAX]{};
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    return std::filesystem::path(std::string(buf, static_cast<size_t>(n)));
}

bool executableExists(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::exists(p, ec) && !ec && ::access(p.c_str(), X_OK) == 0;
}

bool trustedRootPath(const std::filesystem::path& path, std::string_view label,
                     std::string& error) {
    std::filesystem::path current = path;
    while (!current.empty()) {
        struct stat info{};
        if (::stat(current.c_str(), &info) != 0) {
            error = std::format("无法校验 {}：{}", label, ::strerror(errno));
            return false;
        }
        if (info.st_uid != 0 || (info.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
            error = std::format(
                "{} 位于非 root 所有或可被普通用户修改的路径，拒绝自动升级",
                label);
            return false;
        }
        if (current == current.root_path()) break;
        const auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    return true;
}

// 服务允许 spawn 的唯一二进制：<exe>/engines/sing-box → <exe>/sing-box。
std::filesystem::path engineForDirectory(const std::filesystem::path& dir) {
    if (dir.empty()) return {};
    if (const auto p = dir / "engines" / "sing-box"; executableExists(p)) return p;
    if (const auto p = dir / "sing-box"; executableExists(p)) return p;
    return {};
}

std::filesystem::path serviceEngine() {
    return engineForDirectory(cfg::executableDir());
}

std::filesystem::path serviceEngineForExecutable(
    const std::filesystem::path& executable) {
    return engineForDirectory(executable.parent_path());
}

// ---------------- 客户端：unix socket 一问一答 ----------------

// 连接服务 socket；短查询默认 2s，PPTP 拨号由调用方传入更长超时。
int connectSocket(int timeoutSecs = 2) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s",
                  std::string(kSocketPath).c_str());
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    const timeval tv{.tv_sec = timeoutSecs, .tv_usec = 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
}

// 发一行命令、读一行回复。失败返回 nullopt 并填 err。
std::optional<std::string> request(const std::string& cmd, std::string& err,
                                   int timeoutSecs = 2) {
    const int fd = connectSocket(timeoutSecs);
    if (fd < 0) {
        err = "无法连接 Clash-Flux root 服务（请在设置中安装/启动服务）";
        return std::nullopt;
    }
    const std::string line = cmd + "\n";
    size_t sent = 0;
    while (sent < line.size()) {
        const ssize_t n = ::send(fd, line.data() + sent, line.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            err = errnoText("发送命令失败");
            ::close(fd);
            return std::nullopt;
        }
        sent += static_cast<size_t>(n);
    }
    std::string reply;
    char ch;
    while (reply.size() < 4096) {
        const ssize_t n = ::recv(fd, &ch, 1, 0);
        if (n <= 0) break;
        if (ch == '\n') break;
        reply.push_back(ch);
    }
    ::close(fd);
    if (reply.empty()) {
        err = "服务无应答";
        return std::nullopt;
    }
    if (!reply.empty() && reply.back() == '\r') reply.pop_back();
    return reply;
}

} // namespace

// ---- 状态查询（任意进程可调，轻量）----

export ServiceInfo query() {
    ServiceInfo info;
    std::string err;
    const auto reply = request("VERSION", err);
    if (!reply) {
        info.error = err;
        return info;
    }
    return parseVersionReply(*reply);
}

// 版本不匹配时请求 root 服务从当前客户端自身升级，并重新握手确认。
// 没有服务或服务不可达时不主动安装，避免普通启动路径无提示地触发提权。
export bool ensureCompatible(std::string& err) {
    err.clear();
    ServiceInfo info = query();
    if (info.compatible()) return true;
    if (!info.reachable) {
        err = info.error.empty() ? "root 服务不可达" : info.error;
        return false;
    }

    std::string upgradeError;
    const auto reply = request("UPGRADE", upgradeError, 10);
    if (!reply || *reply != "OK") {
        if (reply && !reply->empty()) {
            upgradeError = reply->starts_with("ERR ")
                               ? reply->substr(4)
                               : *reply;
        }
        if (upgradeError == "未知命令") {
            upgradeError =
                "当前 root 服务不支持无授权自升级，旧服务需要先手动安装一次当前服务";
        }
        if (upgradeError.empty()) upgradeError = "root 服务未返回升级结果";
        err = "检测到 root 服务版本不一致，自动升级未完成：" + upgradeError;
        return false;
    }

    // systemd restart 完成后 socket 可能还处于旧进程的收尾窗口，短暂重试
    // 握手，避免把一次成功升级误报为失败。
    for (int attempt = 0; attempt < 50; ++attempt) {
        info = query();
        if (info.compatible()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    err = "root 服务自动升级后版本仍不匹配：" +
          (info.error.empty() ? std::string{"请重新启动应用后重试"} : info.error);
    return false;
}

// 服务 socket 可连且版本与当前客户端兼容。
export bool available() {
    return query().compatible();
}

// 服务 socket 可连，但不要求版本匹配；用于清理旧服务托管的内核。
export bool reachable() {
    return query().reachable;
}

// systemd 单元文件存在（服务可能未在跑）。
export bool installed() {
    std::error_code ec;
    return std::filesystem::exists(
        std::filesystem::path("/etc/systemd/system") / kUnitName, ec);
}

// ---- 客户端命令（阻塞 IO；UI 必须经 RunOnTaskThread 调用）----

// 让服务以 root 启动 sing-box：run -c <config> -D <config 父目录>。
export bool startCore(const std::filesystem::path& config, std::string& err) {
    err.clear();
    if (!ensureCompatible(err)) return false;
    auto reply = request(std::format("START {}", config.string()), err);
    if (!reply) return false;
    // 兼容已经安装但尚未重启的旧 root daemon：旧协议在发现内核
    // 已运行时要求客户端先 STOP。新协议本身是幂等 START；这里只对旧
    // 错误做一次 stop + retry，升级应用后无需用户手动 sudo stop。
    if (reply->starts_with("ERR ") && reply->find("先 STOP") != std::string::npos) {
        std::string stopError;
        const auto stopped = request("STOP", stopError);
        if (!stopped || *stopped != "OK") {
            if (stopped && !stopped->empty()) stopError = *stopped;
            err = "旧 root 服务要求先停止内核，但 STOP 失败：" + stopError;
            return false;
        }
        reply = request(std::format("START {}", config.string()), err);
        if (!reply) return false;
    }
    if (*reply == "OK") return true;
    err = reply->starts_with("ERR ") ? reply->substr(4) : *reply;
    // 旧版 root daemon 只认 .yaml 配置：升级内核格式后服务本体还是旧二进制。
    // 给出明确的重装指引，而不是让用户对着「以 .yaml 结尾」猜原因。
    if (err.find(".yaml") != std::string::npos) {
        err += "（检测到旧版服务：请重新执行 sudo clash-flux service install 升级服务）";
    }
    return false;
}

export bool stopCore(std::string& err) {
    err.clear();
    const auto reply = request("STOP", err);
    if (!reply) return false;
    if (*reply == "OK") return true;
    err = reply->starts_with("ERR ") ? reply->substr(4) : *reply;
    return false;
}

// 服务视角的内核存活（连不上服务 = false）。
export bool coreRunning() {
    std::string err;
    const auto reply = request("STATUS", err);
    return reply && reply->starts_with("RUNNING ");
}

export bool pptpAvailable() {
    std::string err;
    const auto reply = request("PPTP_AVAILABLE", err);
    return reply && *reply == "YES";
}

// Status never releases a session: the caller must first remove the core's
// interface binding before disconnecting and releasing compensation routes.
// An unavailable or older daemon cannot prove ownership of a live interface.
export bool pptpSessionAlive(std::string_view connectionId) {
    if (connectionId.empty() || connectionId.size() > 128) return false;
    std::string err;
    const auto reply = request(std::format("PPTP_STATUS {}", hexEncode(connectionId)), err);
    return reply && *reply == "CONNECTED";
}

export bool startPptp(std::string_view connectionId, std::string_view nativeConfig,
                      std::span<const std::string> routes,
                      PptpSessionInfo& session, std::string& err) {
    err.clear();
    std::string command = std::format("PPTP_START {} {}", hexEncode(connectionId),
                                      hexEncode(nativeConfig));
    for (const std::string& route : routes) {
        command += " ";
        command += hexEncode(route);
    }
    // connectTimeoutSecs 最大 300 秒；给 root daemon 留出清理和 IPC 余量。
    const auto reply = request(command, err, 310);
    if (!reply) return false;
    if (!reply->starts_with("OK")) {
        err = reply->starts_with("ERR ") ? reply->substr(4) : *reply;
        return false;
    }
    const auto fields = splitWords(*reply);
    if (fields.size() < 2) {
        err = "服务返回了无效的 PPTP 会话信息";
        return false;
    }
    const auto interfaceName = hexDecode(fields[1]);
    const auto gateway = fields.size() >= 3 && fields[2] != "-"
                             ? hexDecode(fields[2])
                             : std::optional<std::string>{std::string{}};
    if (!interfaceName || !gateway) {
        err = "服务返回了无效的 PPTP 会话信息";
        return false;
    }
    session.interfaceName = *interfaceName;
    session.gateway = *gateway;
    return true;
}

export bool applyPptpRoutes(std::string_view connectionId,
                            std::span<const std::string> routes,
                            std::string& err) {
    err.clear();
    std::string command = std::format("PPTP_ROUTES {}", hexEncode(connectionId));
    for (const std::string& route : routes) {
        command += " ";
        command += hexEncode(route);
    }
    const auto reply = request(command, err, 10);
    if (!reply) return false;
    if (*reply == "OK") return true;
    err = reply->starts_with("ERR ") ? reply->substr(4) : *reply;
    return false;
}

export bool stopPptp(std::string_view connectionId, std::string& err) {
    err.clear();
    const auto reply = request(std::format("PPTP_STOP {}", hexEncode(connectionId)),
                               err, 10);
    if (!reply) return false;
    if (*reply == "OK") return true;
    err = reply->starts_with("ERR ") ? reply->substr(4) : *reply;
    return false;
}

namespace {

std::optional<uid_t> parseUid(std::string_view value) {
    unsigned long parsed = 0;
    const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(),
                                           parsed);
    if (ec != std::errc() || ptr != value.data() + value.size() ||
        parsed > std::numeric_limits<uid_t>::max()) {
        return std::nullopt;
    }
    return static_cast<uid_t>(parsed);
}

// pkexec/sudo 会保留调用提权前的 UID；直接 root 安装时只能退回当前 UID。
std::optional<uid_t> installingUserUid() {
    for (const char* name : {"PKEXEC_UID", "SUDO_UID"}) {
        if (const char* value = std::getenv(name); value != nullptr) {
            if (const auto uid = parseUid(value)) return uid;
        }
    }
    const uid_t current = ::getuid();
    return current == 0 ? std::nullopt : std::optional<uid_t>{current};
}

std::optional<uid_t> serviceOwnerUid() {
    std::ifstream input{std::string(kOwnerUidPath)};
    std::string value;
    if (!input || !std::getline(input, value)) return std::nullopt;
    return parseUid(value);
}

bool copyServiceFile(const std::filesystem::path& source,
                     const std::filesystem::path& destination,
                     std::string_view label, std::string& error) {
    std::error_code ec;
    const bool sameFile = std::filesystem::equivalent(source, destination, ec);
    ec.clear();
    if (!sameFile) {
        std::filesystem::copy_file(
            source, destination, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            error = std::format("复制 {} 失败：{}", label, ec.message());
            return false;
        }
    }

    ec.clear();
    std::filesystem::permissions(
        destination,
        std::filesystem::perms::owner_all |
            std::filesystem::perms::group_read |
            std::filesystem::perms::group_exec |
            std::filesystem::perms::others_read |
            std::filesystem::perms::others_exec,
        std::filesystem::perm_options::replace, ec);
    if (ec) {
        error = std::format("设置 {} 权限失败：{}", label, ec.message());
        return false;
    }
    return true;
}

// root 服务收到 UPGRADE 后，从 SO_PEERCRED 对应的客户端进程取出当前版本
// 的可执行文件，并把它及相邻 sing-box 安装到固定目录。整个过程不经过
// shell，也不接受客户端自行伪造的源路径。
bool copyServicePayload(const std::filesystem::path& sourceExe,
                        std::string& error, bool printProgress = false) {
    if (sourceExe.empty() || !sourceExe.is_absolute() ||
        !executableExists(sourceExe)) {
        error = "当前客户端可执行文件不可用，无法升级 root 服务";
        return false;
    }
    if (!trustedRootPath(sourceExe, "当前客户端", error)) return false;
    const auto engine = serviceEngineForExecutable(sourceExe);
    if (engine.empty()) {
        error = "当前客户端目录中找不到 sing-box，无法升级 root 服务";
        return false;
    }
    if (!trustedRootPath(engine, "当前客户端的 sing-box", error)) return false;

    const std::filesystem::path installDir{kInstallDir};
    std::error_code ec;
    std::filesystem::create_directories(installDir / "engines", ec);
    if (ec) {
        error = std::format("创建 {} 失败：{}", kInstallDir, ec.message());
        return false;
    }

    const auto installedExe = installDir / "clash-flux";
    const auto installedEngine = installDir / "engines" / "sing-box";
    if (printProgress) {
        std::println("[1/4] 复制 {} -> {}", sourceExe.string(), installedExe.string());
    }
    if (!copyServiceFile(sourceExe, installedExe, "clash-flux", error)) return false;
    if (printProgress) {
        std::println("[2/4] 复制 {} -> {}", engine.string(), installedEngine.string());
    }
    return copyServiceFile(engine, installedEngine, "sing-box", error);
}

bool writeServiceUnit(std::string& error) {
    const std::filesystem::path unitPath =
        std::filesystem::path("/etc/systemd/system") / kUnitName;
    std::ofstream out(unitPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = std::format("写入 {} 失败", unitPath.string());
        return false;
    }
    out << "[Unit]\n"
           "Description=Clash-Flux privileged network service (sing-box + PPTP)\n"
           "After=network.target\n"
           "\n"
           "[Service]\n"
           "Type=simple\n"
           "ExecStart=/usr/local/lib/clash-flux/clash-flux service run\n"
           "Restart=on-failure\n"
           "RestartSec=2\n"
           "UMask=0077\n"
           "\n"
           "[Install]\n"
           "WantedBy=multi-user.target\n";
    if (!out) {
        error = std::format("写入 {} 失败", unitPath.string());
        return false;
    }
    return true;
}

bool reloadSystemd(std::string& error) {
    if (::system("systemctl daemon-reload") != 0) {
        error = "systemctl daemon-reload 失败";
        return false;
    }
    return true;
}

bool upgradeFromClient(const std::filesystem::path& clientExe,
                       std::string& error) {
    if (!copyServicePayload(clientExe, error)) return false;
    if (!writeServiceUnit(error)) return false;
    return reloadSystemd(error);
}

} // namespace

// ---- 管理命令（由 CLI 子命令直接调用，install/uninstall 要求 euid==0）----

// 复制自身 + engines/sing-box 到 kInstallDir，写 systemd 单元，
// daemon-reload + enable --now。非 root 返回非零并提示用 pkexec。
export int install() {
    if (::geteuid() != 0) {
        const auto exe = selfExe();
        std::fprintf(stderr,
                     "安装服务需要 root。请经 pkexec 调用：pkexec %s service install\n",
                     exe.empty() ? "clash-flux" : exe.c_str());
        return 1;
    }

    const auto exe = selfExe();
    if (exe.empty()) {
        std::fprintf(stderr, "无法解析自身可执行文件路径（/proc/self/exe）\n");
        return 1;
    }
    std::string error;
    if (!copyServicePayload(exe, error, true)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }

    std::error_code ec;

    const std::filesystem::path ownerDir{"/etc/clash-flux"};
    std::filesystem::create_directories(ownerDir, ec);
    if (ec) {
        std::fprintf(stderr, "创建 %s 失败: %s\n", ownerDir.c_str(),
                     ec.message().c_str());
        return 1;
    }
    const auto ownerUid = installingUserUid();
    {
        std::ofstream owner(std::string(kOwnerUidPath),
                            std::ios::binary | std::ios::trunc);
        if (!owner) {
            std::fprintf(stderr, "写入 %s 失败\n", std::string(kOwnerUidPath).c_str());
            return 1;
        }
        if (ownerUid) owner << *ownerUid << '\n';
    }
    ::chmod(std::string(kOwnerUidPath).c_str(), 0644);
    if (ownerUid) {
        std::println("服务客户端用户 UID：{}", *ownerUid);
    } else {
        std::println("未能识别提权前用户 UID；服务 socket 将只允许 root");
    }

    const auto unitPath = std::filesystem::path("/etc/systemd/system") / kUnitName;
    std::println("[3/4] 写 systemd 单元 {}", unitPath.string());
    if (!writeServiceUnit(error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }

    std::println("[4/4] systemctl daemon-reload && enable && restart {}", kUnitName);
    if (!reloadSystemd(error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    if (::system(std::format("systemctl enable {}", kUnitName).c_str()) != 0) {
        std::fprintf(stderr, "systemctl enable 失败（journalctl -u %s 查看原因）\n",
                     std::string(kUnitName).c_str());
        return 1;
    }
    // restart 而非 start：单元已在运行时 enable --now 是空操作，磁盘上
    // 换了新二进制也不会生效——升级后必须重启守护进程（restart 对未启动
    // 的单元等价于 start）。
    if (::system(std::format("systemctl restart {}", kUnitName).c_str()) != 0) {
        std::fprintf(stderr, "systemctl restart 失败（journalctl -u %s 查看原因）\n",
                     std::string(kUnitName).c_str());
        return 1;
    }
    std::println("服务已安装并启动。验证：systemctl status {}", kUnitName);
    return 0;
}

// disable --now + 删单元 + 删 kInstallDir。
export int uninstall() {
    if (::geteuid() != 0) {
        const auto exe = selfExe();
        std::fprintf(stderr,
                     "卸载服务需要 root。请经 pkexec 调用：pkexec %s service uninstall\n",
                     exe.empty() ? "clash-flux" : exe.c_str());
        return 1;
    }
    if (!installed()) {
        std::println("服务未安装，无需卸载。");
        return 0;
    }
    std::println("[1/3] systemctl disable --now {}", kUnitName);
    // 服务可能已损坏/未在跑：disable 失败不阻断后续清理。
    if (::system(std::format("systemctl disable --now {}", kUnitName).c_str()) != 0) {
        std::println(stderr, "警告：systemctl disable --now 返回非零，继续清理文件");
    }
    const auto unitPath = std::filesystem::path("/etc/systemd/system") / kUnitName;
    std::println("[2/3] 删除 {}", unitPath.string());
    std::error_code ec;
    std::filesystem::remove(unitPath, ec);
    ::system("systemctl daemon-reload");
    std::println("[3/3] 删除 {} 和服务用户信息", kInstallDir);
    std::filesystem::remove_all(std::filesystem::path{kInstallDir}, ec);
    std::filesystem::remove(std::filesystem::path{kOwnerUidPath}, ec);
    std::filesystem::remove(std::filesystem::path{"/etc/clash-flux"}, ec);
    std::println("服务已卸载。");
    return 0;
}

// ---- 守护进程（systemd ExecStart 入口，root）----

namespace {

volatile sig_atomic_t g_quit = 0;
volatile sig_atomic_t g_childEvent = 0;

void onQuitSignal(int) { g_quit = 1; }
void onChildSignal(int) { g_childEvent = 1; }

// 非阻塞收割 sing-box 子进程。PPTP 会话由各自的会话 runtime 回收；
// waitpid(-1) 会抢走它们的退出状态，把不同引擎的异常误归到
// sing-box 监管链路。
void reapChildren(pid_t& childPid) {
    if (childPid <= 0) return;
    int status = 0;
    const pid_t pid = ::waitpid(childPid, &status, WNOHANG);
    if (pid == childPid || (pid < 0 && errno == ECHILD)) childPid = 0;
}

// 校验 START 的 config 路径：绝对、.json/.yaml 结尾、不含 ".."。
// .yaml 一并接受：sing-box 按 content 而非扩展名读配置，且升级窗口里
// 新客户端（.json）与旧版 daemon（只认 .yaml）可能并存，扩展名不是安全
// 边界——路径合法性（绝对 + 无 ".."）才是。
bool validConfigPath(const std::string& config, std::string& why) {
    const std::filesystem::path p{config};
    if (!p.is_absolute()) {
        why = "配置路径必须是绝对路径";
        return false;
    }
    if (config.find("..") != std::string::npos) {
        why = "配置路径不允许包含 \"..\"";
        return false;
    }
    if (!config.ends_with(".json") && !config.ends_with(".yaml")) {
        why = "配置路径必须以 .json 结尾";
        return false;
    }
    return true;
}

// fork + execv 拉起 sing-box：setsid 独立会话，stdout/stderr 追加到
// <workdir>/sing-box.service.log。参数固定为 run -c <config> -D <parent>。
pid_t spawnEngine(const std::filesystem::path& binary, const std::string& config,
                  std::string& err) {
    const std::string workDir = std::filesystem::path{config}.parent_path().string();
    const std::string logPath = workDir + "/sing-box.service.log";

    const pid_t pid = ::fork();
    if (pid < 0) {
        err = errnoText("fork 失败");
        return -1;
    }
    if (pid == 0) {
        // 子进程
        ::setsid();
        const int log = ::open(logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (log >= 0) {
            ::dup2(log, STDOUT_FILENO);
            ::dup2(log, STDERR_FILENO);
            if (log > STDERR_FILENO) ::close(log);
        }
        const std::string bin = binary.string();
        char* const argv[] = {const_cast<char*>(bin.c_str()),
                              const_cast<char*>("run"),
                              const_cast<char*>("-c"),
                              const_cast<char*>(config.c_str()),
                              const_cast<char*>("-D"),
                              const_cast<char*>(workDir.c_str()),
                              nullptr};
        ::execv(bin.c_str(), argv);
        _exit(127);  // execv 失败
    }
    return pid;
}

// STOP：SIGTERM → 2s 宽限 → SIGKILL。childPid 出参清零。
void stopEngine(pid_t& childPid) {
    if (childPid <= 0) return;
    ::kill(childPid, SIGTERM);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        if (::waitpid(childPid, &status, WNOHANG) == childPid) {
            childPid = 0;
            return;
        }
        ::usleep(10 * 1000);
    }
    ::kill(childPid, SIGKILL);
    ::waitpid(childPid, &status, 0);
    childPid = 0;
}

void replyLine(int fd, std::string_view line) {
    std::string out{line};
    out.push_back('\n');
    size_t sent = 0;
    while (sent < out.size()) {
        const ssize_t n = ::send(fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return;
        sent += static_cast<size_t>(n);
    }
}

bool peerCredentials(int fd, ucred& credentials) {
    socklen_t length = sizeof(credentials);
    return ::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) == 0;
}

bool peerAllowed(const ucred& credentials, const std::optional<uid_t>& ownerUid) {
    if (credentials.uid == 0) return true;
    return ownerUid && credentials.uid == *ownerUid;
}

std::filesystem::path peerExecutable(pid_t pid) {
    if (pid <= 0) return {};
    char buf[PATH_MAX]{};
    const std::string procPath = std::format("/proc/{}/exe", pid);
    const ssize_t n = ::readlink(procPath.c_str(), buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    return std::filesystem::path(std::string(buf, static_cast<size_t>(n)));
}

std::optional<std::string> decodeWord(const std::vector<std::string>& words,
                                      std::size_t index) {
    if (index >= words.size()) return std::nullopt;
    return hexDecode(words[index]);
}

} // namespace

// root 守护循环（unix socket 服务器），正常不返回；出错返回非零。
export int run() {
    if (::geteuid() != 0) {
        std::fprintf(stderr, "service run 必须以 root 运行（由 systemd 拉起）\n");
        return 1;
    }

    if (::mkdir(std::string(kSocketDir).c_str(), 0755) != 0 && errno != EEXIST) {
        std::fprintf(stderr, "%s\n", errnoText(std::format("创建 {}", kSocketDir).c_str()).c_str());
        return 1;
    }
    // systemd 单元的 UMask=0077 会把 mkdir 的 0755 剥成 0700：非 root 的
    // GUI/CLI 连目录都无法穿越，socket 永远不可达（TUN 门禁因此误报
    // "未安装服务"）。目录权限显式校正；访问控制由 socket 本身的
    // 0600 + chown(安装用户) 承担。
    if (::chmod(std::string(kSocketDir).c_str(), 0755) != 0) {
        std::fprintf(stderr, "%s\n", errnoText(std::format("chmod {}", kSocketDir).c_str()).c_str());
        return 1;
    }

    const std::string sockPath{kSocketPath};

    // 单实例：socket 文件已存在时先 connect 探测——能连说明已有服务在跑。
    {
        std::error_code ec;
        if (std::filesystem::exists(sockPath, ec)) {
            if (const int probe = connectSocket(); probe >= 0) {
                ::close(probe);
                std::fprintf(stderr, "已有 clash-flux 服务实例在运行（%s 可连接）\n",
                             sockPath.c_str());
                return 1;
            }
            ::unlink(sockPath.c_str());  // 残留文件
        }
    }

    const int listenFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenFd < 0) {
        std::fprintf(stderr, "%s\n", errnoText("创建 socket 失败").c_str());
        return 1;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sockPath.c_str());
    if (::bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::fprintf(stderr, "%s\n", errnoText("bind 失败").c_str());
        ::close(listenFd);
        return 1;
    }
    const auto ownerUid = serviceOwnerUid();
    if (ownerUid) {
        if (::chown(sockPath.c_str(), *ownerUid, static_cast<gid_t>(-1)) != 0) {
            std::fprintf(stderr, "%s\n", errnoText("设置服务 socket 所有者失败").c_str());
            ::close(listenFd);
            ::unlink(sockPath.c_str());
            return 1;
        }
    }
    // 安装用户 + root；若安装时未识别原始 UID，则退回 root-only，避免
    // 任意本地用户接管 sing-box/PPTP。
    ::chmod(sockPath.c_str(), 0600);
    if (::listen(listenFd, 8) != 0) {
        std::fprintf(stderr, "%s\n", errnoText("listen 失败").c_str());
        ::close(listenFd);
        ::unlink(sockPath.c_str());
        return 1;
    }

    struct sigaction sa{};
    sa.sa_handler = onQuitSignal;
    ::sigemptyset(&sa.sa_mask);
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGINT, &sa, nullptr);
    struct sigaction sc{};
    sc.sa_handler = onChildSignal;
    ::sigemptyset(&sc.sa_mask);
    ::sigaction(SIGCHLD, &sc, nullptr);
    ::signal(SIGPIPE, SIG_IGN);

    std::println("clash-flux 服务就绪，监听 {}", sockPath);

    pid_t childPid = 0;
    bool restartRequested = false;
    while (!g_quit && !restartRequested) {
        pollfd pfd{.fd = listenFd, .events = POLLIN, .revents = 0};
        const int rc = ::poll(&pfd, 1, 200);
        if (g_childEvent) {
            g_childEvent = 0;
            reapChildren(childPid);
        }
        if (rc <= 0) continue;  // 超时 / EINTR

        const int fd = ::accept(listenFd, nullptr, nullptr);
        if (fd < 0) continue;

        ucred peer{};
        if (!peerCredentials(fd, peer) || !peerAllowed(peer, ownerUid)) {
            replyLine(fd, "ERR 未授权的服务客户端");
            ::close(fd);
            continue;
        }

        // 每条连接一行命令。
        std::string cmd;
        char ch;
        while (cmd.size() < 65536) {
            const ssize_t n = ::recv(fd, &ch, 1, 0);
            if (n <= 0) break;
            if (ch == '\n') break;
            cmd.push_back(ch);
        }
        if (!cmd.empty() && cmd.back() == '\r') cmd.pop_back();

        if (cmd == "VERSION") {
            replyLine(fd, versionReply());
        } else if (cmd == "UPGRADE") {
            const auto clientExe = peerExecutable(peer.pid);
            std::string error;
            if (clientExe.empty()) {
                replyLine(fd, "ERR 无法识别请求升级的客户端");
            } else if (!upgradeFromClient(clientExe, error)) {
                replyLine(fd, std::format("ERR {}", error));
            } else {
                // 先给客户端确认，再退出当前旧进程。systemd 的
                // Restart=on-failure 会用刚复制的二进制重新拉起服务。
                replyLine(fd, "OK");
                restartRequested = true;
            }
        } else if (cmd == "STATUS") {
            reapChildren(childPid);
            if (childPid > 0) {
                replyLine(fd, std::format("RUNNING {}", childPid));
            } else {
                replyLine(fd, "STOPPED");
            }
        } else if (cmd == "STOP") {
            reapChildren(childPid);
            if (childPid <= 0) {
                replyLine(fd, "OK");  // 本就没在跑：幂等成功
            } else {
                stopEngine(childPid);
                replyLine(fd, "OK");
            }
        } else if (cmd.starts_with("START ")) {
            const std::string config = cmd.substr(6);
            reapChildren(childPid);
            std::string why;
            if (!validConfigPath(config, why)) {
                replyLine(fd, std::format("ERR {}", why));
            } else {
                const auto binary = serviceEngine();
                if (binary.empty()) {
                    replyLine(fd, "ERR 服务侧找不到 sing-box 二进制");
                } else {
                    // START 是幂等的重启请求。GUI 可能在上一次关闭、崩溃或
                    // 热重载期间没有机会发送 STOP；沿用 Clash Verge 的
                    // 生命周期模型，先回收本服务托管的旧 core，再启动新配置。
                    if (childPid > 0) stopEngine(childPid);
                    std::string err;
                    const pid_t pid = spawnEngine(binary, config, err);
                    if (pid < 0) {
                        replyLine(fd, std::format("ERR {}", err));
                    } else {
                        childPid = pid;
                        replyLine(fd, "OK");
                    }
                }
            }
        } else if (cmd == "PPTP_AVAILABLE") {
            replyLine(fd, pptp::PrivilegedPptpAvailable() ? "YES" : "NO");
        } else if (cmd.starts_with("PPTP_STATUS ")) {
            const auto fields = splitWords(cmd);
            const auto id = decodeWord(fields, 1);
            if (!id || id->empty() || id->size() > 128 || fields.size() != 2) {
                replyLine(fd, "ERR 无效的 PPTP 连接标识");
            } else {
                replyLine(fd, pptp::PrivilegedPptpSessionAlive(*id)
                                  ? "CONNECTED" : "DISCONNECTED");
            }
        } else if (cmd.starts_with("PPTP_START ")) {
            const auto fields = splitWords(cmd);
            const auto id = decodeWord(fields, 1);
            const auto config = decodeWord(fields, 2);
            std::vector<std::string> routes;
            bool valid = id && config && !id->empty() && id->size() <= 128;
            for (std::size_t index = 3; valid && index < fields.size(); ++index) {
                const auto route = decodeWord(fields, index);
                if (!route || route->empty() || route->size() > 64) {
                    valid = false;
                    break;
                }
                routes.push_back(*route);
            }
            if (!valid) {
                replyLine(fd, "ERR 无效的 PPTP 请求字段");
            } else {
                std::string interfaceName;
                std::string gateway;
                std::string error;
                if (!pptp::PrivilegedPptpConnect(*id, *config, routes,
                                                 interfaceName, gateway, error)) {
                    replyLine(fd, std::format("ERR {}", error));
                } else {
                    replyLine(fd, std::format("OK {} {}",
                                              hexEncode(interfaceName),
                                              gateway.empty() ? "-" : hexEncode(gateway)));
                }
            }
        } else if (cmd.starts_with("PPTP_ROUTES ")) {
            const auto fields = splitWords(cmd);
            const auto id = decodeWord(fields, 1);
            std::vector<std::string> routes;
            bool valid = id && !id->empty() && id->size() <= 128;
            for (std::size_t index = 2; valid && index < fields.size(); ++index) {
                const auto route = decodeWord(fields, index);
                if (!route || route->empty() || route->size() > 64) {
                    valid = false;
                    break;
                }
                routes.push_back(*route);
            }
            if (!valid) {
                replyLine(fd, "ERR 无效的 PPTP 路由请求");
            } else {
                std::string error;
                if (pptp::PrivilegedPptpApplyRoutes(*id, routes, error)) {
                    replyLine(fd, "OK");
                } else {
                    replyLine(fd, std::format("ERR {}", error));
                }
            }
        } else if (cmd.starts_with("PPTP_STOP ")) {
            const auto fields = splitWords(cmd);
            const auto id = decodeWord(fields, 1);
            if (!id || id->empty() || id->size() > 128 || fields.size() != 2) {
                replyLine(fd, "ERR 无效的 PPTP 连接标识");
            } else {
                pptp::PrivilegedPptpDisconnect(*id);
                replyLine(fd, "OK");
            }
        } else if (!cmd.empty()) {
            replyLine(fd, "ERR 未知命令");
        }
        ::close(fd);
    }

    // 退出：杀掉 sing-box 子进程、删 socket 文件。
    reapChildren(childPid);
    if (childPid > 0) stopEngine(childPid);
    pptp::PrivilegedPptpShutdown();
    ::close(listenFd);
    ::unlink(sockPath.c_str());
    std::println("clash-flux 服务退出。");
    // UPGRADE 需要让 systemd 重新执行 /usr/local/lib 中刚替换的二进制。
    // 返回非零配合 Restart=on-failure，避免在当前进程里同步 restart 自己造成死锁。
    return restartRequested ? 75 : 0;
}

#else  // !__linux__：服务模式仅 Linux，导出同签名 stub（core_store/cli/UI 无条件 import）。

export bool available() { return false; }
export bool reachable() { return false; }
export ServiceInfo query() {
    ServiceInfo info;
    info.error = "服务模式仅支持 Linux";
    return info;
}
export bool ensureCompatible(std::string& err) {
    err = "服务模式仅支持 Linux";
    return false;
}
export bool installed() { return false; }
export bool startCore(const std::filesystem::path&, std::string& err) {
    err = "服务模式仅支持 Linux";
    return false;
}
export bool stopCore(std::string& err) {
    err = "服务模式仅支持 Linux";
    return false;
}
export bool coreRunning() { return false; }
export bool pptpAvailable() { return false; }
export bool pptpSessionAlive(std::string_view) { return false; }
export bool startPptp(std::string_view, std::string_view,
                      std::span<const std::string>, PptpSessionInfo&, std::string& err) {
    err = "PPTP root 服务仅支持 Linux";
    return false;
}
export bool applyPptpRoutes(std::string_view, std::span<const std::string>,
                            std::string& err) {
    err = "PPTP root 服务仅支持 Linux";
    return false;
}
export bool stopPptp(std::string_view, std::string& err) {
    err = "PPTP root 服务仅支持 Linux";
    return false;
}
export int install() {
#if defined(__ANDROID__)
    std::fputs("服务模式仅支持 Linux（systemd）\n", stdout);
#else
    std::println("服务模式仅支持 Linux（systemd）");
#endif
    return 1;
}
export int uninstall() {
#if defined(__ANDROID__)
    std::fputs("服务模式仅支持 Linux（systemd）\n", stdout);
#else
    std::println("服务模式仅支持 Linux（systemd）");
#endif
    return 1;
}
export int run() {
#if defined(__ANDROID__)
    std::fputs("服务模式仅支持 Linux（systemd）\n", stdout);
#else
    std::println("服务模式仅支持 Linux（systemd）");
#endif
    return 1;
}

#endif

} // namespace service
