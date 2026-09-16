// core_store.cppm — clashflux.store.core：内核编排 store（接口即实现，单例）。
//
// 持有 Db / CoreProcess / ClashApi / CoreStreams 四件套，是 UI 与内核之间的唯一
// 入口。线程契约：snapshot() / settings 读取等轻量方法任意线程可调；标注
// 「阻塞」的方法（startCore/stopCore/refreshRuntime/applyConfig）会做 spawn、
// 网络等待，UI 必须经 RunOnTaskThread 调用；State 写入只在 UI 线程（UI 层在
// 协程恢复点把 snapshot 写进 State）。
//
// 启动流程（startCore）：
//   1. 解析 sing-box 二进制（cfg::singboxBinary）——缺失 → Failed
//   2. 读启用订阅（无订阅 = 空）经 clashflux.singbox 编译为
//      coreWorkDir/config.json（不支持的节点/规则以 warnings 带回）
//   3. spawn `sing-box run -c <config> -D <workdir>`，轮询 /version 等
//      clash_api 就绪（≤30s，覆盖远程规则集首启下载；进程提前退出仍立即判失败）
//   4. 就绪 → Running，拉起 /logs /traffic /connections 三条 WS 流
export module clashflux.store.core;

import std;
import nlohmann.json;
import clashflux.config;
import clashflux.utils;
import clashflux.db;
import clashflux.api;
import clashflux.core;
import clashflux.singbox;
import clashflux.stream;
import clashflux.sysproxy;
import clashflux.service;
import clashflux.routing;
import clashflux.vpn_compensation;

namespace store {

#if defined(__ANDROID__)
// android_bridge: 0=stopped, 1=establishing, 2=attached, 3=failed.
extern "C" int clashflux_android_vpn_state() noexcept;
extern "C" void clashflux_android_start_vpn() noexcept;
extern "C" void clashflux_android_stop_vpn() noexcept;
extern "C" bool clashflux_android_set_clash_mode(const char*) noexcept;
#endif

export struct CoreSnapshot {
    core::CoreState state = core::CoreState::Stopped;
    std::string binaryPath;      // 解析到的内核路径（空 = 未安装）
    std::string version;         // 内核版本（Running 时）
    std::string lastError;
    // 订阅编译降级报告（不支持的节点/规则；启动时更新）
    std::vector<std::string> warnings;
    // 运行配置快照（Running 时有效）
    std::string mode;            // rule / global / direct
    int mixedPort = 7899;
    bool allowLan = false;
    std::string logLevel = "info";
    bool tunEnabled = false;
    // Android sing-box libbox status stream.  Desktop continues to use the
    // sing-box clash_api WebSocket streams below.
    std::int64_t uploadRate = 0;
    std::int64_t downloadRate = 0;
    std::int64_t uploadTotal = 0;
    std::int64_t downloadTotal = 0;
    int connectionCount = 0;

    bool operator==(const CoreSnapshot&) const = default;  // State 变更检测
};

export class CoreStore {
public:
    CoreStore() = default;
    ~CoreStore() noexcept {
        // 正常关闭由 AppRoot 的异步收尾负责；这里保留一个进程退出时的
        // RAII 兜底，避免窗口被外部关闭、组合树提前卸载后 root 服务仍
        // 持有内核。兜底只覆盖本进程直接持有的形态（服务托管/直接
        // spawn）：adopted 不在其列——CLI `core start` 的 detached 驻留
        // 契约就是 CLI 退出后内核继续跑（停止走 `core stop` / pidfile）。
        // 析构阶段不能把异常带出进程；正常 UI 关闭仍由 AppRoot 的任务
        // 线程路径负责，避免阻塞交互线程。
        try {
            const bool serviceOwnsCore =
                managedByService_ ||
                (!process_.running() && service::available() &&
                 service::coreRunning());
            if (serviceOwnsCore || process_.running()) stopCore();
        } catch (...) {
        }
    }
    CoreStore(const CoreStore&) = delete;
    CoreStore& operator=(const CoreStore&) = delete;

    // 惰性初始化（打开 Db、确保 secret 存在、准备 API 端点）。幂等。
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
        // REST controller of its own.  Report the service's authoritative
        // state so the UI never claims that a dead data plane is running.
        switch (clashflux_android_vpn_state()) {
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
        std::lock_guard lock(mutex_);
        return compiledProxyGroups_;
    }

    bool selectProxy(const std::string& group, const std::string& name) {
        setSetting("proxy.selection." + group, name);
#if defined(__ANDROID__)
        if (clashflux_android_vpn_state() == 2)
            return clashflux_android_select_outbound(group.c_str(), name.c_str());
        return true;
#else
        if (snapshot().state != core::CoreState::Running) return true;
        return api_->selectProxy(group, name).ok;
#endif
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
    bool systemProxySupported() { return sysproxy::supported(); }

    // 切换 TUN（阻塞）。sing-box 的 clash_api 不支持热更 tun：内核运行中 →
    // 重新合成 config.json 并重启内核生效，重启失败（如无 root/CAP_NET_ADMIN，
    // sing-box 建 TUN 失败退出）回滚设置并恢复无 TUN 运行；未运行时开启
    // TUN 会尝试使用当前订阅启动内核。成功更新快照。
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
            if (!enable) return true;
            if (ensureRunning()) return true;
            setSetting("core.tun_enabled", "false");
            std::lock_guard lock(mutex_);
            snap_.tunEnabled = false;
            return false;
        }
        if (!stopCore()) {
            setSetting("core.tun_enabled", enable ? "false" : "true");
            return false;
        }
        startCore(lastProfileYaml_);
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
        startCore(lastProfileYaml_);
        return false;
#endif
    }

    // 切换系统代理（阻塞 shell 调用）。成功持久化设置。
    bool applySystemProxy(bool enable) {
        std::lock_guard operationLock(lifecycleMutex_);
#if defined(__ANDROID__)
        (void)enable;
        return false;
#else
        ensureOpen();
        if (enable && !ensureRunning()) return false;
        std::string err;
        const bool ok =
            enable ? sysproxy::enable("127.0.0.1", mixedPort(), err)
                   : sysproxy::disable(err);
        if (!ok) {
            std::lock_guard lock(mutex_);
            snap_.lastError = "系统代理" + std::string(enable ? "开启" : "关闭") +
                              "失败：" + err;
            return false;
        }
        setSetting("proxy.system_enabled", enable ? "true" : "false");
        return true;
#endif
    }

    // Read the selected profile at the time of the action, including after a
    // stopped-core subscription switch. Never silently start an empty profile.
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
            startCore(yaml);
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
        startCore(lastProfileYaml_);
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
            auto options = routingOptions(lastProfileYaml_, true);
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
            startCore(lastProfileYaml_);
#if defined(__ANDROID__)
            if (snapshot().state != core::CoreState::Failed) {
                clashflux_android_start_vpn();
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
    // detached=true（CLI core start）：直连 spawn 走 setsid 脱离会话驻留，
    // 本进程退出不带走内核。
    void startCore(const std::string& profileYaml, bool detached = false,
                   bool useSavedTunSetting = true) {
        std::lock_guard operationLock(lifecycleMutex_);
        ensureOpen();
        stream::logApplication("info", "开始启动代理内核");
        {
            std::lock_guard lock(mutex_);
            // Android 桥线程在应用启动时自动拉起内核，启动慢时（拉远程
            // 规则集等）UI 侧再点启动会 spawn 第二个实例，抢不到 9097/
            // 混合端口的那个以 exit 1 收场，把「内核启动后立即退出」误报
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
                options = routingOptions(profileYaml, useSavedTunSetting);
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
                nlohmann::json groups = nlohmann::json::object();
                for (auto& outbound : config["outbounds"]) {
                    const std::string type = outbound.value("type", "");
                    if (type != "selector" && type != "urltest") continue;
                    const std::string group = outbound.value("tag", "");
                    if (group.empty()) continue;
                    const auto members = outbound.value("outbounds", nlohmann::json::array());
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
                compiledProxyGroups_ = nlohmann::json{{"proxies", groups}}.dump();
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
        //      重复 spawn 撞 9097 与混合端口
        //   3. 直接 spawn（默认）
#if defined(__ANDROID__)
        // Config generation is the only native responsibility on Android.
        // ClashVpnService reads this file and starts libbox after Android has
        // granted VPN consent.  Do not poll the REST controller here.
        {
            std::lock_guard lock(mutex_);
            snap_.mode = mode();
            snap_.mixedPort = mixedPort();
            snap_.allowLan = false;
            snap_.logLevel = logLevel();
            snap_.tunEnabled = false;
            snap_.state = core::CoreState::Stopped;
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
        } else if (api_->version().ok) {
            adopted_ = true;
        } else if (detached) {
            // CLI 驻留形态：setsid 脱离会话 + 日志重定向 + pidfile，CLI 退出
            // 内核仍在。本进程不持句柄，按接管形态管理（活判/停止走 pidfile）。
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
        // 系统代理开关处于开：内核就绪后重指到当前端口（best effort，
        // 失败不判启动失败，记 lastError）。
        if (systemProxyEnabled()) {
            std::string err;
            if (!sysproxy::enable("127.0.0.1", mixedPort(), err)) {
                std::lock_guard lock(mutex_);
                snap_.lastError = "系统代理应用失败：" + err;
            }
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
        // 先摘系统代理：内核停掉后系统仍指向旧端口会断网。
        if (systemProxyEnabled()) {
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
            (!process_.running() && service::available() &&
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

    // 订阅/配置变更时重启当前内核。Android 的 startCore 只负责生成
    // config.json，真正的数据面由 VpnService 持有，因此必须在写完新配置后
    // 再显式请求恢复服务；否则切换订阅会停在 Stopped，代理页自然没有内容。
    void restartCore(const std::string& profileYaml) {
        std::lock_guard operationLock(lifecycleMutex_);
        const core::CoreState before = snapshot().state;
        const bool wasActive = before == core::CoreState::Running ||
                               before == core::CoreState::Starting;
        if (!stopCore()) return;
        startCore(profileYaml);
#if defined(__ANDROID__)
        if (wasActive) clashflux_android_start_vpn();
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
        if (!clashflux_android_set_clash_mode(m.c_str())) return false;
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
        if (clashflux_android_vpn_state() == 3) {
            snap_.state = core::CoreState::Failed;
        } else if (clashflux_android_vpn_state() == 2) {
            snap_.state = core::CoreState::Running;
        } else if (clashflux_android_vpn_state() == 1) {
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

private:
    inline singbox::CompileOptions routingOptions(const std::string& profileYaml,
                                                  bool useSavedTunSetting) {
        singbox::CompileOptions options;
        options.profileYaml = profileYaml;
        options.controller = cfg::controllerAddress();
        options.secret = secret_;
        options.mixedPort = mixedPort();
        options.mode = mode();
        options.allowLan = allowLan();
        options.ipv6 = ipv6Enabled();
        options.logLevel = logLevel();
        options.tunInbound = useSavedTunSetting && tunEnabled();
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
            // root 服务同步处理 PPTP/OpenVPN 建链时，STATUS 可能
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

    std::unique_ptr<db::Db> db_;
    std::unique_ptr<api::ClashApi> api_;
    core::CoreProcess process_;
    stream::CoreStreams streams_;
    std::string secret_;

    std::mutex mutex_;
    std::recursive_mutex lifecycleMutex_;
    std::vector<singbox::NativeConnection> nativeSessions_;
#ifdef _WIN32
    vpn::compensation::RouteLease windowsRouting_;
#endif
    std::once_flag initFlag_;
    CoreSnapshot snap_;
    std::string binaryPath_;
    std::string lastProfileYaml_;    // 最近一次 startCore 的订阅原文（applyTun 重启用）
    std::string compiledProxyGroups_;
    bool managedByService_ = false;  // 内核由 root 服务托管
    bool adopted_ = false;           // 接管的外部内核实例（非本进程 spawn）
};

// 进程级单例（apitab g_requests 同款形态）。
export CoreStore& coreStore() {
    static CoreStore store;
    return store;
}

} // namespace store
