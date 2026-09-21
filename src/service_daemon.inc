// service_daemon.inc — root 服务守护循环
// 由 src/service.cppm 在同一模块单元中包含。

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
