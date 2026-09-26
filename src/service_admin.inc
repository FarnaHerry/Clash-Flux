// service_admin.inc — root 服务安装与升级
// 由 src/service.cppm 在同一模块单元中包含。

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
        // The destination may be the executable of this live daemon (or its
        // running sing-box child). Copying over it directly fails with
        // ETXTBSY. Stage in the same directory, sync it, then rename over the
        // old inode; Linux lets existing processes finish on that inode while
        // systemd starts the replacement after this daemon exits.
        const std::string pattern =
            (destination.parent_path() /
             ("." + destination.filename().string() + ".tmp.XXXXXX"))
                .string();
        std::vector<char> temporaryPattern(pattern.begin(), pattern.end());
        temporaryPattern.push_back('\0');
        UniqueFd temporaryFd{::mkstemp(temporaryPattern.data())};
        if (!temporaryFd.valid()) {
            error = std::format("创建 {} 临时文件失败：{}", label,
                                ::strerror(errno));
            return false;
        }
        const std::filesystem::path temporary{temporaryPattern.data()};
        const auto discardTemporary = [&] {
            temporaryFd.reset();
            ::unlink(temporary.c_str());
        };

        std::filesystem::copy_file(
            source, temporary, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            discardTemporary();
            error = std::format("复制 {} 失败：{}", label, ec.message());
            return false;
        }

        constexpr mode_t executableMode = S_IRUSR | S_IWUSR | S_IXUSR |
                                          S_IRGRP | S_IXGRP |
                                          S_IROTH | S_IXOTH;
        if (::fchmod(temporaryFd.get(), executableMode) != 0) {
            const int savedErrno = errno;
            discardTemporary();
            error = std::format("设置 {} 权限失败：{}", label,
                                ::strerror(savedErrno));
            return false;
        }
        if (::fsync(temporaryFd.get()) != 0) {
            const int savedErrno = errno;
            discardTemporary();
            error = std::format("写入 {} 临时文件失败：{}", label,
                                ::strerror(savedErrno));
            return false;
        }
        // release() 交出所有权：close 的返回值在这里有诊断意义，必须自己检查。
        if (::close(temporaryFd.release()) != 0) {
            const int savedErrno = errno;
            ::unlink(temporary.c_str());
            error = std::format("关闭 {} 临时文件失败：{}", label,
                                ::strerror(savedErrno));
            return false;
        }
        if (::rename(temporary.c_str(), destination.c_str()) != 0) {
            const int savedErrno = errno;
            ::unlink(temporary.c_str());
            error = std::format("替换 {} 失败：{}", label,
                                ::strerror(savedErrno));
            return false;
        }
        return true;
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
