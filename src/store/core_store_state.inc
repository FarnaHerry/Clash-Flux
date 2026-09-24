// core_store_state.inc — 状态、设置与平台运行态
// 由 src/store/core_store.cppm 在 CoreStore 类体内包含。

    void init() {
        ensureOpen();
    }

    void setAndroidRuntimeStats(std::int64_t uploadRate, std::int64_t downloadRate,
                                std::int64_t uploadTotal, std::int64_t downloadTotal,
                                int connectionCount) {
#if defined(__ANDROID__)
        std::lock_guard lock(mutex_);
        snap_.uploadRate = uploadRate;
        snap_.downloadRate = downloadRate;
        snap_.uploadTotal = uploadTotal;
        snap_.downloadTotal = downloadTotal;
        snap_.connectionCount = connectionCount;
#else
        static_cast<void>(uploadRate); static_cast<void>(downloadRate);
        static_cast<void>(uploadTotal); static_cast<void>(downloadTotal);
        static_cast<void>(connectionCount);
#endif
    }

    void setAndroidRuntimeState(int state, const std::string& message) {
#if defined(__ANDROID__)
        std::lock_guard lock(mutex_);
        switch (state) {
            case 1: snap_.state = core::CoreState::Starting; break;
            case 2: snap_.state = core::CoreState::Running; snap_.lastError.clear(); break;
            case 3: snap_.state = core::CoreState::Failed; snap_.lastError = message; break;
            default: snap_.state = core::CoreState::Stopped; break;
        }
#else
        static_cast<void>(state); static_cast<void>(message);
#endif
    }

    void startAndroidApiStreams() {
#if defined(__ANDROID__)
        // libbox is hosted by ClashVpnService and exposes status through the
        // Java-side CommandClient.  It does not provide the desktop
        // clash_api HTTP/WS controller at cfg::controllerWsUrl().  Keep this
        // legacy entry point as a no-op so Android can never start the three
        // invalid desktop WebSocket channels after TUN attachment.
#endif
    }

    void stopAndroidApiStreams() {
#if defined(__ANDROID__)
        streams_.stop();
#endif
    }

    db::Db& db() {
        ensureOpen();
        return *db_;
    }

    api::ClashApi& api() {
        ensureOpen();
        return *api_;
    }

    stream::CoreStreams& streams() { return streams_; }
    core::CoreProcess& process() { return process_; }

    std::string secret() {
        ensureOpen();
        return secret_;
    }

    CoreSnapshot snapshot() {
        std::lock_guard lock(mutex_);
        CoreSnapshot s = snap_;
#if defined(__ANDROID__)
        // libbox is hosted by ClashVpnService and intentionally has no
        // REST controller of its own. Core state and TUN state are separate:
        // the no-TUN core is Running while the Android VPN switch remains off.
        switch (clashflux_android_core_state()) {
            case 1: s.state = core::CoreState::Starting; break;
            case 2: s.state = core::CoreState::Running; break;
            case 3: s.state = core::CoreState::Failed; break;
            default:
                if (s.state != core::CoreState::Failed) s.state = core::CoreState::Stopped;
                break;
        }
        s.version = s.state == core::CoreState::Running ? "sing-box libbox" : "";
        s.tunEnabled = clashflux_android_vpn_state() == 1 ||
                       clashflux_android_vpn_state() == 2;
#endif
        s.binaryPath = binaryPath_;
        return s;
    }

    // ---- 设置（落 settings 表）----
    std::string setting(const std::string& key, const std::string& fallback = "") {
        try {
            // Database creation can fail on a freshly installed Android app
            // (storage/SQLite initialization happens during the first frame).
            // Settings are optional, so an unavailable store must not abort
            // the UI process; use the caller's default and let later actions
            // retry initialization.
            ensureOpen();
            return db_->getSetting(key, fallback);
        } catch (...) {
            return fallback;
        }
    }

    void setSetting(const std::string& key, const std::string& value) {
        try {
            ensureOpen();
            db_->setSetting(key, value);
        } catch (...) {
        }
    }

    std::string proxyGroupsSnapshot() {
        {
            std::lock_guard lock(mutex_);
            if (!compiledProxyGroups_.empty()) return compiledProxyGroups_;
        }

        // Android's libbox CommandClient does not exist until the VPN service
        // is started.  Compile the selected subscription on demand so the
        // proxy page remains usable while the data plane is stopped.  The
        // selected node is persisted by selectProxy() and will be applied by
        // the next startCore() compilation.
        std::lock_guard operationLock(lifecycleMutex_);
        {
            std::lock_guard lock(mutex_);
            if (!compiledProxyGroups_.empty()) return compiledProxyGroups_;
        }
        ensureOpen();

        std::string profileYaml;
        for (const auto& profile : db_->listProfiles()) {
            if (!profile.selected || profile.type == "pptp" ||
                profile.type == "openvpn" || profile.file.empty()) {
                continue;
            }
            std::ifstream input(cfg::profilesDir() / profile.file,
                                std::ios::binary);
            if (input) {
                profileYaml.assign(std::istreambuf_iterator<char>(input),
                                   std::istreambuf_iterator<char>());
            }
            break;
        }
        if (profileYaml.empty()) return {};

        try {
            const auto options = routingOptions(profileYaml, false);
            const auto compiled = core::generateConfig(options);
            if (compiled.json.empty() || !compiled.error.empty()) return {};
            auto config = nlohmann::json::parse(compiled.json, nullptr, false);
            if (!config.is_object()) return {};
            const std::string snapshot = proxyGroupsFromConfig(config);
            std::lock_guard lock(mutex_);
            compiledProxyGroups_ = snapshot;
            return compiledProxyGroups_;
        } catch (...) {
            return {};
        }
    }

    void invalidateProxyGroupsSnapshot() {
        std::lock_guard lock(mutex_);
        compiledProxyGroups_.clear();
    }

    bool selectProxy(const std::string& group, const std::string& name) {
        bool selected = true;
#if defined(__ANDROID__)
        if (clashflux_android_vpn_state() == 2)
            selected =
                clashflux_android_select_outbound(group.c_str(), name.c_str());
#else
        if (snapshot().state == core::CoreState::Running)
            selected = api_->selectProxy(group, name).ok;
#endif
        if (!selected) return false;
        setSetting("proxy.selection." + group, name);
        invalidateProxyGroupsSnapshot();
        return true;
    }

    std::string mode() { return setting("core.mode", "rule"); }
    int mixedPort() {
        try {
            return std::stoi(setting("core.mixed_port", "7899"));
        } catch (...) {
            return 7899;
        }
    }
    bool allowLan() { return setting("core.allow_lan", "false") == "true"; }
    bool ipv6Enabled() { return setting("core.ipv6_enabled", "false") == "true"; }
    std::string logLevel() { return setting("core.log_level", "info"); }
    // TUN 开关（持久化；Android 由 VpnService 交付 fd 后重启内核生效）。
    bool tunEnabled() {
#if defined(__ANDROID__)
        // Android 的系统 VPN 由 ClashVpnService 建立；fd 就绪后 bridge 会重启
        // 内核。这里必须保留用户设置，令 generateConfig 生成 tun inbound，
        // 否则 VPN 会建立却没有数据面接管流量。
        return setting("core.tun_enabled", "false") == "true";
#else
        return setting("core.tun_enabled", "false") == "true";
#endif
    }
    // 系统代理开关（持久化；写入 KDE kioslaverc / GNOME gsettings）。
    bool systemProxyEnabled() {
#if defined(__ANDROID__)
        return false;
#else
        return setting("proxy.system_enabled", "false") == "true";
#endif
    }
    // 当前系统代理是否确实指向本应用。与 systemProxyEnabled() 中保存的
    // 用户意图不同；托盘状态图标只在内核运行时依据该实际状态着色。
    // 桌面端可能需要调用系统命令读取代理设置，调用方必须放在任务线程。
    bool systemProxyActive() { return systemProxyOurs(); }
    bool systemProxySupported() { return sysproxy::supported(); }

    // 启动残留清理：上次异常退出可能把系统代理留在指向本应用（此刻已
    // 不存在的）端口上。仅当当前代理确实指向本应用写入的端口时才撤销；
    // 指向其他代理软件（clash-verge/FlClash 等）时不动其设置。不改动
    // “应当接管”的意图标志——是否恢复接管由调用方显式传入运行态决定。
    void releaseStaleOwnedSystemProxy() {
#if !defined(__ANDROID__)
        if (!systemProxyOurs()) return;
        std::string err;
        sysproxy::disable(err);
#endif
    }

    // 切换 TUN（阻塞）。TUN 是与内核启停解耦的独立开关：
    // - 内核在跑：sing-box 的 clash_api 不支持热更 tun → 重新合成 config.json
    //   并重启内核生效；失败（如无 root/CAP_NET_ADMIN）回滚设置并恢复无 TUN 运行；
    // - 内核没跑：只写配置（下次启动内核时带上 tun），不再隐式拉起内核。
    // 成功更新快照。
    bool applyTun(bool enable) {
        std::lock_guard operationLock(lifecycleMutex_);
#if defined(__ANDROID__)
        (void)enable;
        return false;
#else
        ensureOpen();
        setSetting("core.tun_enabled", enable ? "true" : "false");
        {
            std::lock_guard lock(mutex_);
            snap_.tunEnabled = enable;
        }
        if (snapshot().state != core::CoreState::Running) {
            // 只记意图；内核怎么启动由「启动入口 + 启动策略」决定。
            return true;
        }
        const bool restoreSystemProxy = systemProxyOurs();
        if (!stopCore()) {
            setSetting("core.tun_enabled", enable ? "false" : "true");
            {
                std::lock_guard lock(mutex_);
                snap_.tunEnabled = !enable;
            }
            return false;
        }
        startCore(lastProfileYaml_, false, enable, restoreSystemProxy);
        if (snapshot().state == core::CoreState::Running) return true;
        setSetting("core.tun_enabled", enable ? "false" : "true");
        {
            std::lock_guard lock(mutex_);
            snap_.tunEnabled = !enable;
            snap_.lastError =
                std::format("TUN {}失败（可能需要 root/CAP_NET_ADMIN）",
                            enable ? "开启" : "关闭");
        }
        // 恢复无 TUN 的可用状态（best effort）。
        stopCore();
        startCore(lastProfileYaml_, false, !enable, restoreSystemProxy);
        return false;
#endif
    }

    // 切换系统代理（阻塞 shell 调用）。系统代理是与内核启停解耦的独立开关：
    // - 关闭：只要当前系统代理是我方写入的就撤销（与内核是否运行无关，
    //   避免系统一直指向已死端口）；指向其他软件时不动系统，仅清开关；
    // - 开启且内核在跑：立即写 OS 代理并记录端口；
    // - 开启但内核没跑：只记意图（下次启动内核时由 startCore 恢复），
    //   返回成功表示「意图已记录」。
    bool applySystemProxy(bool enable) {
        std::lock_guard operationLock(lifecycleMutex_);
#if defined(__ANDROID__)
        (void)enable;
        return false;
#else
        ensureOpen();
        if (!enable) {
            std::string err;
            bool ok = true;
            if (systemProxyOurs()) ok = sysproxy::disable(err);
            if (!ok) {
                std::lock_guard lock(mutex_);
                snap_.lastError = "系统代理关闭失败：" + err;
                return false;
            }
            setSetting("proxy.system_enabled", "false");
            return true;
        }
        const std::string previousSetting =
            setting("proxy.system_enabled", "false");
        setSetting("proxy.system_enabled", "true");
        if (snapshot().state != core::CoreState::Running) return true;
        std::string err;
        if (!sysproxy::enable("127.0.0.1", mixedPort(), err)) {
            setSetting("proxy.system_enabled", previousSetting);
            std::lock_guard lock(mutex_);
            snap_.lastError = "系统代理开启失败：" + err;
            return false;
        }
        setSetting("proxy.system_port", std::to_string(mixedPort()));
        return true;
#endif
    }

    // Read the selected profile at the time of the action, including after a
    // stopped-core subscription switch. Never silently start an empty profile.
