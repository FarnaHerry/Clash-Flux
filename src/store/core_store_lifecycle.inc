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
        // 任何未捕获异常都必须让状态离开 Starting：以前 json::parse /
        // filesystem 抛出会越过 fail()，把 snap_.state 永久留在 Starting，而
        // 「启动中拒绝重入」的保护会因此挡掉之后所有启动，直到重启应用。
        try {
            startCoreUnchecked(profileYaml, detached, tunEnabled,
                               systemProxyState, speedTestOnly,
                               repairRuleSetCache);
        } catch (const std::exception& exception) {
            settleStartFailure(std::string{"内核启动异常："} + exception.what());
        } catch (...) {
            settleStartFailure("内核启动未知异常");
        }
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
                            bool repairRuleSetCache = true) {
#if defined(CLASHFLUX_IOS)
        // iOS only has the Packet Tunnel data plane. Never accept a desktop-style
        // proxy-only launch, even if an old preference or caller passes false.
        tunEnabled = true;
#endif
        std::lock_guard operationLock(lifecycleMutex_);
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
            snap_.lastError.clear();
        }
        lastProfileYaml_ = profileYaml;

        const std::filesystem::path workDir = cfg::coreWorkDir();
        const std::filesystem::path configFile = workDir / "config.json";
        std::vector<singbox::RuleSetResource> ruleSetResources;
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
            // 两遍编译：第一遍拿到自有 GEO 资源清单，直连预取 .srs 缓存（失败
            // 不致命）；第二遍命中本地文件即以 local rule_set 生成——内核
            // 首启不再因代理不可用而拉取失败退出，预取过的缓存按周刷新。
            singbox::CompileOptions options;
            try {
                if (preparedOptions_) {
                    options = std::move(*preparedOptions_);
                    preparedOptions_.reset();
                } else options = routingOptions(profileYaml, tunEnabled, speedTestOnly);
            } catch (const std::exception& exception) {
                fail(std::string("读取主 VPN 路由计划失败：") + exception.what());
                return;
            }
            singbox::CompileResult compiled;
            std::string providerError;
            if (!prepareHttpRuleProviders(options, compiled, providerError)) {
                { std::lock_guard lock(mutex_); snap_.warnings = compiled.warnings; snap_.fidelity = compiled.fidelity; }
                fail("规则集/订阅准备失败：" + providerError); return;
            }
            if (compiled.json.empty() || !compiled.error.empty()) {
                {
                    std::lock_guard lock(mutex_);
                    snap_.warnings = compiled.warnings;
                    snap_.fidelity = compiled.fidelity;
                }
                fail("订阅编译失败：" + (compiled.error.empty()
                                             ? std::string{"未知错误"}
                                             : compiled.error));
                return;
            }
            {
                auto config = nlohmann::json::parse(compiled.json, nullptr, false);
                if (!config.is_object()) {
                    fail("订阅编译结果不是合法 JSON 对象");
                    return;
                }
                const std::string proxyGroups = proxyGroupsFromConfig(config, compiled.sourceObjects);
                {
                    std::lock_guard lock(mutex_);
                    compiledProxyGroups_ = proxyGroups;
                }
                compiled.json = config.dump();
            }
            prefetchRuleSets(compiled.ruleSetResources, workDir);
            [[maybe_unused]] const bool geoInputsChanged = !compiled.ruleSetResources.empty() || options.ruleSetDir != workDir.string();
            options.ruleSetDir = workDir.string();
#if defined(__ANDROID__)
            // generateConfig applies Android's strict-route policy.
            compiled = core::generateConfig(options);
#else
            if (geoInputsChanged) compiled = core::generateConfig(options);
#endif
            if (compiled.json.empty() || !compiled.error.empty()) {
                {
                    std::lock_guard lock(mutex_);
                    snap_.warnings = compiled.warnings;
                    snap_.fidelity = compiled.fidelity;
                }
                fail("订阅编译失败：" + (compiled.error.empty()
                                             ? std::string{"未知错误"}
                                             : compiled.error));
                return;
            }
            {
                auto config = nlohmann::json::parse(compiled.json, nullptr, false);
                if (!config.is_object()) {
                    fail("订阅编译结果不是合法 JSON 对象");
                    return;
                }
                const std::string proxyGroups = proxyGroupsFromConfig(config, compiled.sourceObjects);
                {
                    std::lock_guard lock(mutex_);
                    compiledProxyGroups_ = proxyGroups;
                }
                compiled.json = config.dump();
            }
#ifdef _WIN32
            if (options.tunInbound) {
                for (const auto& native : options.nativeConnections) {
                    if (native.connected && !native.interfaceName.empty())
                        windowsNativeInterfaces.push_back(native.interfaceName);
                }
                if (!windowsNativeInterfaces.empty()) {
                    const auto config =
                        nlohmann::json::parse(compiled.json, nullptr, false);
                    for (const auto& inbound : config["inbounds"]) {
                        if (inbound.value("type", "") != "tun") continue;
                        windowsTunInterface = inbound.value("interface_name", "ClashFlux");
                        windowsExclusions = inbound.value("route_exclude_address", std::vector<std::string>{});
                    }
                }
            }
#endif
            std::string checkError;
            if (!checkCompiledPlan(compiled, checkError)) { fail(checkError); return; }
            ruleSetResources = compiled.ruleSetResources;
            const auto revisionText = setting("routing.applied_revision", "0");
            std::uint64_t appliedRevision = 0;
            const auto parsedRevision = std::from_chars(revisionText.data(), revisionText.data() + revisionText.size(), appliedRevision);
            if (parsedRevision.ec != std::errc() || parsedRevision.ptr != revisionText.data() + revisionText.size()) {
                fail("已应用计划修订号格式错误；保留原设置"); return;
            }
            if (appliedRevision == std::numeric_limits<std::uint64_t>::max()) { fail("已应用计划修订号超出范围"); return; }
            options.planRevision = appliedRevision + 1;
            lastPreparedOptions_ = options;
            {
                std::ifstream previousConfig(configFile, std::ios::binary);
                if (previousConfig) {
                    const std::string previousText(std::istreambuf_iterator<char>(previousConfig), {});
                    const auto previousJson = nlohmann::json::parse(previousText, nullptr, false);
                    const auto candidateJson = nlohmann::json::parse(compiled.json, nullptr, false);
                    residentConfigMatches = !previousJson.is_discarded() && previousJson == candidateJson;
                }
            }
            // Android's VpnService waits for this file from another thread.
            // Never expose a partially-written JSON document to libbox; an
            // atomic rename also prevents a stale config from being selected
            // while a new subscription is being compiled.
#if defined(__ANDROID__)
            std::error_code staleConfigError;
            std::filesystem::remove(configFile, staleConfigError);
#endif
            const std::filesystem::path tempConfig = configFile.string() + ".tmp";
            struct TempConfig {
                std::filesystem::path path;
                ~TempConfig() { std::error_code ec; std::filesystem::remove(path, ec); }
            } tempCleanup{tempConfig};
            std::ofstream out(tempConfig, std::ios::binary | std::ios::trunc);
            if (!out) {
                fail("无法写入运行时配置: " + configFile.string());
                return;
            }
            out << compiled.json;
            out.flush();
            out.close();
            if (!out) { fail("运行时配置写入失败"); return; }
            std::string commitError;
            if (!api::CommitFile(tempConfig, configFile, commitError)) { fail(commitError); return; }
            std::lock_guard lock(mutex_);
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
            snap_.tunEnabled = tunEnabled;
            snap_.state = tunEnabled || speedTestOnly
                              ? core::CoreState::Stopped
                              : core::CoreState::Starting;
        }
        if (!speedTestOnly) {
            if (tunEnabled) {
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
                if (repairRuleSetCache && !ruleSetResources.empty() &&
                    isRuleSetCacheFailure(message)) {
                    // 坏缓存会让内核每次启动都 FATAL（且按周刷新的 mtime 判断
                    // 永远认为它新鲜）：只清本次自有 GEO 与默认缓存后重试一次。
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
                                       /*repairRuleSetCache=*/false);
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
            if (!service::prepareWindowsTun(
                    windowsTunInterface, windowsNativeInterfaces, windowsExclusions, error)) {
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
            snap_.planRevision = lastPreparedOptions_ ? lastPreparedOptions_->planRevision : 0;
            lastAppliedOptions_ = lastPreparedOptions_;
            setSetting("routing.applied_revision", std::to_string(snap_.planRevision));
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
        serviceDetached_ = false;
#endif
        stream::logApplication("info", "请求停止代理内核");
#if defined(__ANDROID__)
        clashflux_android_stop_vpn();
        std::lock_guard lock(mutex_);
        snap_.state = core::CoreState::Stopped;
        snap_.version.clear();
        snap_.tunEnabled = false;
        return true;
#elif defined(CLASHFLUX_IOS)
        streams_.stop();
        process_.stop();
        std::error_code ec;
        std::filesystem::remove(cfg::coreWorkDir() / "core.pid", ec);
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
            const long pid = readPidFile();
            if (pid > 0) {
                // pidfile 可能陈旧且该 pid 已被系统复用：先确认它仍然是 sing-box
                // 再发信号，绝不误杀无关进程（无法判定的平台返回空串＝保持旧行为）。
                const std::string image = core::pidImageName(pid);
                if (image.empty() || image.find("sing-box") != std::string::npos) {
                    core::killPid(pid);
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
                ((profile.type == "pptp" || profile.type == "openvpn") && !routing::SplitRoutes(profile.nativeRoutes).empty()))) return true;
        const auto policy = routing::DecodePolicy(setting("vpn.global_policy", ""));
        return std::ranges::any_of(policy.rules, [&](const auto& rule) {
            return rule.enabled && rule.connectionId == connection;
        });
    }

    bool validateSourceUpdate(std::int64_t id, const std::string& content, std::string& error) {
        std::lock_guard lock(lifecycleMutex_);
        ensureOpen();
        try {
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
                    options.auxiliarySources.clear(); options.globalRules.clear(); options.nativeConnections.clear();
                }
            }
            singbox::CompileResult compiled;
            return prepareHttpRuleProviders(options, compiled, error) && checkCompiledPlan(compiled, error);
        } catch (const std::exception& e) { error = e.what(); return false; }
    }

    bool restartCore(const std::string& profileYaml) {
        std::lock_guard operationLock(lifecycleMutex_);
        const auto before = snapshot();
        const bool wasActive = before.state == core::CoreState::Running || before.state == core::CoreState::Starting;
        const bool systemProxyEnabled = systemProxyOurs();
        singbox::CompileOptions candidate;
        std::string error;
        try {
            candidate = routingOptions(profileYaml, before.tunEnabled);
            singbox::CompileResult compiled;
            if (!prepareHttpRuleProviders(candidate, compiled, error) || !checkCompiledPlan(compiled, error)) {
                std::lock_guard lock(mutex_); snap_.lastError = "候选计划被拒绝：" + error; return false;
            }
        } catch (const std::exception& e) {
            std::lock_guard lock(mutex_); snap_.lastError = e.what(); return false;
        }
        const auto previous = lastAppliedOptions_;
        if (!stopCore()) return false;
        preparedOptions_ = candidate;
        startCore(profileYaml, false, before.tunEnabled,
                  systemProxyEnabled ? std::optional<bool>(true) : std::nullopt);
#if defined(__ANDROID__)
        if (wasActive && snapshot().state != core::CoreState::Failed) {
            if (before.tunEnabled) clashflux_android_start_vpn(); else clashflux_android_start_core();
        }
        return snapshot().state != core::CoreState::Failed;
#else
        if (snapshot().state == core::CoreState::Running) return true;
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
#if !defined(__ANDROID__)
        if (j.contains("tun") && j["tun"].is_object()) {
            snap_.tunEnabled = j["tun"].value("enable", false);
        }
#endif
#endif
    }

    // 切换出站模式（阻塞）。模式是「配置」而不是内核运行前提：内核没起来时
    // 只写库（下次 startCore 用新配置生成 clash_mode 前置规则），内核真的在
    // 跑时才热切换；只有热切换被内核拒绝才算失败。
    bool applyMode(const std::string& m) {
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
