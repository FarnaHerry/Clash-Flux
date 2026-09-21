// service_client.inc — root 服务客户端 socket 协议
// 由 src/service.cppm 在同一模块单元中包含。

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
