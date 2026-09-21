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
            return std::filesystem::exists(
                std::format("/proc/{}", pid));  // POSIX /proc 判定
        }
        return process_.running();
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
                const auto mtime = std::filesystem::last_write_time(dest, ec);
                if (!ec) {
                    stale = decltype(mtime)::clock::now() - mtime >
                            std::chrono::hours{24 * 7};
                }
            }
            if (!stale) continue;
            api::ClashApi::DownloadOptions download;
            download.timeoutSecs = 8;  // 直连快速失败，不拖慢启动
            api_->downloadToFile(url, dest, download);
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
        // Android starts the bundled engine from the Java/native shell before
        // HuxerUI necessarily builds its first frame. AppRoot may therefore
        // initialize the same store concurrently; call_once makes lazy DB/API
        // construction safe for both paths while preserving the existing
        // single-process singleton contract on desktop.
        std::call_once(initFlag_, [this] {
            auto database = std::make_unique<db::Db>(cfg::databaseFile());
            std::string secret = database->getSetting("core.secret", "");
            if (secret.empty()) {
                secret = cfg::randomSecret();
                database->setSetting("core.secret", secret);
            }
            db_ = std::move(database);
            secret_ = std::move(secret);
            api_ = std::make_unique<api::ClashApi>(cfg::controllerBaseUrl(), secret_);
            stream::logApplication("debug", "应用数据库与内核 API 已初始化");
        });
    }

    void fail(std::string error) {
        stream::logApplication("error", "内核操作失败：" + error);
        std::lock_guard lock(mutex_);
        snap_.state = core::CoreState::Failed;
        snap_.lastError = std::move(error);
    }
