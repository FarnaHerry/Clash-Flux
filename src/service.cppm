// service.cppm — clashflux.service：服务模式（接口即实现，单文件模块）。
//
// 对齐 Clash Verge Rev 的 service mode：TUN 需要 root/CAP_NET_ADMIN，一次性
// pkexec 提权执行 `clash-flux service install` 安装一个 systemd 服务；服务以
// root 常驻（`clash-flux service run`），唯一职责是代替用户态 GUI/CLI 拉起
// mihomo 内核进程。之后开关 TUN 不再需要密码。
//
// 进程间通道：unix socket /run/clash-flux/service.sock（0666，用户态客户端
// 可连）。协议为每条连接一行命令（\n 结尾）、一行回复：
//   START <configPath>  → OK / ERR <原因>
//   STOP                → OK / ERR <原因>
//   STATUS              → RUNNING <pid> / STOPPED
//   VERSION             → clash-flux-service 0.1.0
//
// 安全模型：服务是 root 而 socket 人人可连，因此服务只 spawn 固定二进制
// （<服务exe目录>/engines/mihomo → <服务exe目录>/mihomo，安装后形态即
// /usr/local/lib/clash-flux/engines/mihomo），参数固定为
// `-d <parent(config)> -f <config>`，configPath 必须是不含 ".." 的 .yaml
// 绝对路径——客户端无法注入任何其他可执行文件或参数。
module;

// 服务模式仅 Linux（systemd + unix socket）；非 Linux 平台下方导出同签名 stub。
#if defined(__linux__)
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

namespace service {

export constexpr std::string_view kUnitName = "clash-flux.service";
export constexpr std::string_view kSocketPath = "/run/clash-flux/service.sock";
export constexpr std::string_view kInstallDir = "/usr/local/lib/clash-flux";

#if defined(__linux__)

namespace {

constexpr std::string_view kVersionReply = "clash-flux-service 0.1.0";
constexpr std::string_view kSocketDir = "/run/clash-flux";

std::string errnoText(const char* what) {
    return std::format("{}: {}", what, ::strerror(errno));
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

// 连接服务 socket（2s 收发超时），失败返回 -1。
int connectSocket() {
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
    const timeval tv{.tv_sec = 2, .tv_usec = 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
}

// 发一行命令、读一行回复。失败返回 nullopt 并填 err。
std::optional<std::string> request(const std::string& cmd, std::string& err) {
    const int fd = connectSocket();
    if (fd < 0) {
        err = "无法连接服务（服务未运行？sudo systemctl start clash-flux）";
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
    const auto reply = request(std::format("START {}", config.string()), err);
    if (!reply) return false;
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

    const auto unitPath = std::filesystem::path("/etc/systemd/system") / kUnitName;
    std::println("[3/4] 写 systemd 单元 {}", unitPath.string());
    {
        std::ofstream out(unitPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::fprintf(stderr, "写入 %s 失败\n", unitPath.c_str());
            return 1;
        }
        out << "[Unit]\n"
               "Description=Clash-Flux core service (mihomo launcher)\n"
               "After=network.target\n"
               "\n"
               "[Service]\n"
               "Type=simple\n"
               "ExecStart=/usr/local/lib/clash-flux/clash-flux service run\n"
               "Restart=on-failure\n"
               "RestartSec=2\n"
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
    std::println("[3/3] 删除 {}", kInstallDir);
    std::filesystem::remove_all(std::filesystem::path{kInstallDir}, ec);
    std::println("服务已卸载。");
    return 0;
}

// ---- 守护进程（systemd ExecStart 入口，root）----

namespace {

volatile sig_atomic_t g_quit = 0;
volatile sig_atomic_t g_childEvent = 0;

void onQuitSignal(int) { g_quit = 1; }
void onChildSignal(int) { g_childEvent = 1; }

// 非阻塞收割所有已退出子进程；childPid 被收割时清零。
void reapChildren(pid_t& childPid) {
    int status = 0;
    pid_t pid;
    while ((pid = ::waitpid(-1, &status, WNOHANG)) > 0) {
        if (pid == childPid) childPid = 0;
    }
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
    ::chmod(sockPath.c_str(), 0666);  // 用户态客户端要连
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

        // 每条连接一行命令。
        std::string cmd;
        char ch;
        while (cmd.size() < 4096) {
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
            } else if (childPid > 0) {
                replyLine(fd, "ERR 内核已在运行，先 STOP");
            } else {
                const auto binary = serviceMihomo();
                if (binary.empty()) {
                    replyLine(fd, "ERR 服务侧找不到 mihomo 二进制");
                } else {
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
        } else if (!cmd.empty()) {
            replyLine(fd, "ERR 未知命令");
        }
        ::close(fd);
    }

    // 退出：杀掉 mihomo 子进程、删 socket 文件。
    reapChildren(childPid);
    if (childPid > 0) stopMihomo(childPid);
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
export int install() {
    std::println("服务模式仅支持 Linux（systemd）");
    return 1;
}
export int uninstall() {
    std::println("服务模式仅支持 Linux（systemd）");
    return 1;
}
export int run() {
    std::println("服务模式仅支持 Linux（systemd）");
    return 1;
}

#endif

} // namespace service
