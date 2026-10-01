// core_store_helpers.inc — 私有编排辅助函数
// 由 src/store/core_store.cppm 在 CoreStore 类体内包含。

    // 代理页快照（组 + 每节点 type/udp）由 clashflux.singbox 的纯函数生成，
    // 便于单测；这里只把「持久化的组选中项」从 settings 里喂进去。
    std::string proxyGroupsFromConfig(nlohmann::json& config,
        const std::vector<singbox::SourceObject>& objects = {}) {
        std::string main;
        for (const auto& profile : db_->listProfiles())
            if (profile.selected) main = routing::ConnectionId(profile.id);
        return singbox::BuildProxySnapshot(config, [this, &objects, &main](const std::string& group) {
            const auto key = "proxy.selection." + group;
            auto saved = setting(key, "");
            if (!saved.empty()) return saved;
            // 仅当前 main 迁移历史无来源键；次来源不继承其它订阅的同名选择。
            for (const auto& object : objects) {
                if (object.tag != group || object.sourceId != main || object.kind != vpn::TargetKind::Group) continue;
                const auto legacy = setting("proxy.selection." + object.objectId, "");
                for (const auto& member : objects) {
                    if (member.sourceId == main && member.objectId == legacy) {
                        setSetting(key, member.tag); return member.tag;
                    }
                }
            }
            return std::string{};
        });
    }

    inline singbox::CompileOptions routingOptions(
        const std::string& profileYaml, bool tunEnabled,
        bool speedTestOnly = false, const vpn::VpnPolicy* overridePolicy = nullptr,
        const std::optional<std::string>& mainOverride = std::nullopt,
        const singbox::ProfileSource* updated = nullptr, bool includeAuxiliary = true) {
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
        auto profiles = db_->listProfiles();
        if (mainOverride) {
            for (auto& profile : profiles)
                if (profile.type != "pptp" && profile.type != "openvpn")
                    profile.selected = routing::ConnectionId(profile.id) == *mainOverride;
        }
        const auto policy = !includeAuxiliary ? vpn::VpnPolicy{} : overridePolicy ? *overridePolicy
            : routing::DecodePolicy(setting("vpn.global_policy", ""));
        std::string policyError;
        if (!routing::ValidatePlatformPolicy(policy, profiles, policyError)) throw std::runtime_error(policyError);
        routing::PopulateOptions(options, profiles, policy, nativeSessions_);
        if (updated && updated->id == options.mainConnectionId) options.profileYaml = updated->content;
#if !defined(__ANDROID__) && !defined(CLASHFLUX_IOS)
        if (includeAuxiliary) for (const auto& profile : profiles) {
            if (profile.selected || profile.type == "pptp" || profile.type == "openvpn") continue;
            const auto id = routing::ConnectionId(profile.id);
            if (!std::ranges::any_of(policy.rules, [&](const auto& rule) {
                    return rule.enabled && rule.connectionId == id;
                })) continue;
            if (updated && updated->id == id) {
                options.auxiliarySources.push_back({id, profile.name, updated->content}); continue;
            }
            std::ifstream input(cfg::profilesDir() / profile.file, std::ios::binary);
            if (!input || profile.file.empty()) {
                options.auxiliarySources.push_back({id, profile.name, "", false}); continue;
            }
            options.auxiliarySources.push_back({id, profile.name,
                std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>())});
        }
#endif
        return options;
    }

    bool checkCompiledPlan(const singbox::CompileResult& compiled, std::string& error) {
        if (!compiled.error.empty() || compiled.json.empty()) {
            error = compiled.error.empty() ? "编排没有有效配置" : compiled.error; return false;
        }
#if !defined(__ANDROID__) && !defined(CLASHFLUX_IOS)
        const auto file = cfg::coreWorkDir() / ("candidate-" + cfg::randomSecret() + ".json");
        struct CandidateFile {
            std::filesystem::path path;
            ~CandidateFile() { std::error_code ec; std::filesystem::remove(path, ec); }
        } cleanup{file};
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        output << compiled.json; output.close();
        if (!output) { error = "无法写入候选编排"; return false; }
        return core::checkConfig(cfg::singboxBinary(), cfg::coreWorkDir(), file, error);
#else
        // libbox 在后台启动时核验；没有谎报成桌面的 CLI 预检查。
        error.clear(); return true;
#endif
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
    // 而按周刷新的 mtime 判断永远认为它"新鲜"。只清理应用自有 GEO 与
    // 默认内核 cache.db；用户提供的 .srs 不属于此缓存。
    void purgeRuleSetCache(const std::filesystem::path& workDir,
                          const std::vector<singbox::RuleSetResource>& resources) {
        if (resources.empty()) return;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(workDir, ec)) {
            if (ec) break;
            std::error_code typeError;
            if (!entry.is_regular_file(typeError) || typeError) continue;
            const std::string name = entry.path().filename().string();
            bool ruleSetCache = false;
#if !defined(__ANDROID__)
            const auto suffix = name.ends_with(".srs.part") ? std::string_view{".srs.part"}
                              : name.ends_with(".srs") ? std::string_view{".srs"}
                                                       : std::string_view{};
            if (!suffix.empty()) {
                const auto tag = std::string_view(name).substr(0, name.size() - suffix.size());
                const auto url = singbox::BuiltinRuleSetUrl(tag);
                ruleSetCache = !url.empty() && std::ranges::any_of(resources,
                    [&](const auto& resource) {
                        return resource.tag == tag && resource.url == url;
                    });
            }
#endif
            const bool engineCache =
                name == "cache.db" || name.starts_with("cache.db.");
            if (!ruleSetCache && !engineCache) continue;
            std::error_code removeError;
            std::filesystem::remove(entry.path(), removeError);
        }
    }

    // 编译器清单包含未命中与已命中的自有 GEO，才能真正按周刷新。
    // 不扫描原生 JSON 的远程来源，不改下载 detour/format/更新策略。
    void prefetchRuleSets(const std::vector<singbox::RuleSetResource>& resources,
                          const std::filesystem::path& workDir) {
#if defined(__ANDROID__)
        // Android GEO 使用 Gradle 固定来源资产；不能改为运行时刷新。
        return;
#endif
        for (const auto& resource : resources) {
            const auto& tag = resource.tag;
            const auto& url = resource.url;
            const auto ownedUrl = singbox::BuiltinRuleSetUrl(tag);
            if (ownedUrl.empty() || url != ownedUrl) continue;
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
            download.validate = [](const std::filesystem::path& temp) {
                return singbox::RuleSetCacheValid(temp) ? std::string{}
                    : std::string{"文件不是兼容的 SRS 规则集（文件头校验失败）"};
            };
            const auto result = api_->downloadToFile(url, dest, download);
            // 校验临时文件后才提交，HTTP 200 错误页也不能毁掉旧缓存。
            // 校验仅筛文件头，完整解压与规则校验仍由内核负责。
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
