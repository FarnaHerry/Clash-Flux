// core_store_helpers.inc — 私有编排辅助函数
// 由 src/store/core_store.cppm 在 CoreStore 类体内包含。

    std::string proxyGroupsFromConfig(nlohmann::json& config) {
        nlohmann::json groups = nlohmann::json::object();
        if (!config.is_object() || !config.contains("outbounds") ||
            !config["outbounds"].is_array()) {
            return nlohmann::json{{"proxies", groups}}.dump();
        }
        for (auto& outbound : config["outbounds"]) {
            const std::string type = outbound.value("type", "");
            if (type != "selector" && type != "urltest") continue;
            const std::string group = outbound.value("tag", "");
            if (group.empty()) continue;
            const auto members =
                outbound.value("outbounds", nlohmann::json::array());
            std::string current = outbound.value("default", "");
            const std::string saved = setting("proxy.selection." + group, "");
            if (type == "selector" && !saved.empty()) {
                for (const auto& member : members) {
                    if (member == saved) {
                        outbound["default"] = saved;
                        current = saved;
                        break;
                    }
                }
            }
            if (current.empty() && !members.empty())
                current = members.front().get<std::string>();
            groups[group] = {{"type", type}, {"now", current},
                             {"all", members},
                             {"selectable", type == "selector"}};
        }
        return nlohmann::json{{"proxies", groups}}.dump();
    }

    inline singbox::CompileOptions routingOptions(
        const std::string& profileYaml, bool tunEnabled,
        bool speedTestOnly = false) {
        singbox::CompileOptions options;
        options.profileYaml = profileYaml;
        options.controller = cfg::controllerAddress();
        options.secret = secret_;
        options.mixedPort = mixedPort();
        options.mode = mode();
        options.allowLan = allowLan();
        options.ipv6 = ipv6Enabled();
        options.logLevel = logLevel();
        options.tunInbound = tunEnabled;
        options.speedTestOnly = speedTestOnly;
        options.ruleSetDir = cfg::coreWorkDir().string();
        routing::PopulateOptions(options, db_->listProfiles(),
            routing::DecodePolicy(setting("vpn.global_policy", "")), nativeSessions_);
        return options;
    }

    // 三种托管形态的存活判定：服务托管问服务、接管的外部实例看 pidfile、
    // 直接 spawn 看进程句柄。接管形态没有 pidfile（对端不是本应用拉的）
    // 时只好信任——WS 断流会在 UI 层表现为无数据。
    bool coreAlive() {
        if (managedByService_) {
            // root 服务同步处理 PPTP 建链时，STATUS 可能
            // 超时。先查内核自己的控制器，避免 UI 存活泵在
            // PPTP 拨号期间每次阻塞 2 秒；仅在控制器不通时才向
            // root 服务确认进程状态。
            if (api_->version().ok) return true;
            return service::coreRunning();
        }
        if (adopted_) {
            const long pid = readPidFile();
            if (pid <= 0) return true;  // 无 pidfile：信任
#if defined(_WIN32)
            // Windows 没有 /proc：旧实现用 /proc/<pid> 判定，接管/驻留形态
            // 恒为 false，正在运行的内核会被误报成「启动后立即退出」。
            return core::pidAlive(pid);
#else
            // POSIX：僵尸进程同样有 /proc 条目（子进程退出码尚未被回收时），
            // 不能算存活——否则 detached 启动的内核已经 FATAL 退出，这里还会
            // 白等 30s 并报「控制器未就绪」。
            std::ifstream stat(std::format("/proc/{}/stat", pid));
            if (!stat) return false;
            const std::string content((std::istreambuf_iterator<char>(stat)),
                                      std::istreambuf_iterator<char>());
            const auto nameEnd = content.rfind(')');
            if (nameEnd == std::string::npos || nameEnd + 2 >= content.size())
                return false;
            return content[nameEnd + 2] != 'Z';
#endif
        }
        return process_.running();
    }

    // detached spawn（CLI/接管形态）没有本进程输出管道：诊断尾部只能读
    // <workDir>/core.log 中本次启动之后追加的内容（旧的启动记录不算）。
    static std::string detachedTail(const std::filesystem::path& workDir,
                                    std::uintmax_t offset) {
        std::ifstream in(workDir / "core.log", std::ios::binary);
        if (!in) return {};
        in.seekg(static_cast<std::streamoff>(offset));
        std::string line;
        std::string last;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) last = line;
        }
        if (last.size() > 300) last.resize(300);
        return core::stripAnsi(std::move(last));
    }

    // 规则集缓存清理：坏文件（0 字节、被写入错误内容、被中间设备截断）会让
    // 内核每次启动都 FATAL（parse rule-set: read rule: unexpected EOF），
    // 而按周刷新的 mtime 判断永远认为它"新鲜"。清掉 .srs 与内核自己的
    // cache.db 后重新预取，是唯一能自愈这种情况的手段。
    void purgeRuleSetCache(const std::filesystem::path& workDir) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(workDir, ec)) {
            if (ec) break;
            std::error_code typeError;
            if (!entry.is_regular_file(typeError) || typeError) continue;
            const std::string name = entry.path().filename().string();
            const bool ruleSetCache = name.ends_with(".srs") ||
                                      name.ends_with(".srs.part");
            const bool engineCache =
                name == "cache.db" || name.starts_with("cache.db.");
            if (!ruleSetCache && !engineCache) continue;
            std::error_code removeError;
            std::filesystem::remove(entry.path(), removeError);
        }
    }

    // 直连预取远程规则集到 <workDir>/<tag>.srs（best-effort：失败即回落
    // remote，由内核启动时经默认出站拉取）。缓存按周刷新，刷新失败沿用
    // 旧文件。Android 的 curl 无 TLS，这里会快速失败并保持 remote 行为。
    void prefetchRuleSets(const std::string& configJson,
                          const std::filesystem::path& workDir) {
        const auto j = nlohmann::json::parse(configJson, nullptr, false);
        if (!j.is_object() || !j.contains("route") || !j["route"].is_object()) {
            return;
        }
        const auto& route = j["route"];
        if (!route.contains("rule_set") || !route["rule_set"].is_array()) {
            return;
        }
        for (const auto& ruleSet : route["rule_set"]) {
            if (!ruleSet.is_object() || ruleSet.value("type", "") != "remote") {
                continue;
            }
            const std::string tag = ruleSet.value("tag", "");
            const std::string url = ruleSet.value("url", "");
            if (tag.empty() || url.empty()) continue;
            const auto dest = workDir / (tag + ".srs");
            std::error_code ec;
            bool stale = true;
            if (std::filesystem::exists(dest, ec) && !ec) {
                if (!singbox::RuleSetCacheValid(dest)) {
                    // 坏缓存必须重下，否则内核会一直读同一个坏文件。
                    std::error_code removeError;
                    std::filesystem::remove(dest, removeError);
                } else {
                    const auto mtime = std::filesystem::last_write_time(dest, ec);
                    if (!ec) {
                        stale = decltype(mtime)::clock::now() - mtime >
                                std::chrono::hours{24 * 7};
                    }
                }
            }
            if (!stale) continue;
            api::ClashApi::DownloadOptions download;
            download.timeoutSecs = 8;  // 直连快速失败，不拖慢启动
            const auto result = api_->downloadToFile(url, dest, download);
            // downloadToFile 只在成功时替换目标：刷新失败沿用上一份缓存，但
            // 缓存本身不可用（半截文件/错误内容）时删掉它，让第二遍编译回落
            // remote，而不是把坏文件写进 local rule_set 让内核 FATAL。
            const bool cacheUsable = singbox::RuleSetCacheValid(dest);
            if (!result.ok || !cacheUsable) {
                if (!cacheUsable) {
                    std::error_code removeError;
                    std::filesystem::remove(dest, removeError);
                }
                // 用拼接而非 std::format：clang-21/libc++ 对含 std::string 实参的
                // 多参数 std::format 会实例化宽字符候选并硬错误（见 src/api.cpp）。
                const std::string reason = result.ok
                                               ? std::string{"文件内容不是规则集"}
                                               : result.error;
                stream::logApplication(
                    "warning",
                    cacheUsable ? "规则集 " + tag + " 刷新失败，沿用本地缓存：" +
                                      reason
                                : "规则集 " + tag + " 预取失败，改由内核在线拉取：" +
                                      reason);
            }
        }
    }

    long readPidFile() {
        std::ifstream in(cfg::coreWorkDir() / "core.pid");
        long pid = 0;
        in >> pid;
        return pid;
    }

    // 系统代理所有权：当前桌面代理是否指向本应用写入的端口（开启时记录
    // proxy.system_port）。只有指向本应用时才允许撤销/重指，避免误关或
    // 抢占其他代理软件（clash-verge/FlClash 等）接管中的设置。
    bool systemProxyOurs() {
#if defined(__ANDROID__)
        return false;
#else
        const std::string current = sysproxy::currentProxy();
        return !current.empty() &&
               current == std::format("127.0.0.1:{}",
                                      setting("proxy.system_port",
                                              std::to_string(mixedPort())));
#endif
    }

    void ensureOpen() {
        // 数据库由启动流程的 Persistence::open 负责（异步、带 0→1 迁移）；
        // 这里只构建同步门面与内核 API 端点。Persistence 未就绪时先建空 secret
        // 的端点，启动任务随后调用 ensureSecret() 刷新。
        std::call_once(initFlag_, [this] {
            db_ = std::make_unique<db::Db>(cfg::databaseFile());
            if (clashflux::persistence::persistence().ready()) {
                ensureSecret();
            } else {
                api_ = std::make_unique<api::ClashApi>(cfg::controllerBaseUrl(),
                                                       secret_);
            }
            stream::logApplication("debug", "持久化门面与内核 API 端点已初始化");
        });
    }

    void fail(std::string error) {
        stream::logApplication("error", "内核操作失败：" + error);
        std::lock_guard lock(mutex_);
        snap_.state = core::CoreState::Failed;
        snap_.lastError = std::move(error);
    }
