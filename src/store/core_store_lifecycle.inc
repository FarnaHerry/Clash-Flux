// core_store_lifecycle.inc — sing-box 内核生命周期
// 由 src/store/core_store.cppm 在 CoreStore 类体内包含。

    // 内核把规则集解析失败报成启动 FATAL（parse rule-set: read rule:
    // unexpected EOF）：本地 .srs 缓存被截断或内容错误时会发生。清掉缓存后
    // 重新预取（curl 会校验 Content-Length）即可自愈；其他启动失败不重试。
    static bool isRuleSetCacheFailure(const std::string& message) {
        return message.find("rule-set") != std::string::npos &&
               message.find("EOF") != std::string::npos;
    }

    void startCore(const std::string& profileYaml, bool detached = false,
                   bool tunEnabled = false,
                   std::optional<bool> systemProxyState = std::nullopt,
                   bool speedTestOnly = false,
                   bool repairRuleSetCache = true) {
        std::lock_guard operationLock(lifecycleMutex_);
        ensureOpen();
        const auto serviceInfo = service::query();
        if (serviceInfo.reachable && !serviceInfo.compatible()) {
            std::string upgradeError;
            if (!service::ensureCompatible(upgradeError)) {
                fail(upgradeError.empty()
                         ? (serviceInfo.error.empty()
                                ? "root 服务版本检查失败"
                                : serviceInfo.error)
                         : upgradeError);
                return;
            }
        }
        stream::logApplication("info", "开始启动代理内核");
        {
            std::lock_guard lock(mutex_);
            // Android 桥线程在应用启动时自动拉起内核，启动慢时（拉远程
            // 规则集等）UI 侧再点启动会 spawn 第二个实例，抢不到控制
            // 端口/混合端口的那个以 exit 1 收场，把「内核启动后立即退出」误报
            // 给用户。启动进行中直接拒绝重入。
            if (snap_.state == core::CoreState::Starting) return;
#if defined(__ANDROID__)
            // The Android core is libbox hosted by ClashVpnService, not an
            // executable path discoverable by the native layer.
            binaryPath_ = "sing-box libbox";
#else
            binaryPath_ = cfg::singboxBinary().string();
            if (binaryPath_.empty() && !service::available()) {
                snap_.state = core::CoreState::Failed;
                snap_.lastError = "未找到 sing-box 内核（engines/ 或 PATH），也未安装服务";
                return;
            }
#endif
            snap_.state = core::CoreState::Starting;
            snap_.lastError.clear();
        }
        lastProfileYaml_ = profileYaml;

        const std::filesystem::path workDir = cfg::coreWorkDir();
        const std::filesystem::path configFile = workDir / "config.json";
        // detached spawn 把内核输出追加进 core.log：记下启动前的长度，失败时
        // 只取本次启动新增的行作为诊断尾部。
        [[maybe_unused]] std::uintmax_t detachedLogSize = 0;
#ifdef _WIN32
        std::string windowsTunInterface;
        std::vector<std::string> windowsNativeInterfaces;
        std::vector<std::string> windowsExclusions;
#endif
        {
            // 两遍编译：第一遍拿到远程规则集清单，直连预取 .srs 缓存（失败
            // 不致命）；第二遍命中本地文件即以 local rule_set 生成——内核
            // 首启不再因代理不可用而拉取失败退出，预取过的缓存按周刷新。
            singbox::CompileOptions options;
            try {
                options = routingOptions(profileYaml, tunEnabled,
                                         speedTestOnly);
            } catch (const std::exception& exception) {
                fail(std::string("读取主 VPN 路由计划失败：") + exception.what());
                return;
            }
            auto compiled = core::generateConfig(options);
            if (compiled.json.empty() || !compiled.error.empty()) {
                fail("订阅编译失败：" + (compiled.error.empty()
                                             ? std::string{"未知错误"}
                                             : compiled.error));
                return;
            }
            {
                auto config = nlohmann::json::parse(compiled.json);
                const std::string proxyGroups = proxyGroupsFromConfig(config);
                {
                    std::lock_guard lock(mutex_);
                    compiledProxyGroups_ = proxyGroups;
                }
                compiled.json = config.dump();
            }
            prefetchRuleSets(compiled.json, workDir);
            options.ruleSetDir = workDir.string();
            compiled = core::generateConfig(options);
            if (compiled.json.empty() || !compiled.error.empty()) {
                fail("订阅编译失败：" + (compiled.error.empty()
                                             ? std::string{"未知错误"}
                                             : compiled.error));
                return;
            }
#ifdef _WIN32
            if (options.tunInbound) {
                for (const auto& native : options.nativeConnections) {
                    if (native.connected && !native.interfaceName.empty())
                        windowsNativeInterfaces.push_back(native.interfaceName);
                }
                if (!windowsNativeInterfaces.empty()) {
                    const auto config = nlohmann::json::parse(compiled.json);
                    for (const auto& inbound : config["inbounds"]) {
                        if (inbound.value("type", "") != "tun") continue;
                        windowsTunInterface = inbound.value("interface_name", "ClashFlux");
                        windowsExclusions = inbound.value("route_exclude_address", std::vector<std::string>{});
                    }
                }
            }
#endif
            // Android's VpnService waits for this file from another thread.
            // Never expose a partially-written JSON document to libbox; an
            // atomic rename also prevents a stale config from being selected
            // while a new subscription is being compiled.
#if defined(__ANDROID__)
            std::error_code staleConfigError;
            std::filesystem::remove(configFile, staleConfigError);
#endif
            const std::filesystem::path tempConfig =
                configFile.string() + ".tmp";
            std::ofstream out(tempConfig, std::ios::binary | std::ios::trunc);
            if (!out) {
                fail("无法写入运行时配置: " + configFile.string());
                return;
            }
            out << compiled.json;
            out.flush();
            out.close();
#ifdef _WIN32
            // std::filesystem::rename does not replace an existing target on
            // Windows. Remove only this app-owned runtime file after the new
            // contents are safely closed, then commit the replacement.
            std::error_code replaceError;
            std::filesystem::remove(configFile, replaceError);
#endif
            std::error_code renameError;
            std::filesystem::rename(tempConfig, configFile, renameError);
            if (renameError) {
                fail("无法提交运行时配置: " + renameError.message());
                return;
            }
            std::lock_guard lock(mutex_);
            snap_.warnings = std::move(compiled.warnings);
        }

        // 三种拉起方式（按优先级）：
        //   1. root 服务托管（装了服务模式 → TUN 等特权操作开箱可用）
        //   2. 接管已在跑的内核（CLI detached spawn / 上次 GUI 残留）：避免
        //      重复 spawn 撞控制端口与混合端口
        //   3. 直接 spawn（默认）
#if defined(__ANDROID__)
        // Android uses the same lifecycle split as desktop: libbox starts
        // immediately without TUN, while a TUN request only changes the
        // service mode after the user grants Android VPN consent.
        {
            std::lock_guard lock(mutex_);
            snap_.mode = mode();
            snap_.mixedPort = mixedPort();
            snap_.allowLan = false;
            snap_.logLevel = logLevel();
            snap_.tunEnabled = tunEnabled;
            snap_.state = tunEnabled || speedTestOnly
                              ? core::CoreState::Stopped
                              : core::CoreState::Starting;
        }
        if (!tunEnabled && !speedTestOnly && !clashflux_android_start_core()) {
            fail("无法启动 Android sing-box 内核服务");
        }
        return;
#else
        if (service::available()) {
            std::string err;
            if (!service::startCore(configFile, err)) {
                fail("服务托管启动失败：" + err);
                return;
            }
            managedByService_ = true;
        } else {
            const bool controllerAnswering = api_->version().ok;
            const long residentPid = readPidFile();
            if (controllerAnswering && residentPid > 0 &&
                core::pidAlive(residentPid)) {
                // 接管本应用自己拉起的驻留内核（pidfile 命中且进程活着：
                // CLI detached spawn / 上次 GUI 残留）。
                adopted_ = true;
            } else if (controllerAnswering) {
                // 控制端口有应答但不是本应用的驻留进程：多半是其他代理
                // 软件占用了端口。明确报错，不误接管、不重复 spawn。
                fail("控制端口被其他程序占用（可能是其他代理软件），内核未启动");
                return;
            } else if (detached) {
                // CLI 驻留形态：setsid 脱离会话 + 日志重定向 + pidfile，CLI
                // 退出内核仍在。本进程不持句柄，按接管形态管理（活判/停止
                // 走 pidfile）。
                std::error_code logSizeError;
                detachedLogSize = std::filesystem::file_size(workDir / "core.log",
                                                             logSizeError);
                if (logSizeError) detachedLogSize = 0;
                std::string err;
                if (!core::spawnDetached(binaryPath_, workDir, configFile, err)) {
                    fail(err);
                    return;
                }
                adopted_ = true;
            } else if (!process_.start(binaryPath_, workDir, configFile)) {
                fail(process_.lastError());
                return;
            }
        }
#endif

        // 等 clash_api 就绪（≤30s）：订阅含 GEOIP 规则时冷启动要拉远程
        // .srs 规则集（可能还走尚未就绪的代理），5s 窗口会误判慢启动为
        // 失败；进程已退出仍立即失败，30s 只是给慢启动的上限。
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        bool ready = false;
        while (std::chrono::steady_clock::now() < deadline) {
            if (!coreAlive()) {
                std::string message = std::format(
                    "内核启动后立即退出（exit {}）",
                    managedByService_ || adopted_ ? -1 : process_.exitCode());
                // 直接 spawn 才有本进程输出管道；附上内核最后的报错行，
                // 把「exit 1」背后的真实原因（端口占用、配置解析、GeoIP
                // 拉取失败等）直接呈给用户。
                if (!managedByService_ && !adopted_) {
                    if (const std::string tailText = process_.recentTail();
                        !tailText.empty()) {
                        message += "：" + tailText;
                    }
                } else if (adopted_) {
                    // 驻留/接管形态：内核输出在 core.log，旧实现只报一句
                    // 「exit -1」，用户无法区分配置、规则集还是网络问题。
                    if (const std::string tailText =
                            detachedTail(workDir, detachedLogSize);
                        !tailText.empty()) {
                        message += "：" + tailText;
                    }
                }
                if (message.find("address already in use") != std::string::npos ||
                    message.find("bind:") != std::string::npos) {
                    message += "（端口可能被其他代理软件占用；可在设置中修改混合端口）";
                }
                if (repairRuleSetCache && isRuleSetCacheFailure(message)) {
                    // 坏缓存会让内核每次启动都 FATAL（且按周刷新的 mtime 判断
                    // 永远认为它新鲜）：清掉 .srs 与内核自带缓存后重试一次。
                    // 递归调用前复位状态，避开启动重入保护。
                    purgeRuleSetCache(workDir);
                    stream::logApplication(
                        "warning",
                        "本地规则集缓存疑似损坏，已清理并重新预取后重试启动内核");
                    {
                        std::lock_guard lock(mutex_);
                        snap_.state = core::CoreState::Stopped;
                        snap_.lastError.clear();
                    }
                    startCore(profileYaml, detached, tunEnabled, systemProxyState,
                              speedTestOnly, /*repairRuleSetCache=*/false);
                    return;
                }
                fail(std::move(message));
                return;
            }
            if (const auto r = api_->version(); r.ok) {
                const auto j = nlohmann::json::parse(r.body, nullptr, false);
                std::lock_guard lock(mutex_);
                snap_.version = j.is_object() ? j.value("version", "") : "";
                ready = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }
        if (!ready) {
            if (managedByService_) {
                std::string err;
                service::stopCore(err);
                managedByService_ = false;
            } else {
                process_.stop();
            }
            fail("external-controller 30s 内未就绪");
            return;
        }

#ifdef _WIN32
        if (!windowsNativeInterfaces.empty()) {
            std::string error;
            windowsRouting_ = vpn::compensation::PrepareWindowsTun(
                windowsTunInterface, windowsNativeInterfaces, windowsExclusions, error);
            if (!windowsRouting_) {
                stopCore();
                fail("Windows PPTP/TUN 补偿失败：" + error);
                return;
            }
        }
#endif
        streams_.start(cfg::controllerWsUrl(), secret_, logLevel());
        {
            std::lock_guard lock(mutex_);
            snap_.state = core::CoreState::Running;
        }
        stream::logApplication("info", "代理内核控制器已就绪");
        refreshRuntime();
        // 只有调用方明确要求恢复本次运行态时才处理系统代理。普通启动
        // 不读取历史设置，也不改变其他软件当前接管的系统代理。
        if (systemProxyState.has_value() && *systemProxyState) {
            const std::string current = sysproxy::currentProxy();
            if (current.empty() || systemProxyOurs()) {
                std::string err;
                if (sysproxy::enable("127.0.0.1", mixedPort(), err)) {
                    setSetting("proxy.system_port", std::to_string(mixedPort()));
                } else {
                    std::lock_guard lock(mutex_);
                    snap_.lastError = "系统代理应用失败：" + err;
                }
            } else {
                std::lock_guard lock(mutex_);
                snap_.lastError =
                    "系统代理当前由其他软件（" + current + "）接管，未重新指向本应用";
            }
        } else if (systemProxyState.has_value() && systemProxyOurs()) {
            std::string err;
            sysproxy::disable(err);
        }
    }

    bool stopCore() {
        std::lock_guard operationLock(lifecycleMutex_);
#ifdef _WIN32
        // Remove native interface fallback defaults while TUN still captures
        // ordinary traffic. PPTP's server route remains owned by its session.
        windowsRouting_.reset();
#endif
        stream::logApplication("info", "请求停止代理内核");
#if defined(__ANDROID__)
        clashflux_android_stop_vpn();
        std::lock_guard lock(mutex_);
        snap_.state = core::CoreState::Stopped;
        snap_.version.clear();
        snap_.tunEnabled = false;
        return true;
#else
        // 先摘系统代理：内核停掉后系统仍指向旧端口会断网。仅当当前代理
        // 确实指向本应用写入的端口时才撤销；其他代理软件接管中不动。
        if (systemProxyOurs()) {
            std::string err;
            sysproxy::disable(err);
        }
        streams_.stop();
        // 正常情况下 managedByService_ 会记录所有权；但 GUI 可能在 START
        // 成功后还没来得及写入标记就被关闭，或者上一次实例异常退出留下了
        // 服务侧内核。只要本地没有直连 CoreProcess，且 root 服务报告
        // 有自己的 core，就补发一次幂等 STOP，避免下一次启动误报“先 STOP”。
        const bool serviceOwnsCore =
            managedByService_ ||
            (!process_.running() && service::reachable() &&
             service::coreRunning());
        if (serviceOwnsCore) {
            std::string err;
            if (!service::stopCore(err)) {
                std::lock_guard lock(mutex_);
                snap_.state = core::CoreState::Failed;
                snap_.lastError = "root 服务停止内核失败：" + err;
                return false;
            } else {
                managedByService_ = false;
            }
            adopted_ = false;
        } else if (process_.running()) {
            process_.stop();
        } else {
            // 接管/外部实例（本进程 detached spawn，或另一进程——CLI、上次
            // GUI 残留——拉起的内核）：本进程没有句柄，经 pidfile 终止。
            adopted_ = false;
            if (const long pid = readPidFile(); pid > 0) core::killPid(pid);
        }
        // 等监视线程收尾（最多 ~2s 宽限 + waitpid）。
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (process_.running() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::error_code ec;
        std::filesystem::remove(cfg::coreWorkDir() / "core.pid", ec);
#if defined(__linux__) && !defined(__ANDROID__)
        std::string routeCleanupError;
        vpn::compensation::CleanupManagedLinuxRoutes(routeCleanupError);
        if (!routeCleanupError.empty()) {
            std::lock_guard lock(mutex_);
            snap_.lastError = "主内核已停止，但系统路由清理失败：" + routeCleanupError;
        }
#endif
        std::lock_guard lock(mutex_);
        snap_.state = core::CoreState::Stopped;
        snap_.version.clear();
        return true;
#endif
    }

    // 订阅/配置变更时重启当前内核。Android 的 libbox 由 VpnService 托管，
    // 但无 TUN 内核也要保持常驻；写完新配置后按当前 TUN 状态恢复对应服务。
    void restartCore(const std::string& profileYaml) {
        std::lock_guard operationLock(lifecycleMutex_);
        const core::CoreState before = snapshot().state;
        const bool wasActive = before == core::CoreState::Running ||
                               before == core::CoreState::Starting;
        const bool tunEnabled = snapshot().tunEnabled;
        const bool systemProxyEnabled = systemProxyOurs();
        if (!stopCore()) return;
        startCore(profileYaml, false, tunEnabled,
                  systemProxyEnabled ? std::optional<bool>(true)
                                     : std::nullopt);
#if defined(__ANDROID__)
        if (wasActive) {
            if (tunEnabled) {
                clashflux_android_start_vpn();
            } else {
                clashflux_android_start_core();
            }
        }
#endif
    }

    // 拉 /configs 刷新运行配置快照（阻塞：UI 必须 RunOnTaskThread）。
    void refreshRuntime() {
        ensureOpen();
#if defined(__ANDROID__)
        // libbox has no Clash-compatible /configs endpoint.  Its VPN state
        // is reflected by snapshot(); configuration is persisted locally.
        return;
#else
        const auto r = api_->configs();
        if (!r.ok) return;
        const auto j = nlohmann::json::parse(r.body, nullptr, false);
        if (!j.is_object()) return;
        std::lock_guard lock(mutex_);
        snap_.mode = j.value("mode", "");
        snap_.mixedPort = j.value("mixed-port", snap_.mixedPort);
        snap_.allowLan = j.value("allow-lan", snap_.allowLan);
        snap_.logLevel = j.value("log-level", snap_.logLevel);
#if !defined(__ANDROID__)
        if (j.contains("tun") && j["tun"].is_object()) {
            snap_.tunEnabled = j["tun"].value("enable", false);
        }
#endif
#endif
    }

    // 切换出站模式（阻塞）。成功即更新快照。
    bool applyMode(const std::string& m) {
        ensureOpen();
#if defined(__ANDROID__)
        // The preferred mode is configuration, not merely live CommandClient
        // state.  Allow changing it before VpnService exists so the next
        // generated config starts in that mode; only require the JNI update
        // when a data plane is actually running.
        if (clashflux_android_vpn_state() == 2 &&
            !clashflux_android_set_clash_mode(m.c_str())) return false;
#else
        const nlohmann::json body = {{"mode", m}};
        const auto r = api_->patchConfigs(body.dump());
        if (!r.ok) return false;
#endif
        setSetting("core.mode", m);
        std::lock_guard lock(mutex_);
        snap_.mode = m;
        return true;
    }

    // 崩溃检测：UI 泵每拍调用；Running 但内核已不在 → Failed。
    void checkAlive() {
#if defined(__ANDROID__)
        std::lock_guard lock(mutex_);
        if (clashflux_android_core_state() == 3) {
            snap_.state = core::CoreState::Failed;
        } else if (clashflux_android_core_state() == 2) {
            snap_.state = core::CoreState::Running;
        } else if (clashflux_android_core_state() == 1) {
            snap_.state = core::CoreState::Starting;
        } else if (snap_.state != core::CoreState::Failed) {
            snap_.state = core::CoreState::Stopped;
        }
        return;
#else
        std::lock_guard lock(mutex_);
        if (snap_.state == core::CoreState::Running && !coreAlive()) {
            snap_.state = core::CoreState::Failed;
            snap_.lastError =
                std::format("sing-box 内核异常退出（exit {}）",
                            managedByService_ || adopted_ ? -1
                                                          : process_.exitCode());
            if (!managedByService_ && !adopted_) {
                if (const std::string tailText = process_.recentTail();
                    !tailText.empty()) {
                    snap_.lastError += "：" + tailText;
                }
            }
        }
#endif
    }
