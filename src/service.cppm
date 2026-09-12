// service.cppm — clashflux.service：服务模式（接口即实现，单文件模块）。
//
// 对齐 Clash Verge Rev 的 service mode：TUN/PPTP 需要 root/CAP_NET_ADMIN，一次性
// pkexec 提权执行 `clash-flux service install` 安装一个 systemd 服务；服务以
// root 常驻（`clash-flux service run`），统一代替用户态 GUI/CLI 管理 mihomo
// 和系统 PPTP/OpenVPN 连接。之后开关 TUN、拨号和安装原生路由都不再需要用户手动 sudo。
//
// 进程间通道：unix socket /run/clash-flux/service.sock（安装用户 + root 可连）。
// 协议为每条连接一行命令（\n 结尾）、一行回复：
//   START <configPath>  → OK / ERR <原因>
//   STOP                → OK / ERR <原因>
//   STATUS              → RUNNING <pid> / STOPPED
//   VERSION             → clash-flux-service 0.1.0
//   PPTP_AVAILABLE      → YES / NO
//   PPTP_START <hex id> <hex config> [hex route ...] → OK <hex if> <hex gateway>
//   PPTP_ROUTES <hex id> [hex route ...]            → OK / ERR <原因>
//   PPTP_STOP <hex id>                              → OK / ERR <原因>
//   OPENVPN_AVAILABLE                              → YES / NO
//   OPENVPN_START <hex id> <hex config> [hex route ...] → OK <hex if> <hex gateway>
//   OPENVPN_ROUTES <hex id> [hex route ...]            → OK / ERR <原因>
//   OPENVPN_STOP <hex id>                              → OK / ERR <原因>
//
// 安全模型：安装时记录 pkexec/sudo 的原始用户 UID；socket 只允许该 UID 和
// root，daemon 还通过 SO_PEERCRED 二次校验。服务只 spawn 固定 mihomo/OpenVPN 二进制
// （<服务exe目录>/engines/mihomo → <服务exe目录>/mihomo，安装后形态即
// /usr/local/lib/clash-flux/engines/mihomo），参数固定为
// `-d <parent(config)> -f <config>`，configPath 必须是不含 ".." 的 .yaml
// 绝对路径。PPTP/OpenVPN 请求的字段经过十六进制编码，服务端只接受固定协议字段。
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
import clashflux.openvpn;
import clashflux.pptp;

namespace service {

export constexpr std::string_view kUnitName = "clash-flux.service";
export constexpr std::string_view kSocketPath = "/run/clash-flux/service.sock";
export constexpr std::string_view kInstallDir = "/usr/local/lib/clash-flux";

// Returned by native VPN start calls on every platform.  The service backend
// is Linux-only, but the client API and its non-Linux stubs must share the
// same public signature so the Android legacy source can include this module.
export struct PptpSessionInfo {
    std::string interfaceName;
    std::string gateway;
};

export struct OpenVpnSessionInfo {
    std::string interfaceName;
    std::string gateway;
};

#if defined(__linux__) && !defined(__ANDROID__)

namespace {

constexpr std::string_view kVersionReply = "clash-flux-service 0.1.0";
constexpr std::string_view kSocketDir = "/run/clash-flux";
constexpr std::string_view kOwnerUidPath = "/etc/clash-flux/owner.uid";

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

// 服务允许 spawn 的唯一二进制：<exe>/engines/mihomo → <exe>/mihomo。
std::filesystem::path serviceMihomo() {
    const std::filesystem::path dir = cfg::executableDir();
    if (dir.empty()) return {};
    if (const auto p = dir / "engines" / "mihomo"; executableExists(p)) return p;
    if (const auto p = dir / "mihomo"; executableExists(p)) return p;
    return {};
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

// 服务已安装且 socket 可连（= 可托管内核）。
export bool available() {
    std::string err;
    const auto reply = request("VERSION", err);
    return reply && reply->starts_with("clash-flux-service");
}

// systemd 单元文件存在（服务可能未在跑）。
export bool installed() {
    std::error_code ec;
    return std::filesystem::exists(
        std::filesystem::path("/etc/systemd/system") / kUnitName, ec);
}

// ---- 客户端命令（阻塞 IO；UI 必须经 RunOnTaskThread 调用）----

// 让服务以 root 启动 mihomo：-d <config 父目录> -f <config>。
export bool startCore(const std::filesystem::path& config, std::string& err) {
    err.clear();
    auto reply = request(std::format("START {}", config.string()), err);
    if (!reply) return false;
    // 兼容已经安装但尚未重启的旧 root daemon：旧协议在发现 mihomo
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

export bool openvpnAvailable() {
    std::string err;
    const auto reply = request("OPENVPN_AVAILABLE", err);
    return reply && *reply == "YES";
}

export bool startOpenVpn(std::string_view connectionId,
                         std::string_view nativeConfig,
                         std::span<const std::string> routes,
                         OpenVpnSessionInfo& session, std::string& err) {
    err.clear();
    std::string command = std::format("OPENVPN_START {} {}",
                                      hexEncode(connectionId),
                                      hexEncode(nativeConfig));
    for (const std::string& route : routes) {
        command += " ";
        command += hexEncode(route);
    }
    const auto reply = request(command, err, 310);
    if (!reply) return false;
    if (!reply->starts_with("OK")) {
        err = reply->starts_with("ERR ") ? reply->substr(4) : *reply;
        return false;
    }
    const auto fields = splitWords(*reply);
    if (fields.size() < 2) {
        err = "服务返回了无效的 OpenVPN 会话信息";
        return false;
    }
    const auto interfaceName = hexDecode(fields[1]);
    const auto gateway = fields.size() >= 3 && fields[2] != "-"
                             ? hexDecode(fields[2])
                             : std::optional<std::string>{std::string{}};
    if (!interfaceName || !gateway) {
        err = "服务返回了无效的 OpenVPN 会话信息";
        return false;
    }
    session.interfaceName = *interfaceName;
    session.gateway = *gateway;
    return true;
}

export bool applyOpenVpnRoutes(std::string_view connectionId,
                               std::span<const std::string> routes,
                               std::string& err) {
    err.clear();
    std::string command = std::format("OPENVPN_ROUTES {}", hexEncode(connectionId));
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

export bool stopOpenVpn(std::string_view connectionId, std::string& err) {
    err.clear();
    const auto reply = request(std::format("OPENVPN_STOP {}", hexEncode(connectionId)),
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

} // namespace

// ---- 管理命令（由 CLI 子命令直接调用，install/uninstall 要求 euid==0）----

// 复制自身 + engines/mihomo 到 kInstallDir，写 systemd 单元，
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
    const auto mihomo = serviceMihomo();
    if (mihomo.empty()) {
        std::fprintf(stderr,
                     "找不到 mihomo 内核（<exe>/engines/mihomo 或 <exe>/mihomo），无法安装\n");
        return 1;
    }

    const std::filesystem::path installDir{kInstallDir};
    std::error_code ec;
    std::filesystem::create_directories(installDir / "engines", ec);
    if (ec) {
        std::fprintf(stderr, "创建 %s 失败: %s\n", std::string(kInstallDir).c_str(),
                     ec.message().c_str());
        return 1;
    }

    std::println("[1/4] 复制 {} -> {}", exe.string(), (installDir / "clash-flux").string());
    std::filesystem::copy_file(exe, installDir / "clash-flux",
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        std::fprintf(stderr, "复制 clash-flux 失败: %s\n", ec.message().c_str());
        return 1;
    }
    std::filesystem::permissions(installDir / "clash-flux",
                                 std::filesystem::perms::owner_all |
                                     std::filesystem::perms::group_read |
                                     std::filesystem::perms::group_exec |
                                     std::filesystem::perms::others_read |
                                     std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::replace, ec);

    std::println("[2/4] 复制 {} -> {}", mihomo.string(),
                 (installDir / "engines" / "mihomo").string());
    std::filesystem::copy_file(mihomo, installDir / "engines" / "mihomo",
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        std::fprintf(stderr, "复制 mihomo 失败: %s\n", ec.message().c_str());
        return 1;
    }
    std::filesystem::permissions(installDir / "engines" / "mihomo",
                                 std::filesystem::perms::owner_all |
                                     std::filesystem::perms::group_read |
                                     std::filesystem::perms::group_exec |
                                     std::filesystem::perms::others_read |
                                     std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::replace, ec);

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
    {
        std::ofstream out(unitPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::fprintf(stderr, "写入 %s 失败\n", unitPath.c_str());
            return 1;
        }
        out << "[Unit]\n"
               "Description=Clash-Flux privileged network service (mihomo + PPTP + OpenVPN)\n"
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
    }

    std::println("[4/4] systemctl daemon-reload && enable --now {}", kUnitName);
    if (::system("systemctl daemon-reload") != 0) {
        std::fprintf(stderr, "systemctl daemon-reload 失败\n");
        return 1;
    }
    if (::system(std::format("systemctl enable --now {}", kUnitName).c_str()) != 0) {
        std::fprintf(stderr, "systemctl enable --now 失败（journalctl -u %s 查看原因）\n",
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

// 非阻塞收割 mihomo 子进程。PPTP/OpenVPN 也是本服务的直接
// 子进程，必须由各自的会话 runtime 回收；waitpid(-1) 会抢走它们
// 的退出状态，把不同引擎的异常误归到 mihomo 监管链路。
void reapChildren(pid_t& childPid) {
    if (childPid <= 0) return;
    int status = 0;
    const pid_t pid = ::waitpid(childPid, &status, WNOHANG);
    if (pid == childPid || (pid < 0 && errno == ECHILD)) childPid = 0;
}

// 校验 START 的 config 路径：绝对、.yaml 结尾、不含 ".."。
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
    if (!config.ends_with(".yaml")) {
        why = "配置路径必须以 .yaml 结尾";
        return false;
    }
    return true;
}

// fork + execv 拉起 mihomo：setsid 独立会话，stdout/stderr 追加到
// <workdir>/mihomo.service.log。参数固定为 -d <parent> -f <config>。
pid_t spawnMihomo(const std::filesystem::path& binary, const std::string& config,
                  std::string& err) {
    const std::string workDir = std::filesystem::path{config}.parent_path().string();
    const std::string logPath = workDir + "/mihomo.service.log";

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
                              const_cast<char*>("-d"),
                              const_cast<char*>(workDir.c_str()),
                              const_cast<char*>("-f"),
                              const_cast<char*>(config.c_str()),
                              nullptr};
        ::execv(bin.c_str(), argv);
        _exit(127);  // execv 失败
    }
    return pid;
}

// STOP：SIGTERM → 2s 宽限 → SIGKILL。childPid 出参清零。
void stopMihomo(pid_t& childPid) {
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

bool peerAllowed(int fd, const std::optional<uid_t>& ownerUid) {
    ucred credentials{};
    socklen_t length = sizeof(credentials);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0) {
        return false;
    }
    if (credentials.uid == 0) return true;
    return ownerUid && credentials.uid == *ownerUid;
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
    // 任意本地用户接管 mihomo/PPTP。
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
    while (!g_quit) {
        pollfd pfd{.fd = listenFd, .events = POLLIN, .revents = 0};
        const int rc = ::poll(&pfd, 1, 200);
        if (g_childEvent) {
            g_childEvent = 0;
            reapChildren(childPid);
        }
        if (rc <= 0) continue;  // 超时 / EINTR

        const int fd = ::accept(listenFd, nullptr, nullptr);
        if (fd < 0) continue;

        if (!peerAllowed(fd, ownerUid)) {
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
            replyLine(fd, kVersionReply);
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
                stopMihomo(childPid);
                replyLine(fd, "OK");
            }
        } else if (cmd.starts_with("START ")) {
            const std::string config = cmd.substr(6);
            reapChildren(childPid);
            std::string why;
            if (!validConfigPath(config, why)) {
                replyLine(fd, std::format("ERR {}", why));
            } else {
                const auto binary = serviceMihomo();
                if (binary.empty()) {
                    replyLine(fd, "ERR 服务侧找不到 mihomo 二进制");
                } else {
                    // START 是幂等的重启请求。GUI 可能在上一次关闭、崩溃或
                    // 热重载期间没有机会发送 STOP；沿用 Clash Verge 的
                    // 生命周期模型，先回收本服务托管的旧 core，再启动新配置。
                    if (childPid > 0) stopMihomo(childPid);
                    std::string err;
                    const pid_t pid = spawnMihomo(binary, config, err);
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
        } else if (cmd == "OPENVPN_AVAILABLE") {
            replyLine(fd, openvpn::PrivilegedOpenVpnAvailable() ? "YES" : "NO");
        } else if (cmd.starts_with("OPENVPN_START ")) {
            const auto fields = splitWords(cmd);
            const auto id = decodeWord(fields, 1);
            const auto config = decodeWord(fields, 2);
            std::vector<std::string> routes;
            bool valid = id && config && !id->empty() && id->size() <= 128 &&
                         config->size() <= 20000;
            for (std::size_t index = 3; valid && index < fields.size(); ++index) {
                const auto route = decodeWord(fields, index);
                if (!route || route->empty() || route->size() > 64) {
                    valid = false;
                    break;
                }
                routes.push_back(*route);
            }
            if (!valid) {
                replyLine(fd, "ERR 无效的 OpenVPN 请求字段");
            } else {
                std::string interfaceName;
                std::string gateway;
                std::string error;
                if (!openvpn::PrivilegedOpenVpnConnect(*id, *config, routes,
                                                       interfaceName, gateway, error)) {
                    replyLine(fd, std::format("ERR {}", error));
                } else {
                    replyLine(fd, std::format("OK {} {}",
                                              hexEncode(interfaceName),
                                              gateway.empty() ? "-" : hexEncode(gateway)));
                }
            }
        } else if (cmd.starts_with("OPENVPN_ROUTES ")) {
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
                replyLine(fd, "ERR 无效的 OpenVPN 路由请求");
            } else {
                std::string error;
                if (openvpn::PrivilegedOpenVpnApplyRoutes(*id, routes, error)) {
                    replyLine(fd, "OK");
                } else {
                    replyLine(fd, std::format("ERR {}", error));
                }
            }
        } else if (cmd.starts_with("OPENVPN_STOP ")) {
            const auto fields = splitWords(cmd);
            const auto id = decodeWord(fields, 1);
            if (!id || id->empty() || id->size() > 128 || fields.size() != 2) {
                replyLine(fd, "ERR 无效的 OpenVPN 连接标识");
            } else {
                openvpn::PrivilegedOpenVpnDisconnect(*id);
                replyLine(fd, "OK");
            }
        } else if (!cmd.empty()) {
            replyLine(fd, "ERR 未知命令");
        }
        ::close(fd);
    }

    // 退出：杀掉 mihomo 子进程、删 socket 文件。
    reapChildren(childPid);
    if (childPid > 0) stopMihomo(childPid);
    pptp::PrivilegedPptpShutdown();
    openvpn::PrivilegedOpenVpnShutdown();
    ::close(listenFd);
    ::unlink(sockPath.c_str());
    std::println("clash-flux 服务退出。");
    return 0;
}

#else  // !__linux__：服务模式仅 Linux，导出同签名 stub（core_store/cli/UI 无条件 import）。

export bool available() { return false; }
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
export bool openvpnAvailable() { return false; }
export bool startOpenVpn(std::string_view, std::string_view,
                         std::span<const std::string>, OpenVpnSessionInfo&,
                         std::string& err) {
    err = "OpenVPN root 服务仅支持 Linux";
    return false;
}
export bool applyOpenVpnRoutes(std::string_view, std::span<const std::string>,
                               std::string& err) {
    err = "OpenVPN root 服务仅支持 Linux";
    return false;
}
export bool stopOpenVpn(std::string_view, std::string& err) {
    err = "OpenVPN root 服务仅支持 Linux";
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
