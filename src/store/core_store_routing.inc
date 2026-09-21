// core_store_routing.inc — 原生连接路由与配置刷新
// 由 src/store/core_store.cppm 在 CoreStore 类体内包含。

    inline bool ensureRunning() {
        std::lock_guard operationLock(lifecycleMutex_);
        if (snapshot().state == core::CoreState::Running) return true;
        ensureOpen();
        for (const auto& profile : db_->listProfiles()) {
            if (!profile.selected || profile.type == "pptp" || profile.type == "openvpn") continue;
            std::ifstream input(cfg::profilesDir() / profile.file, std::ios::binary);
            if (!input) { fail("无法读取当前订阅配置，请先更新或重新导入订阅"); return false; }
            const std::string yaml{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            if (yaml.empty()) { fail("当前订阅配置为空，请先更新订阅"); return false; }
            // 只恢复当前运行态；不要让启动过程重新读取已经可能过期的
            // 持久化 TUN/系统代理开关。系统代理由本次 applySystemProxy
            // 操作在内核就绪后显式打开。
            startCore(yaml, false, snapshot().tunEnabled, std::nullopt);
            return snapshot().state == core::CoreState::Running;
        }
        fail("请先选择一个代理订阅，再开启 TUN 或系统代理");
        return false;
    }

    // ---- 内核控制（阻塞：UI 必须 RunOnTaskThread）----

#ifdef _WIN32
    // Native dialing must run before TUN captures are installed: an existing
    // TUN /32 could otherwise beat the new server's physical /32 route.
    inline std::unique_lock<std::recursive_mutex> lockNativeLifecycle() {
        return std::unique_lock(lifecycleMutex_);
    }
    inline bool resumeNativeRouting(std::string& error) {
        std::lock_guard operationLock(lifecycleMutex_);
        const bool tunEnabled = snapshot().tunEnabled;
        const bool systemProxyEnabled = systemProxyOurs();
        startCore(lastProfileYaml_, false, tunEnabled,
                  systemProxyEnabled ? std::optional<bool>(true)
                                     : std::nullopt);
        if (snapshot().state == core::CoreState::Running) return true;
        error = snapshot().lastError;
        return false;
    }
#endif

    // Called by the native store after establishing a session, or BEFORE
    // tearing one down. Suspend unavailable destinations before the interface
    // can disappear or be reused by another connection.
    inline bool updateNativeRouting(std::vector<singbox::NativeConnection> sessions,
                                    std::string& error) {
        std::lock_guard operationLock(lifecycleMutex_);
        nativeSessions_ = std::move(sessions);
        return refreshRouting(error);
    }

    // Recompile saved global rules on every policy/session change. Core
    // lifecycle operations are serialized so a TUN toggle cannot race a VPN
    // connect/disconnect and restore an obsolete configuration.
    inline bool refreshRouting(std::string& error) {
        std::lock_guard operationLock(lifecycleMutex_);
        ensureOpen();
        error.clear();
        const bool running = snapshot().state == core::CoreState::Running;
        try {
            const bool tunEnabled = snapshot().tunEnabled;
            const bool systemProxyEnabled = systemProxyOurs();
            auto options = routingOptions(lastProfileYaml_, tunEnabled);
            const auto compiled = core::generateConfig(std::move(options));
            if (!compiled.error.empty() || compiled.json.empty()) {
                error = "路由计划编译失败：" + compiled.error;
                return false;
            }
            if (!running) return true;
            if (!stopCore()) {
                error = snapshot().lastError;
                return false;
            }
            startCore(lastProfileYaml_, false, tunEnabled,
                      systemProxyEnabled ? std::optional<bool>(true)
                                         : std::nullopt);
#if defined(__ANDROID__)
            if (snapshot().state != core::CoreState::Failed) {
                if (tunEnabled) {
                    clashflux_android_start_vpn();
                } else {
                    clashflux_android_start_core();
                }
                return true;
            }
#else
            if (snapshot().state == core::CoreState::Running) return true;
#endif
            error = snapshot().lastError;
            if (error.empty()) error = "应用路由计划后主 VPN 未能启动";
            // Never leave the old core using an interface that is being
            // released. Stopping the main core is also required on failure.
            stopCore();
            fail(error);
            return false;
        } catch (const std::exception& exception) {
            error = exception.what();
            return false;
        }
    }

    // 启动内核；profileYaml 为启用订阅的原文（无订阅传空）。
    // tunEnabled/systemProxyState 是本次操作捕获的运行态，不从 settings
    // 表重新读取。这样普通 core start 只启动内核，不会隐式恢复 TUN 或
    // 系统代理；恢复上次接管状态的启动路径必须显式传入这些状态。
    // detached=true（CLI core start）：直连 spawn 走 setsid 脱离会话驻留，
    // 本进程退出不带走内核。
