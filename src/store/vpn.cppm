// vpn.cppm — clashflux.store.vpn：订阅化的原生 VPN 连接管理。
//
// Profile.type 决定 nativeConfig/nativeRoutes 的解释方式。OpenVPN 使用
// sing-box endpoint，PPTP 保留系统原生路径；Profile 通过同一个 VpnManager
// 管理，活动连接上限由共享领域层按平台能力统一限制。
export module clashflux.store.vpn;

import std;
import clashflux.db;
import clashflux.config;
import clashflux.core;
import clashflux.openvpn;
import clashflux.pptp;
import clashflux.service;
import clashflux.vpn;
import clashflux.routing;
import clashflux.singbox;
import clashflux.store.core;

namespace store {

// 连接 id 是跨引擎、跨平台的稳定引用。规则页只保存这个 id，不保存某个
// 引擎的临时接口名；这样同一条全局规则可以在 PPTP/sing-box 等引擎之间复用。
export inline std::string ProfileConnectionId(std::int64_t profileId) {
    return routing::ConnectionId(profileId);
}

namespace detail {

inline std::string EncodePolicy(const vpn::VpnPolicy& policy) {
    return routing::EncodePolicy(policy);
}

inline vpn::VpnPolicy DecodePolicy(const std::string& text) {
    return routing::DecodePolicy(text);
}

} // namespace detail

export struct PptpState {
    std::int64_t profileId = 0;
    std::string name;
    vpn::ConnectionState state = vpn::ConnectionState::Idle;
    bool toolsAvailable = false;
    std::string interfaceName;
    std::string gateway;
    std::string error;

    bool operator==(const PptpState&) const = default;
};

export struct OpenVpnState {
    std::int64_t profileId = 0;
    std::string name;
    vpn::ConnectionState state = vpn::ConnectionState::Idle;
    bool toolsAvailable = false;
    std::string interfaceName;
    std::string gateway;
    std::string error;

    bool operator==(const OpenVpnState&) const = default;
};

// Capability and session states are published together, including before any
// connection has been dialed. Rendering never probes or constructs VpnStore.
export struct VpnSnapshot {
    bool pptpAvailable = false;
    bool openVpnAvailable = true;
    std::vector<PptpState> pptp;
    std::vector<OpenVpnState> openvpn;
    bool operator==(const VpnSnapshot&) const = default;
};

export class VpnStore {
public:
    VpnStore()
        : adapter_(pptp::MakePptpAdapter()),
          openVpnAdapter_(openvpn::MakeOpenVpnAdapter()) {
        manager_.setAdapters({adapter_, openVpnAdapter_});
        monitor_ = std::jthread([this](std::stop_token stop) {
            std::unique_lock lock(monitorMutex_);
            while (!stop.stop_requested()) {
                monitorWake_.wait_for(lock, stop, std::chrono::seconds(1),
                                      [] { return false; });
                if (stop.stop_requested()) break;
                lock.unlock();
                try { refreshSessions(); } catch (...) {}
                lock.lock();
            }
        });
    }

    ~VpnStore() { shutdown(); }

    VpnStore(const VpnStore&) = delete;
    VpnStore& operator=(const VpnStore&) = delete;

    void init() { ensureLoaded(); }

    // 全局规则是持久化的编排策略：默认主连接负责未命中的流量，rules
    // 把特定域名/IP/网段交给某个订阅连接。返回副本，UI 不会触碰 manager
    // 的内部容器，也不会把 settings 表访问放到渲染线程。
    vpn::VpnPolicy globalPolicy() {
        ensureLoaded();
        std::lock_guard lock(operationMutex_);
        return manager_.policy();
    }

    bool saveGlobalPolicy(vpn::VpnPolicy policy, std::string& error) {
        ensureLoaded();
        std::lock_guard lock(operationMutex_);
        auto planLock = coreStore().lockPlanLifecycle();
        const auto previous = manager_.policy();
        policy.revision = previous.revision;
        if (previous == policy) { error.clear(); return true; }
        if (!routing::ValidatePlatformPolicy(policy, coreStore().db().listProfiles(), error)) return false;
        if (previous.revision == std::numeric_limits<std::uint64_t>::max()) { error = "计划修订号超出范围"; return false; }
        policy.revision = previous.revision + 1;
        for (std::size_t i = 0; i < policy.rules.size(); ++i) {
            auto& rule = policy.rules[i];
            if (rule.id.empty()) rule.id = "route-" + cfg::randomSecret();
            rule.order = static_cast<std::int64_t>(i);
        }
        // 内核运行时，publishRouting → restartCore 会用同一份候选（相同 TUN
        // 状态 + 新落库策略）在停止旧实例前做「编译 + 内核 check」，拒绝时走
        // 下方回滚；这里再预编译一次纯属重复，且是保存全局规则卡顿的主因。
        // 内核未运行时没有候选门禁，这里做唯一一次「编译 + check」验证。
        if (coreStore().snapshot().state != core::CoreState::Running &&
            !coreStore().validateRoutingPlan(coreStore().currentMainContent(),
                                             error, &policy)) {
            return false;
        }
        try {
            if (!manager_.applyPolicy(policy, error)) {
                // A failed rollback means the native route state is unknown.
                if (error.find("回滚失败") != std::string::npos) coreStore().stopCore();
                return false;
            }
            coreStore().db().setSetting("vpn.global_policy",
                                       detail::EncodePolicy(policy));
            coreStore().invalidateProxyGroupsSnapshot();
            if (!publishRouting(error)) {
                const auto failure = error;
                coreStore().db().setSetting("vpn.global_policy",
                                           detail::EncodePolicy(previous));
                std::string rollbackError;
                const bool restored = manager_.applyPolicy(previous, rollbackError);
                if (!restored) {
                    error = failure + "；恢复原生路由失败：" + rollbackError;
                    coreStore().stopCore();
                    return false;
                }
                publishRouting(rollbackError);
                error = failure;
                return false;
            }
            error.clear();
            return true;
        } catch (const std::exception& exception) {
            error = exception.what();
            std::string ignored;
            if (!manager_.applyPolicy(previous, ignored)) coreStore().stopCore();
            try {
                coreStore().db().setSetting("vpn.global_policy",
                                           detail::EncodePolicy(previous));
                if (!publishRouting(ignored)) coreStore().stopCore();
            } catch (...) { coreStore().stopCore(); }
            return false;
        }
    }

    // 阻塞的系统 VPN 清理由调用方放到任务线程；连接操作彼此串行，但不
    // 持有快照锁，因此 UI 轮询不会等待 pppd/RAS 收尾。
    void shutdown() noexcept {
        monitor_.request_stop();
        monitorWake_.notify_all();
        if (monitor_.joinable()) monitor_.join();
        std::lock_guard operationLock(operationMutex_);
        // Stop capture before releasing native interfaces and their leases.
        // Clearing the overlay afterwards must not restart TUN during exit.
        try {
            coreStore().stopCore();
            std::string ignored;
            coreStore().updateNativeRouting({}, ignored);
        } catch (...) {
        }
        manager_.disconnectAll();
        std::lock_guard snapshotLock(mutex_);
        snapshots_.clear();
        openVpnSnapshots_.clear();
    }

    PptpState state(std::int64_t profileId) {
        std::lock_guard lock(mutex_);
        return stateLocked(profileId);
    }

    std::vector<PptpState> states() {
        std::lock_guard lock(mutex_);
        std::vector<PptpState> result;
        result.reserve(snapshots_.size());
        for (const auto& [profileId, snapshot] : snapshots_) {
            (void)profileId;
            result.push_back(snapshot);
        }
        return result;
    }

    VpnSnapshot snapshot() {
        std::lock_guard lock(mutex_);
        VpnSnapshot result{.pptpAvailable = adapter_.descriptor.available,
                           .openVpnAvailable = openVpnAdapter_.descriptor.available};
        result.pptp.reserve(snapshots_.size());
        for (const auto& [id, state] : snapshots_) result.pptp.push_back(state);
        result.openvpn.reserve(openVpnSnapshots_.size());
        for (const auto& [id, state] : openVpnSnapshots_) result.openvpn.push_back(state);
        return result;
    }

    OpenVpnState openVpnState(std::int64_t profileId) {
        std::lock_guard lock(mutex_);
        return openVpnStateLocked(profileId);
    }

    std::vector<OpenVpnState> openVpnStates() {
        std::lock_guard lock(mutex_);
        std::vector<OpenVpnState> result;
        result.reserve(openVpnSnapshots_.size());
        for (const auto& [profileId, snapshot] : openVpnSnapshots_) {
            (void)profileId;
            result.push_back(snapshot);
        }
        return result;
    }

    // 连接一个 PPTP 订阅。阻塞，UI 必须经 RunOnTaskThread 调用。
    bool connectPptp(const db::Profile& profile, std::string& error) {
        ensureLoaded();
        if (profile.type != "pptp") {
            error = "该订阅不是 PPTP 类型";
            return false;
        }
        std::string parseError;
        if (!pptp::ParsePptpConfig(profile.nativeConfig, parseError)) {
            error = parseError;
            setError(profile.id, error);
            return false;
        }

        vpn::VpnConnection connection{
            .id = connectionId(profile.id),
            .name = profile.name,
            .kind = vpn::ConnectionKind::Pptp,
            .canBeMain = false,
            .enginePreference = {vpn::EngineKind::SystemPptp},
            .internalRoutes = splitRoutes(profile.nativeRoutes),
            .nativeConfig = profile.nativeConfig,
        };

        const std::string id = connectionId(profile.id);
        std::unique_lock operationLock(operationMutex_);
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
        // An installed service may be stopped or may have changed since the
        // last poll. Check/start it before engine selection or stopping TUN.
        if (!service::ensureCompatible(error)) {
            refreshPptpAvailability();
            setError(profile.id, error);
            return false;
        }
#endif
        if (!refreshPptpAvailability()) {
            error = "网络服务侧 PPTP 后端不可用";
            setError(profile.id, error);
            return false;
        }
        if (vpn::PptpSingleSession()) {
            // Windows RAS 只有一条 PPTP 会话槽位。在停止主内核之前先拒绝
            // 第二条拨号，不为注定失败的拨号扰动正在运行的 TUN/内核。
            for (const auto& other : manager_.connections()) {
                if (other.kind == vpn::ConnectionKind::Pptp && other.id != id &&
                    (other.state == vpn::ConnectionState::Connected ||
                     other.state == vpn::ConnectionState::Connecting)) {
                    error = std::format(
                        "Windows 系统拨号同一时间只支持一条 PPTP；请先断开「{}」",
                        other.name);
                    setError(profile.id, error);
                    return false;
                }
            }
        }
#ifdef _WIN32
        auto mainOperation = coreStore().lockNativeLifecycle();
        const bool mainWasRunning = coreStore().snapshot().state == core::CoreState::Running;
        if (mainWasRunning && !coreStore().stopCore()) {
            error = coreStore().snapshot().lastError;
            setError(profile.id, error);
            return false;
        }
#endif
        blockBeforeDisconnect(id);
        manager_.disconnect(id);
        {
            std::lock_guard snapshotLock(mutex_);
            manager_.upsertConnection(std::move(connection));
            PptpState& snapshot = snapshots_[profile.id];
            snapshot.profileId = profile.id;
            snapshot.name = profile.name;
            snapshot.state = vpn::ConnectionState::Connecting;
            snapshot.toolsAvailable = adapter_.descriptor.available;
            snapshot.interfaceName.clear();
            snapshot.gateway.clear();
            snapshot.error.clear();
        }

        bool connected = manager_.connect(id, error);
        bool routingApplied = connected && publishRouting(error);
#ifdef _WIN32
        if (routingApplied && mainWasRunning) routingApplied = coreStore().resumeNativeRouting(error);
#endif
        if (connected && !routingApplied) {
            // Compilation can fail before refreshRouting stops the old core.
            // Never release an interface still referenced by that core.
            coreStore().stopCore();
            manager_.disconnect(id);
            const auto failure = error;
            std::string ignored;
            if (!publishRouting(ignored)) coreStore().stopCore();
            error = "PPTP 已撤销，主 VPN 路由更新失败：" + failure;
            connected = false;
        }
#ifdef _WIN32
        if (!connected && mainWasRunning && coreStore().snapshot().state != core::CoreState::Running) {
            std::string restoreError;
            if (!coreStore().resumeNativeRouting(restoreError)) error += "；恢复主内核失败：" + restoreError;
        }
#endif
        {
            std::lock_guard snapshotLock(mutex_);
            updateSnapshotLocked(profile.id, connected ? std::string_view{} : error);
        }
        return connected;
    }

    void disconnectPptp(std::int64_t profileId) {
        ensureLoaded();
        std::lock_guard operationLock(operationMutex_);
        blockBeforeDisconnect(connectionId(profileId));
        manager_.disconnect(connectionId(profileId));
        std::lock_guard snapshotLock(mutex_);
        if (snapshots_.contains(profileId)) {
            updateSnapshotLocked(profileId, {});
        }
    }

    void forgetPptp(std::int64_t profileId) {
        ensureLoaded();
        std::lock_guard operationLock(operationMutex_);
        blockBeforeDisconnect(connectionId(profileId));
        manager_.removeConnection(connectionId(profileId));
        std::lock_guard snapshotLock(mutex_);
        snapshots_.erase(profileId);
    }

    // 连接一个 OpenVPN 原生订阅。阻塞，UI 必须经 RunOnTaskThread 调用。
    bool connectOpenVpn(const db::Profile& profile, std::string& error) {
        ensureLoaded();
        if (profile.type != "openvpn") {
            error = "该订阅不是 OpenVPN 类型";
            return false;
        }
        std::string parseError;
        if (!openvpn::ParseOpenVpnConfig(profile.nativeConfig, parseError)) {
            error = parseError;
            setOpenVpnError(profile.id, error);
            return false;
        }

        vpn::VpnConnection connection{
            .id = connectionId(profile.id),
            .name = profile.name,
            .kind = vpn::ConnectionKind::OpenVpn,
            .canBeMain = false,
            .enginePreference = {vpn::EngineKind::SingBox},
            .internalRoutes = splitRoutes(profile.nativeRoutes),
            .nativeConfig = profile.nativeConfig,
        };

        const std::string id = connectionId(profile.id);
        std::unique_lock operationLock(operationMutex_);
        blockBeforeDisconnect(id);
        manager_.disconnect(id);
        {
            std::lock_guard snapshotLock(mutex_);
            manager_.upsertConnection(std::move(connection));
            OpenVpnState& snapshot = openVpnSnapshots_[profile.id];
            snapshot.profileId = profile.id;
            snapshot.name = profile.name;
            snapshot.state = vpn::ConnectionState::Connecting;
            snapshot.toolsAvailable = openVpnAdapter_.descriptor.available;
            snapshot.interfaceName.clear();
            snapshot.gateway.clear();
            snapshot.error.clear();
        }

        bool connected = manager_.connect(id, error);
        if (connected && !publishRouting(error)) {
            coreStore().stopCore();
            manager_.disconnect(id);
            const auto failure = error;
            std::string ignored;
            if (!publishRouting(ignored)) coreStore().stopCore();
            error = "OpenVPN 已撤销，主 VPN 路由更新失败：" + failure;
            connected = false;
        }
        {
            std::lock_guard snapshotLock(mutex_);
            updateOpenVpnSnapshotLocked(profile.id,
                                        connected ? std::string_view{} : error);
        }
        return connected;
    }

    void disconnectOpenVpn(std::int64_t profileId) {
        ensureLoaded();
        std::lock_guard operationLock(operationMutex_);
        blockBeforeDisconnect(connectionId(profileId));
        manager_.disconnect(connectionId(profileId));
        std::lock_guard snapshotLock(mutex_);
        if (openVpnSnapshots_.contains(profileId)) {
            updateOpenVpnSnapshotLocked(profileId, {});
        }
    }

    void forgetOpenVpn(std::int64_t profileId) {
        ensureLoaded();
        std::lock_guard operationLock(operationMutex_);
        blockBeforeDisconnect(connectionId(profileId));
        manager_.removeConnection(connectionId(profileId));
        std::lock_guard snapshotLock(mutex_);
        openVpnSnapshots_.erase(profileId);
    }

private:
    // Called only on worker threads while holding operationMutex_. Keep the
    // cached capability and existing snapshots consistent under the UI lock.
    bool refreshPptpAvailability() {
        const bool available = pptp::PptpToolsAvailable();
        manager_.setEngineAvailable(vpn::EngineKind::SystemPptp, available);
        std::lock_guard lock(mutex_);
        adapter_.descriptor.available = available;
        for (auto& [id, snapshot] : snapshots_) snapshot.toolsAvailable = available;
        return available;
    }

    // PPTP can die independently of UI actions. OpenVPN endpoint liveness is
    // owned by the resident sing-box instance and follows the core lifecycle.
    inline void refreshSessions() {
        std::lock_guard operationLock(operationMutex_);
        refreshPptpAvailability();
        for (const auto& connection : manager_.connections()) {
            if (connection.state != vpn::ConnectionState::Connected ||
                connection.kind != vpn::ConnectionKind::Pptp) continue;
            const bool alive = pptp::PptpSessionAlive(connection.id);
            if (alive) continue;
            const std::string id = connection.id;
            blockBeforeDisconnect(id);
            manager_.disconnect(id);
            std::lock_guard snapshotLock(mutex_);
            for (auto& [profileId, snapshot] : snapshots_) {
                if (connectionId(profileId) != id) continue;
                updateSnapshotLocked(profileId, "原生 VPN 已断开或服务不可用");
                snapshot.state = vpn::ConnectionState::Failed;
            }
            for (auto& [profileId, snapshot] : openVpnSnapshots_) {
                if (connectionId(profileId) != id) continue;
                updateOpenVpnSnapshotLocked(profileId, "原生 VPN 已断开或服务不可用");
                snapshot.state = vpn::ConnectionState::Failed;
            }
        }
    }

    inline bool publishRouting(std::string& error, std::string_view unavailable = {}) {
        std::vector<singbox::NativeConnection> sessions;
        for (const auto& connection : manager_.connections()) {
            singbox::NativeConnection session;
            session.id = connection.id;
            session.connected = connection.id != unavailable &&
                connection.state == vpn::ConnectionState::Connected;
            session.interfaceName = session.connected &&
                    connection.kind == vpn::ConnectionKind::Pptp
                ? connection.interfaceName
                : "";
            session.kind = connection.kind;
            session.transportAddress = session.connected ? connection.transportAddress : "";
            session.nativeConfig = session.connected &&
                    connection.kind == vpn::ConnectionKind::OpenVpn
                ? connection.nativeConfig
                : "";
            sessions.push_back(std::move(session));
        }
        return coreStore().updateNativeRouting(std::move(sessions), error);
    }

    inline void blockBeforeDisconnect(std::string_view id) {
        const auto* connection = manager_.findConnection(id);
        if (connection == nullptr || connection->state != vpn::ConnectionState::Connected) return;
        std::string error;
        if (!publishRouting(error, id)) coreStore().stopCore();
    }

    static std::string connectionId(std::int64_t profileId) {
        return ProfileConnectionId(profileId);
    }

    static std::vector<std::string> splitRoutes(const std::string& text) {
        std::vector<std::string> routes;
        std::size_t begin = 0;
        while (begin <= text.size()) {
            const std::size_t end = text.find_first_of(",\n", begin);
            std::string route = text.substr(
                begin, end == std::string::npos ? std::string::npos : end - begin);
            while (!route.empty() && std::isspace(
                                           static_cast<unsigned char>(route.front()))) {
                route.erase(route.begin());
            }
            while (!route.empty() && std::isspace(
                                           static_cast<unsigned char>(route.back()))) {
                route.pop_back();
            }
            if (!route.empty()) routes.push_back(std::move(route));
            if (end == std::string::npos) break;
            begin = end + 1;
        }
        return routes;
    }

    PptpState stateLocked(std::int64_t profileId) const {
        if (const auto it = snapshots_.find(profileId); it != snapshots_.end()) {
            return it->second;
        }
        return PptpState{
            .profileId = profileId,
            .toolsAvailable = adapter_.descriptor.available,
        };
    }

    OpenVpnState openVpnStateLocked(std::int64_t profileId) const {
        if (const auto it = openVpnSnapshots_.find(profileId);
            it != openVpnSnapshots_.end()) {
            return it->second;
        }
        return OpenVpnState{
            .profileId = profileId,
            .toolsAvailable = openVpnAdapter_.descriptor.available,
        };
    }

    void updateSnapshotLocked(std::int64_t profileId, std::string_view error) {
        PptpState& snapshot = snapshots_[profileId];
        snapshot.profileId = profileId;
        snapshot.toolsAvailable = adapter_.descriptor.available;
        if (const vpn::VpnConnection* connection =
                manager_.findConnection(connectionId(profileId));
            connection != nullptr) {
            snapshot.name = connection->name;
            snapshot.state = connection->state;
            snapshot.interfaceName = connection->interfaceName;
            snapshot.gateway = connection->gateway;
        }
        snapshot.error = std::string(error);
    }

    void setError(std::int64_t profileId, const std::string& error) {
        std::lock_guard lock(mutex_);
        PptpState& snapshot = snapshots_[profileId];
        snapshot.profileId = profileId;
        snapshot.toolsAvailable = adapter_.descriptor.available;
        snapshot.state = vpn::ConnectionState::Failed;
        snapshot.error = error;
    }

    void updateOpenVpnSnapshotLocked(std::int64_t profileId,
                                     std::string_view error) {
        OpenVpnState& snapshot = openVpnSnapshots_[profileId];
        snapshot.profileId = profileId;
        snapshot.toolsAvailable = openVpnAdapter_.descriptor.available;
        if (const vpn::VpnConnection* connection =
                manager_.findConnection(connectionId(profileId));
            connection != nullptr) {
            snapshot.name = connection->name;
            snapshot.state = connection->state;
            snapshot.interfaceName = connection->interfaceName;
            snapshot.gateway = connection->gateway;
        }
        snapshot.error = std::string(error);
    }

    void setOpenVpnError(std::int64_t profileId, const std::string& error) {
        std::lock_guard lock(mutex_);
        OpenVpnState& snapshot = openVpnSnapshots_[profileId];
        snapshot.profileId = profileId;
        snapshot.toolsAvailable = openVpnAdapter_.descriptor.available;
        snapshot.state = vpn::ConnectionState::Failed;
        snapshot.error = error;
    }

    void ensureLoaded() {
        std::call_once(loadOnce_, [this] {
            coreStore().init();

            const std::string savedPolicy =
                coreStore().setting("vpn.global_policy", "");
            if (!savedPolicy.empty()) {
                manager_.setPolicy(detail::DecodePolicy(savedPolicy));
            }

            // 兼容旧版“连接页 PPTP”保存的 settings：首次进入订阅页时转成
            // 一个 PPTP 订阅，之后所有入口都只走 Profile。
            const std::string legacyConfig =
                coreStore().setting("vpn.pptp.config", "");
            if (!legacyConfig.empty()) {
                std::string parseError;
                if (!pptp::ParsePptpConfig(legacyConfig, parseError)) return;

                bool alreadyMigrated = false;
                for (const db::Profile& profile : coreStore().db().listProfiles()) {
                    if (profile.type == "pptp") {
                        alreadyMigrated = true;
                        break;
                    }
                }
                if (!alreadyMigrated) {
                    db::Profile profile;
                    profile.name = "PPTP 内网";
                    profile.type = "pptp";
                    profile.nativeConfig = legacyConfig;
                    profile.nativeRoutes =
                        coreStore().setting("vpn.pptp.routes", "");
                    try {
                        coreStore().db().saveProfile(profile);
                        coreStore().setSetting("vpn.pptp.config", "");
                        coreStore().setSetting("vpn.pptp.routes", "");
                    } catch (...) {
                        // 保留 settings，下一次进入时继续尝试迁移。
                    }
                }
            }
        });
    }

    std::once_flag loadOnce_;
    mutable std::mutex mutex_;
    std::mutex operationMutex_;
    vpn::EngineAdapter adapter_;
    vpn::EngineAdapter openVpnAdapter_;
    vpn::VpnManager manager_;
    std::map<std::int64_t, PptpState> snapshots_;
    std::map<std::int64_t, OpenVpnState> openVpnSnapshots_;
    std::mutex monitorMutex_;
    std::condition_variable_any monitorWake_;
    std::jthread monitor_;
};

export VpnStore& vpnStore() {
    static VpnStore store;
    return store;
}

} // namespace store
