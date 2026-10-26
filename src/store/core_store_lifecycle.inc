// core_store_lifecycle.inc — sing-box 内核生命周期
// 由 src/store/core_store.cppm 在 CoreStore 类体内包含。

    // Blocking startup maintenance shares the core lifecycle lock: an explicit
    // start/reconfigure cannot race a service payload replacement.
    service::StartupResult synchronizeService() {
        std::lock_guard operationLock(lifecycleMutex_);
        auto healthPause = streams_.pauseHealth();
        return service::synchronizeInstalled();
    }

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
                   bool repairRuleSetCache = true,
                   bool startVpn = true) {
        std::lock_guard operationLock(lifecycleMutex_);
        // 任何未捕获异常都必须让状态离开 Starting：以前 json::parse /
        // filesystem 抛出会越过 fail()，把 snap_.state 永久留在 Starting，而
        // 「启动中拒绝重入」的保护会因此挡掉之后所有启动，直到重启应用。
        try {
            startCoreUnchecked(profileYaml, detached, tunEnabled,
                               systemProxyState, speedTestOnly,
                               repairRuleSetCache, startVpn);
        } catch (const std::exception& exception) {
            settleStartFailure(std::string{"内核启动异常："} + exception.what());
        } catch (...) {
            settleStartFailure("内核启动未知异常");
        }
        preparedStart_.reset();
        preparedOptions_.reset();
    }

    // 异常只记错误、不把已经跑起来的内核翻成 Failed；仍停在 Starting 时才走 fail()。
    void settleStartFailure(const std::string& error) {
        {
            std::lock_guard lock(mutex_);
            if (snap_.state != core::CoreState::Starting) {
                snap_.lastError = error;
                stream::logApplication("warning", "内核启动流程异常：" + error);
                return;
            }
        }
        fail(error);
    }

    void startCoreUnchecked(const std::string& profileYaml, bool detached = false,
                            bool tunEnabled = false,
                            std::optional<bool> systemProxyState = std::nullopt,
                            bool speedTestOnly = false,
                            bool repairRuleSetCache = true,
                            bool startVpn = true) {
#if defined(CLASHFLUX_IOS)
        // iOS only has the Packet Tunnel data plane. Never accept a desktop-style
        // proxy-only launch, even if an old preference or caller passes false.
        tunEnabled = true;
#endif
        std::lock_guard operationLock(lifecycleMutex_);
        auto healthPause = streams_.pauseHealth();
        ensureOpen();
#ifdef _WIN32
        // All Windows data-plane work belongs to SCM, including non-TUN mode.
        std::string windowsServiceError;
        if (!service::ensureCompatible(windowsServiceError)) {
            fail(windowsServiceError);
            return;
        }
#endif
        const auto serviceInfo = service::query();
        if (serviceInfo.reachable && !serviceInfo.compatible()) {
            std::string upgradeError;
            if (!service::ensureCompatible(upgradeError)) {
                fail(upgradeError.empty()
                         ? (serviceInfo.error.empty()
                                ? "内核服务版本检查失败"
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
#if defined(__ANDROID__) || defined(CLASHFLUX_IOS)
            // The Android core is libbox hosted by ClashVpnService, not an
            // executable path discoverable by the native layer. iOS likewise
            // hosts Libbox inside its Packet Tunnel extension.
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
            snap_.runningSince = {};
            snap_.lastError.clear();
        }
        lastProfileYaml_ = profileYaml;

        const std::filesystem::path workDir = cfg::coreWorkDir();
        const std::filesystem::path configFile = workDir / "config.json";
        std::vector<singbox::RuleSetResource> ruleSetResources;
        bool geoRepairSafe = false;
        [[maybe_unused]] bool residentConfigMatches = false;
        // detached spawn 把内核输出追加进 core.log：记下启动前的长度，失败时
        // 只取本次启动新增的行作为诊断尾部。
        [[maybe_unused]] std::uintmax_t detachedLogSize = 0;
#ifdef _WIN32
        std::string windowsTunInterface;
        std::vector<std::string> windowsNativeInterfaces;
        std::vector<std::string> windowsExclusions;
#endif
        {
            auto plan = std::move(preparedStart_);
            std::string prepareError;
            if (!plan) {
                auto options = preparedOptions_ ? std::move(*preparedOptions_) :
                    routingOptions(profileYaml, tunEnabled, speedTestOnly);
                preparedOptions_.reset();
                plan = prepareStartPlan(std::move(options), prepareError);
            }
            if (!plan) { fail("规则集/订阅准备失败：" + prepareError); return; }
            auto& options = plan->options;
            auto& compiled = plan->compiled;
            auto config = nlohmann::json::parse(compiled.json);
            // Re-read after preparation: another client may have started while
            // downloads/checks were running. Reject before replacing config.json.
            if (!checkNetworkConflicts(options, config, prepareError)) { fail(prepareError); return; }
#ifdef _WIN32
            if (options.tunInbound) {
                for (const auto& native : options.nativeConnections)
                    if (native.connected && !native.interfaceName.empty())
                        windowsNativeInterfaces.push_back(native.interfaceName);
                if (!windowsNativeInterfaces.empty()) for (const auto& inbound : config["inbounds"]) {
                    if (inbound.value("type", "") != "tun") continue;
                    windowsTunInterface = inbound.value("interface_name", "ClashFlux");
                    windowsExclusions = inbound.value("route_exclude_address", std::vector<std::string>{});
                }
            }
#endif
            std::ifstream previousConfig(configFile, std::ios::binary);
            if (previousConfig) {
                const std::string text(std::istreambuf_iterator<char>(previousConfig), {});
                const auto previous = nlohmann::json::parse(text, nullptr, false);
                residentConfigMatches = !previous.is_discarded() && previous == config;
            }
            previousConfig.close(); // Windows replacement needs the read handle closed.
            ruleSetResources = compiled.ruleSetResources;
            if (config.contains("route") && config["route"].contains("rule_set"))
                geoRepairSafe = clashflux::rule_cache::MayRepairGeo(config["route"]["rule_set"], ruleSetResources, workDir);
            std::string commitError;
            if (!api::CommitFile(plan->file, configFile, commitError)) { fail(commitError); return; }
            lastPreparedOptions_ = options;
            lastPreparedJson_ = compiled.json;
            std::lock_guard lock(mutex_);
            compiledProxyGroups_ = std::move(plan->proxyGroups);
            snap_.warnings = std::move(compiled.warnings);
            snap_.fidelity = std::move(compiled.fidelity);
            snap_.sourceObjects = std::make_shared<const std::vector<singbox::SourceObject>>(std::move(compiled.sourceObjects));
            previewSourceObjects_ = snap_.sourceObjects;
            snap_.participatingSources = std::move(compiled.participatingSources);
        }

        // 三种拉起方式（按优先级）：
        //   1. root 服务托管（装了服务模式 → TUN 等特权操作开箱可用）
        //   2. 接管已在跑的内核（CLI detached spawn / 上次 GUI 残留）：避免
        //      重复 spawn 撞控制端口与混合端口
        //   3. 直接 spawn（默认）
#if defined(__ANDROID__)
        // Android compiles the selected profile in the UI process, then hands
        // its TUN intent to the isolated VpnService runtime after the system
        // has authorized it. Reopening the app must preserve that persisted
        // mode instead of replacing the running tunnel with proxy-only config.
        {
            std::lock_guard lock(mutex_);
            snap_.mode = mode();
            snap_.mixedPort = mixedPort();
            snap_.allowLan = false;
            snap_.logLevel = logLevel();
            snap_.tunEnabled = tunEnabled && startVpn;
            snap_.state = (tunEnabled && startVpn) || speedTestOnly
                              ? core::CoreState::Stopped
                              : core::CoreState::Starting;
        }
        if (!speedTestOnly) {
            // startVpn=false：按持久化 TUN 意图编译（保留 config.json 的 TUN
            // inbound，供服务被系统回收后按 VPN 模式恢复），但只以 core-only
            // 拉起常驻服务——Java 侧对 core-only 请求已有保护：VPN 存活时保留
            // 隧道模式且不重启数据面，未运行则只起无 TUN 内核。UI 打开软件因此
            // 不会自动启动代理；TUN 只由用户显式操作（首页悬浮按钮/设置开关/
            // 快捷磁贴）或服务回收恢复拉起。
            if (tunEnabled && startVpn) {
                clashflux_android_start_vpn();
            } else if (!clashflux_android_start_core()) {
                fail("无法启动 Android sing-box 内核服务");
            }
        }
        return;
#else
        const bool useService =
#ifdef _WIN32
            true; // A failed/restarting SCM service must never trigger GUI spawn.
#else
            service::available();
#endif
        if (useService) {
            std::string err;
            if (!service::startCore(configFile, err)) {
                fail("服务托管启动失败：" + err);
                return;
            }
            managedByService_ = true;
#ifdef _WIN32
            serviceDetached_ = detached;
#endif
        } else {
            const bool controllerAnswering = api_->version().ok;
            const long residentPid = readPidFile();
            if (controllerAnswering && residentPid > 0 &&
                core::pidAlive(residentPid)) {
                // 接管本应用自己拉起的驻留内核（pidfile 命中且进程活着：
                // CLI detached spawn / 上次 GUI 残留）。
                const auto image = core::pidImageName(residentPid);
                if (!image.empty() && image.find("sing-box") == std::string::npos) {
                    fail("驻留 PID 已不是 sing-box，拒绝接管或终止"); return;
                }
                if (residentConfigMatches) adopted_ = true;
                else {
                    // 不能用新计划的元数据冒充仍运行旧配置的驻留内核。
                    core::killPid(residentPid);
                    const auto stopDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
                    while (core::pidAlive(residentPid) && std::chrono::steady_clock::now() < stopDeadline)
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    if (core::pidAlive(residentPid)) { fail("旧驻留内核未退出，新计划未应用"); return; }
                    if (detached) {
                        std::string error;
                        if (!core::spawnDetached(binaryPath_, workDir, configFile, error)) { fail(error); return; }
                        adopted_ = true;
                    } else if (!process_.start(binaryPath_, workDir, configFile)) {
                        fail(process_.lastError()); return;
                    }
                }
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

        // 等鉴权原生控制服务就绪（≤30s）：订阅含 GEOIP 规则时冷启动要拉远程
        // .srs 规则集（可能还走尚未就绪的代理），5s 窗口会误判慢启动为
        // 失败；进程已退出仍立即失败，30s 只是给慢启动的上限。
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        bool ready = false;
        while (std::chrono::steady_clock::now() < deadline) {
            if (!coreAlive()) {
                std::string message = std::format(
                    "内核启动后立即退出（exit {}）",
                    managedByService_ || adopted_ ? -1 : process_.exitCode());
#ifdef _WIN32
                if (managedByService_) {
                    message = service::coreFailure();
                    if (message.empty()) message = "服务确认内核已退出；当前服务未提供启动诊断，请更新服务后重试";
                }
#endif
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
                if (repairRuleSetCache && geoRepairSafe && !ruleSetResources.empty() &&
                    isRuleSetCacheFailure(message)) {
                    // 坏缓存会让内核每次启动都 FATAL（且按周刷新的 mtime 判断
                    // 永远认为它新鲜）：归属明确时只清本次自有 GEO 文件；
                    // 原生共享缓存保留，不能破坏个人/订阅资源的离线恢复。
                    // 递归调用前复位状态，避开启动重入保护。
                    purgeRuleSetCache(workDir, ruleSetResources);
                    stream::logApplication(
                        "warning",
                        "自有 GEO 缓存疑似损坏，尝试清理并重新预取后重试启动内核");
                    {
                        std::lock_guard lock(mutex_);
                        snap_.state = core::CoreState::Stopped;
                        snap_.lastError.clear();
                    }
                    startCoreUnchecked(profileYaml, detached, tunEnabled,
                                       systemProxyState, speedTestOnly,
                                       /*repairRuleSetCache=*/false, startVpn);
                    return;
                }
                fail(std::move(message));
                return;
            }
            if (const auto r = nativeApi_->runtime(); r.ok) {
                std::lock_guard lock(mutex_);
                snap_.version = r.version;
                snap_.nativeStartedAt = r.startedAt;
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
            fail("原生控制接口 30s 内未就绪");
            return;
        }

#ifdef _WIN32
        if (!windowsNativeInterfaces.empty()) {
            std::string error;
            if (!service::prepareWindowsTun(
                    windowsTunInterface, windowsNativeInterfaces, windowsExclusions, error)) {
                stopCore();
                fail("Windows PPTP/TUN 补偿失败：" + error);
                return;
            }
        }
#endif
        streams_.start(cfg::controllerWsUrl(), secret_, logLevel(), cfg::nativeControllerBaseUrl());
        {
            std::lock_guard lock(mutex_);
            snap_.state = core::CoreState::Running;
            snap_.runningSince = std::chrono::steady_clock::now();
            lastAppliedOptions_ = lastPreparedOptions_;
            lastAppliedJson_ = lastPreparedJson_;
            // The ready instance is running this accepted plan. sing-box's
            // compatibility /configs does not report TUN status; leaving the
            // default false here makes the next routing handoff remove TUN.
            clashflux::runtime_plan::Ready(snap_, lastAppliedOptions_ && lastAppliedOptions_->tunInbound,
                lastAppliedOptions_ ? lastAppliedOptions_->planRevision : 0);
            setSetting("routing.applied_revision", std::to_string(snap_.planRevision));
        }
        stream::logApplication("info", "代理内核控制器已就绪");
        refreshRuntime();
        captureHealthEvidence(false);
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
            if (!sysproxy::disable(err) || systemProxyOurs()) {
                std::lock_guard lock(mutex_);
                snap_.lastError = "系统代理撤销失败：" +
                    (err.empty() ? std::string{"系统设置仍指向本应用"} : err);
            }
        }
    }

    bool stopCore() {
        std::lock_guard operationLock(lifecycleMutex_);
        auto healthPause = streams_.pauseHealth();
#ifdef _WIN32
        serviceDetached_ = false;
#endif
        stream::logApplication("info", "请求停止代理内核");
#if defined(__ANDROID__)
        if (!clashflux_android_stop_vpn()) {
            std::lock_guard lock(mutex_);
            snap_.state = core::CoreState::Failed;
            snap_.lastError = "Android 后台服务未确认内核退出";
            return false;
        }
        std::lock_guard lock(mutex_);
        snap_.state = core::CoreState::Stopped;
        snap_.runningSince = {};
        snap_.version.clear();
        clashflux::runtime_plan::Stopped(snap_);
        return true;
#elif defined(CLASHFLUX_IOS)
        streams_.stop();
        if (!clashflux_ios_stop_tunnel()) {
            std::lock_guard lock(mutex_);
            snap_.state = core::CoreState::Failed;
            snap_.lastError = "iOS Packet Tunnel 未确认退出";
            return false;
        }
        process_.stop();
        std::error_code ec;
        std::filesystem::remove(cfg::coreWorkDir() / "core.pid", ec);
        std::lock_guard lock(mutex_);
        snap_.state = core::CoreState::Stopped;
        snap_.runningSince = {};
        snap_.version.clear();
        clashflux::runtime_plan::Stopped(snap_);
        return true;
#else
        // 先摘系统代理：内核停掉后系统仍指向旧端口会断网。仅当当前代理
        // 确实指向本应用写入的端口时才撤销；其他代理软件接管中不动。
        std::string cleanupError;
        if (systemProxyOurs()) {
            std::string err;
            if (!sysproxy::disable(err) || systemProxyOurs()) {
                cleanupError = "系统代理撤销失败：" +
                    (err.empty() ? std::string{"系统设置仍指向本应用"} : err);
            }
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
                snap_.lastError = "内核服务停止内核失败：" + err;
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
            const long pid = readPidFile();
            if (pid > 0) {
                // pidfile 可能陈旧且该 pid 已被系统复用：先确认它仍然是 sing-box
                // 再发信号，绝不误杀无关进程（无法判定的平台返回空串＝保持旧行为）。
                const std::string image = core::pidImageName(pid);
                if (image.empty() || image.find("sing-box") != std::string::npos) {
                    core::killPid(pid);
                    const auto stillOurProcess = [pid] {
                        if (!core::pidAlive(pid)) return false;
                        const std::string current = core::pidImageName(pid);
                        return current.empty() || current.find("sing-box") != std::string::npos;
                    };
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
                    while (stillOurProcess() && std::chrono::steady_clock::now() < deadline) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                    if (stillOurProcess()) {
                        std::lock_guard lock(mutex_);
                        snap_.state = core::CoreState::Failed;
                        snap_.lastError = "驻留内核停止超时，进程仍在运行";
                        return false;
                    }
                } else {
                    stream::logApplication(
                        "warning",
                        "忽略陈旧的 core.pid（PID " + std::to_string(pid) +
                            " 已是 " + image + "，不是 sing-box），未发送终止信号");
                }
            }
        }
        // 等监视线程收尾（最多 ~2s 宽限 + waitpid）。
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (process_.running() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (process_.running()) {
            std::lock_guard lock(mutex_);
            snap_.state = core::CoreState::Failed;
            snap_.lastError = "内核停止超时，进程仍在运行";
            return false;
        }
        std::error_code ec;
        std::filesystem::remove(cfg::coreWorkDir() / "core.pid", ec);
#if defined(__linux__) && !defined(__ANDROID__)
        std::string routeCleanupError;
        vpn::compensation::CleanupManagedLinuxRoutes(routeCleanupError);
        if (!routeCleanupError.empty()) {
            if (!cleanupError.empty()) cleanupError += "；";
            cleanupError += "主内核已停止，但系统路由清理失败：" + routeCleanupError;
        }
#endif
        {
            std::lock_guard lock(mutex_);
            snap_.state = core::CoreState::Stopped;
            snap_.runningSince = {};
            snap_.version.clear();
            snap_.lastError = cleanupError;
            clashflux::runtime_plan::Stopped(snap_);
        }
        captureHealthEvidence(false);
        return cleanupError.empty();
#endif
    }

    // 订阅/配置变更时重启当前内核。Android 的 libbox 由 VpnService 托管，
    // 但无 TUN 内核也要保持常驻；写完新配置后按当前 TUN 状态恢复对应服务。
    auto lockPlanLifecycle() { return std::unique_lock(lifecycleMutex_); }

    // 候选检查不改变订阅数据库、运行态或组选项；HTTP 自有缓存只在转换与
    // 候选检查成功后提交。适用于 UI/CLI 的写入口。
    bool validateRoutingPlan(const std::string& profileYaml, std::string& error,
                             const vpn::VpnPolicy* policy = nullptr,
                             const std::optional<std::string>& main = std::nullopt) {
        std::lock_guard lock(lifecycleMutex_);
        ensureOpen();
        try {
            auto options = routingOptions(profileYaml, snapshot().tunEnabled, false, policy, main);
            singbox::CompileResult compiled;
            if (!prepareHttpRuleProviders(options, compiled, error, policy) ||
                !validatePlanTargets(options, compiled, policy, error)) return false;
            return checkCompiledPlan(compiled, error);
        } catch (const std::exception& e) { error = e.what(); return false; }
    }

    singbox::RuleSetLibrary ruleSetLibrary() {
        std::lock_guard lock(lifecycleMutex_);
        ensureOpen();
        return routing::DecodeRuleSetLibrary(clashflux::persistence::persistence().setting("rules.resources"));
    }

    // Shared library edits serialize with profile edits/refreshes. Validate the
    // full proposed library before publishing settings or touching a running core.
    bool saveRuleSetLibrary(singbox::RuleSetLibrary library, std::string& error) {
        std::lock_guard lock(lifecycleMutex_);
        error.clear();
        try {
            ensureOpen();
            if (!clashflux::persistence::persistence().ready()) { error = "订阅尚未加载完成"; return false; }
            const auto previous = ruleSetLibrary();
            if (library.revision != previous.revision) { error = "规则集资源库已变化，请重新打开后编辑"; return false; }
            if (!singbox::ValidateRuleSetLibrary(library, error)) return false;
            const auto policy = routing::DecodePolicy(setting("vpn.global_policy", ""));
            if (!singbox::ValidateGlobalRuleSetReferences(library, policy.rules, error)) return false;
            // Even disabled personal records retain their references. Removal
            // cannot turn a later re-enable into a different or missing match.
            for (const auto& profile : db_->listProfiles()) {
                const auto edits = routing::DecodeProfileRules(setting(routing::ProfileRulesKey(profile.id), ""));
                if (!singbox::ValidateRuleSetReferences(library, edits, error)) { error += "：" + profile.name; return false; }
            }
            if (library.revision == std::numeric_limits<std::uint64_t>::max()) { error = "规则集修订号超出范围"; return false; }
            const auto content = currentMainContent();
            auto options = routingOptions(content, false);
            options.ruleSetLibrary = library;
            options.validateAllPersonalRuleSets = true;
            singbox::CompileResult compiled;
            if (!prepareHttpRuleProviders(options, compiled, error)) return false;
            ++library.revision;
            auto& data = clashflux::persistence::persistence();
            const auto oldText = data.setting("rules.resources");
            data.setSetting("rules.resources", routing::EncodeRuleSetLibrary(library));
            bool ok = true;
            try {
                if (snapshot().state == core::CoreState::Running) ok = restartCore(content, true);
            } catch (const std::exception& exception) { error = exception.what(); ok = false; }
            if (!ok) {
                data.setSetting("rules.resources", oldText);
                if (error.empty()) error = snapshot().lastError;
                if (error.empty()) error = "规则集未能应用";
                return false;
            }
            invalidateProxyGroupsSnapshot();
            return true;
        } catch (const std::exception& exception) { error = exception.what(); return false; }
    }

    // The pinned native control API has no resource-update RPC. Rotate only
    // this remote cache identity and use the existing checked restart/rollback.
    // A stopped/unreferenced resource is queued, never reported as downloaded.
    bool refreshPersonalRuleSet(const std::string& id, std::uint64_t expectedRevision,
                                bool& downloaded, std::string& error) {
        std::lock_guard lock(lifecycleMutex_);
        downloaded = false;
        error.clear();
        if (!core::SupportsRuleSetTools()) { error = "此平台暂不支持手动重新下载规则集"; return false; }
        try {
            auto library = ruleSetLibrary();
            if (library.revision != expectedRevision) { error = "规则集资源库已变化，请重新打开后编辑"; return false; }
            const auto resource = std::ranges::find(library.resources, id, &singbox::PersonalRuleSet::id);
            if (resource == library.resources.end() || resource->type != "remote") { error = "请选择远程规则集"; return false; }
            if (resource->refresh_generation == std::numeric_limits<std::uint64_t>::max()) { error = "规则集更新次数超出范围"; return false; }
            const auto options = routingOptions(currentMainContent(), tunEnabled());
            const bool referenced = std::ranges::any_of(options.globalRules, [&](const auto& rule) {
                return rule.enabled && rule.match == vpn::MatchKind::RuleSet && rule.pattern == id;
            }) || std::ranges::any_of(options.profileRuleEdits.rules, [&](const auto& rule) {
                return rule.enabled && rule.match == "rule_set" && rule.pattern == id &&
                    rule.operation != "disable" && rule.operation != "final";
            });
            const bool running = snapshot().state == core::CoreState::Running;
            ++resource->refresh_generation;
            if (!saveRuleSetLibrary(std::move(library), error)) return false;
            downloaded = running && referenced;
            return true;
        } catch (const std::exception& exception) { error = exception.what(); return false; }
    }

    singbox::CompileResult profileRulesPreview(std::int64_t id, const std::string& content) {
        std::lock_guard lock(lifecycleMutex_);
        ensureOpen();
        auto options = routingOptions(content, false, false, nullptr,
            routing::ConnectionId(id), nullptr, false);
        auto result = singbox::compileConfig(options);
        if (result.error.empty() && !result.json.empty()) {
            auto document = nlohmann::json::parse(result.json);
            // Editor previews do not need the application's control credentials.
            if (document.contains("experimental") && document["experimental"].contains("clash_api"))
                document["experimental"]["clash_api"].erase("secret");
            if (document.contains("services") && document["services"].is_array())
                for (auto& item : document["services"]) if (item.value("tag", "") == "clash-flux-control") item.erase("secret");
            result.json = document.dump(2);
        }
        return result;
    }

    bool validateProfileRuleSource(std::int64_t id, const std::string& content, std::string& error) {
        std::lock_guard lock(lifecycleMutex_);
        ensureOpen();
        auto options = routingOptions(content, false, false, nullptr,
            routing::ConnectionId(id), nullptr, false);
        singbox::CompileResult compiled;
        return prepareHttpRuleProviders(options, compiled, error) && checkCompiledPlan(compiled, error);
    }

    std::string currentMainContent() {
        ensureOpen();
        const auto file = selectedProfileFile();
        if (file.empty()) return {};
        std::ifstream input(file, std::ios::binary);
        if (!input) throw std::runtime_error("无法读取 main 来源内容");
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    bool sourceParticipates(std::int64_t id) {
        std::lock_guard operationLock(lifecycleMutex_);
        ensureOpen();
        const auto connection = routing::ConnectionId(id);
        if (std::ranges::any_of(nativeSessions_, [&](const auto& session) { return session.id == connection && session.connected; })) return true;
        for (const auto& profile : db_->listProfiles())
            if (profile.id == id && (profile.selected ||
                ((profile.type == "pptp") && !routing::SplitRoutes(profile.nativeRoutes).empty()))) return true;
        const auto policy = routing::DecodePolicy(setting("vpn.global_policy", ""));
        return std::ranges::any_of(policy.rules, [&](const auto& rule) {
            return rule.enabled && rule.connectionId == connection;
        });
    }

    bool validateSourceUpdate(std::int64_t id, const std::string& content, std::string& error) {
        std::lock_guard lock(lifecycleMutex_);
        ensureOpen();
        try {
            const auto edits = routing::DecodeProfileRules(setting(routing::ProfileRulesKey(id), ""));
            if (!edits.rules.empty()) {
                const auto plan = singbox::InspectProfileRules(content, edits);
                if (!plan.error.empty()) { error = plan.error; return false; }
            }
            if (content.find_first_not_of(" \t\r\n") == std::string::npos) {
                error = "订阅内容为空"; return false;
            }
            const auto sourceId = routing::ConnectionId(id);
            const singbox::ProfileSource updated{sourceId, "", content};
            bool main = false;
            for (const auto& profile : db_->listProfiles()) if (profile.id == id && profile.selected) main = true;
            auto options = routingOptions(main ? content : currentMainContent(), snapshot().tunEnabled,
                                          false, nullptr, std::nullopt, &updated);
            if (sourceId == options.mainConnectionId) options.profileYaml = content;
            else {
                bool replaced = false;
                for (auto& source : options.auxiliarySources)
                    if (source.id == sourceId) { source.content = content; replaced = true; }
                if (!replaced) {
                    options.profileYaml = content; options.mainConnectionId = sourceId;
                    options.profileRuleEdits = edits;
                    options.auxiliarySources.clear(); options.globalRules.clear(); options.nativeConnections.clear();
                }
            }
            singbox::CompileResult compiled;
            return prepareHttpRuleProviders(options, compiled, error) && checkCompiledPlan(compiled, error);
        } catch (const std::exception& e) { error = e.what(); return false; }
    }

    bool restartCore(const std::string& profileYaml, bool skipUnchanged = false) {
        std::lock_guard operationLock(lifecycleMutex_);
        auto healthPause = streams_.pauseHealth();
        const auto before = snapshot();
        const bool wasActive = before.state == core::CoreState::Running || before.state == core::CoreState::Starting;
        const bool systemProxyEnabled = systemProxyOurs();
        const auto preparationBegan = std::chrono::steady_clock::now();
        std::unique_ptr<PreparedStart> candidate;
        std::string error;
        try {
            candidate = prepareStartPlan(routingOptions(profileYaml, before.tunEnabled), error);
            if (!candidate) {
                std::lock_guard lock(mutex_); snap_.lastError = "候选计划被拒绝：" + error; return false;
            }
        } catch (const std::exception& e) {
            std::lock_guard lock(mutex_); snap_.lastError = e.what(); return false;
        }
        if (skipUnchanged && wasActive && candidate->compiled.json == lastAppliedJson_) return true;
        const auto previous = lastAppliedOptions_;
        stream::logApplication("info", std::format("主计划准备完成，用时 {} ms（原内核保持运行）",
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - preparationBegan).count()));
        const auto handoffBegan = std::chrono::steady_clock::now();
        if (!stopCore()) return false;
        preparedStart_ = std::move(candidate);
        startCore(profileYaml, false, before.tunEnabled,
                  systemProxyEnabled ? std::optional<bool>(true) : std::nullopt);
#if defined(__ANDROID__)
        if (wasActive && snapshot().state != core::CoreState::Failed) {
            if (before.tunEnabled) clashflux_android_start_vpn(); else clashflux_android_start_core();
        }
        return snapshot().state != core::CoreState::Failed;
#else
        if (snapshot().state == core::CoreState::Running) {
            stream::logApplication("info", std::format("主计划交接完成，用时 {} ms（控制器就绪）",
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - handoffBegan).count()));
            return true;
        }
        const auto failure = snapshot().lastError;
        // 只有上一份计划仍拥有同一原生接口时才允许恢复，不能复活已释放的接口。
        const bool safe = previous && std::ranges::all_of(previous->nativeConnections, [&](const auto& old) {
            if (!old.connected) return true;
            return std::ranges::any_of(nativeSessions_, [&](const auto& current) {
                return current.id == old.id && current.connected && current.interfaceName == old.interfaceName &&
                       current.nativeConfig == old.nativeConfig && current.transportAddress == old.transportAddress;
            });
        });
        if (wasActive && safe && stopCore()) {
            preparedOptions_ = *previous;
            startCore(previous->profileYaml, false, previous->tunInbound,
                      systemProxyEnabled ? std::optional<bool>(true) : std::nullopt);
        }
        const bool restored = snapshot().state == core::CoreState::Running;
        std::lock_guard lock(mutex_);
        snap_.lastError = failure + (restored ? "；已恢复上一份运行计划" : "；未能恢复上一份运行计划");
        return false;
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
        // TUN belongs to the accepted running plan and platform lifecycle,
        // rather than the incomplete Clash compatibility config response.
#endif
    }

    // 切换出站模式（阻塞）。模式是「配置」而不是内核运行前提：内核没起来时
    // 只写库（下次 startCore 用新配置生成 clash_mode 前置规则），内核真的在
    // 跑时才热切换；只有热切换被内核拒绝才算失败。
    bool applyMode(const std::string& m) {
        std::lock_guard operationLock(lifecycleMutex_);
        auto healthPause = streams_.pauseHealth();
        ensureOpen();
        bool live = false;
#if defined(__ANDROID__)
        // VpnService 数据面存在才算「真的在跑」；此时才要求 JNI 同步成功，
        // 否则仅落配置，等下次生成配置时生效。
        live = clashflux_android_vpn_state() == 2;
        if (live && !clashflux_android_set_clash_mode(m.c_str())) return false;
#else
        {
            std::lock_guard lock(mutex_);
            live = snap_.state == core::CoreState::Running;
        }
        if (live) {
            const nlohmann::json body = {{"mode", m}};
            const auto r = api_->patchConfigs(body.dump());
            if (!r.ok) return false;
        }
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
        bool managed, adopted;
        std::uint64_t epoch;
        {
            std::unique_lock owner(lifecycleMutex_, std::try_to_lock);
            if (!owner.owns_lock()) return; // an intentional transition owns liveness
            std::lock_guard lock(mutex_);
            if (snap_.state != core::CoreState::Running) return;
            managed = managedByService_;
            adopted = adopted_;
            epoch = snap_.runtimeEpoch;
        }
        // No model or lifecycle lock during controller/service I/O.
        std::optional<bool> alive;
        if (managed) {
            alive = service::coreRunningEvidence();
        } else if (adopted) {
            const long pid = readPidFile();
            if (pid > 0) alive = core::pidAlive(pid);
        } else {
            std::unique_lock owner(lifecycleMutex_, std::try_to_lock);
            if (!owner.owns_lock()) return;
            { std::lock_guard lock(mutex_); if (snap_.runtimeEpoch != epoch) return; }
            alive = process_.running();
        }
        if (!alive || *alive) return;
        std::unique_lock owner(lifecycleMutex_, std::try_to_lock);
        if (!owner.owns_lock()) return;
        std::lock_guard lock(mutex_);
        if (snap_.state != core::CoreState::Running || snap_.runtimeEpoch != epoch) return;
        snap_.state = core::CoreState::Failed;
        snap_.lastError = std::format("sing-box 内核异常退出（exit {}）", managed || adopted ? -1 : process_.exitCode());
        if (!managed && !adopted) {
            if (const auto tail = process_.recentTail(); !tail.empty()) snap_.lastError += "：" + tail;
        }
#endif
    }
