// vpn.cppm — clashflux.vpn：连接编排层（与具体引擎、平台解耦）。
//
// sing-box、PPTP、OpenVPN、WireGuard 等都只是 EngineAdapter 的实现。上层只
// 关心：连接是什么、默认主 VPN 是谁、哪些目标交给哪条 VPN，以及每条 VPN 自己
// 的引擎原生规则。引擎不支持某种连接时，由 SelectEngine 按偏好和能力自动降级。
export module clashflux.vpn;

import std;

namespace vpn {

export enum class EngineKind {
    SingBox,
    SystemPptp,
    SystemWireGuard,
    Unknown,
};

// 平台和 TUN 后端是两个概念：Linux/Windows 先由 sing-box 提供全流量入口，
// Android 后续由 Java VpnService 建立系统 TUN，再把 fd 交给 native 数据面。
export enum class PlatformKind {
    Linux,
    Windows,
    MacOS,
    Android,
    Unknown,
};

export enum class TunBackendKind {
    SingBox,
    AndroidVpnService,
    Unsupported,
};

export enum class TunCaptureMode {
    Disabled,
    FullDevice,
};

export enum class ConnectionKind {
    ProxyConfig,  // 订阅等代理配置（Clash YAML / 原生 sing-box JSON）
    Pptp,
    OpenVpn,
    WireGuard,
};

export enum class ConnectionState {
    Disabled,
    Idle,
    Connecting,
    Connected,
    Failed,
};

export enum class MatchKind {
    Any,
    ExactDomain,
    DomainSuffix,
    ExactIp,
    Ipv4Cidr,
};

export inline std::string_view MatchKindName(MatchKind kind) noexcept {
    switch (kind) {
    case MatchKind::Any: return "全部";
    case MatchKind::ExactDomain: return "精确域名";
    case MatchKind::DomainSuffix: return "域名后缀";
    case MatchKind::ExactIp: return "精确 IP";
    case MatchKind::Ipv4Cidr: return "IPv4 网段";
    }
    return "未知";
}

export inline std::optional<MatchKind> ParseMatchKind(
    std::string_view name) noexcept {
    if (name == "全部" || name == "any") return MatchKind::Any;
    if (name == "精确域名" || name == "exact-domain") {
        return MatchKind::ExactDomain;
    }
    if (name == "域名后缀" || name == "domain-suffix") {
        return MatchKind::DomainSuffix;
    }
    if (name == "精确 IP" || name == "exact-ip") return MatchKind::ExactIp;
    if (name == "IPv4 网段" || name == "ipv4-cidr") {
        return MatchKind::Ipv4Cidr;
    }
    return std::nullopt;
}

// 主 VPN 选择规则。priority 越大越先匹配；同优先级时更具体的匹配类型优先。
export struct RouteRule {
    MatchKind match = MatchKind::Any;
    std::string pattern;
    std::string connectionId;
    int priority = 0;

    bool operator==(const RouteRule&) const = default;
};

// Routing sees the destination host, not an encrypted HTTP path.
export inline std::optional<std::string> NormalizeRuleDomain(std::string_view input) {
    const auto first = input.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    input = input.substr(first, input.find_last_not_of(" \t\r\n") - first + 1);
    const auto scheme = input.find("://");
    if (scheme != std::string_view::npos) {
        std::string protocol(input.substr(0, scheme));
        std::ranges::transform(protocol, protocol.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (protocol != "http" && protocol != "https") return {};
        input.remove_prefix(scheme + 3);
        input = input.substr(0, input.find_first_of("/?#"));
        const auto colon = input.find(':');
        if (colon != std::string_view::npos) {
            unsigned int port = 0;
            const auto value = input.substr(colon + 1);
            const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), port);
            if (ec != std::errc{} || end != value.data() + value.size() ||
                port == 0 || port > 65535) return {};
            input = input.substr(0, colon);
        }
    }
    if (input.starts_with('.')) input.remove_prefix(1);
    if (input.ends_with('.')) input.remove_suffix(1);
    if (input.empty() || input.size() > 253) return {};
    std::string result(input);
    std::ranges::transform(result, result.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::size_t begin = 0;
    while (begin < result.size()) {
        const auto dot = result.find('.', begin);
        const auto label = std::string_view(result).substr(begin,
            dot == std::string::npos ? result.size() - begin : dot - begin);
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-' ||
            !std::ranges::all_of(label, [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
            })) return {};
        if (dot == std::string::npos) return result;
        begin = dot + 1;
    }
    return {};
}

// 某条 VPN 的声明。nativeConfig/nativeRules 保留给引擎自己解释：OpenVPN
// 保存 .ovpn 原文并由 sing-box 编译，PPTP 保存拨号参数，未来其他引擎不需要
// 修改这层数据结构。
export struct VpnConnection {
    std::string id;
    std::string name;
    ConnectionKind kind = ConnectionKind::ProxyConfig;
    bool enabled = true;
    bool canBeMain = true;
    std::vector<EngineKind> enginePreference;
    std::vector<std::string> internalRoutes;  // 连接成功后应进入该隧道的 CIDR
    std::vector<RouteRule> internalRules;     // 引擎/连接自身的规则
    // 原生 VPN 适配器建立连接后回填。Linux 通常是 ppp0/tun0/wg0，Windows
    // 使用 UTF-8 接口别名（sing-box bind_interface 不接受数字索引）。
    std::string interfaceName;
    std::string gateway;
    std::string nativeConfig;                 // 引擎原生配置，不由编排层解析
    ConnectionState state = ConnectionState::Idle;
    std::optional<EngineKind> activeEngine;
    std::string transportAddress;            // 会话实际拨号的服务器 IPv4

    bool operator==(const VpnConnection&) const = default;
};

// Desktop can route several auxiliary VPN profiles at once. Android keeps one
// auxiliary profile active beside the main sing-box TUN; the same manager and
// sing-box compiler are used on both platforms.
export inline std::size_t MaxConcurrentAuxiliaryConnections() noexcept {
#if defined(__ANDROID__)
    return 1;
#else
    return std::numeric_limits<std::size_t>::max();
#endif
}

export struct VpnPolicy {
    // 空值表示没有默认主 VPN；匹配不到规则时保持直连/系统默认路由。
    std::string defaultMainId;
    std::vector<RouteRule> rules;

    bool operator==(const VpnPolicy&) const = default;
};

// 一条交给操作系统路由表的网段。它的目的不是替代 sing-box 规则，而是让
// PPTP/WireGuard 这类系统接口型隧道绕过主 TUN；OpenVPN endpoint 不安装系统路由。
export struct TunNativeRoute {
    std::string destination;
    std::string connectionId;
    std::string interfaceName;
    std::string gateway;

    bool operator==(const TunNativeRoute&) const = default;
};

// 全流量接管入口的声明。Linux/Windows 当前直接使用 sing-box TUN；Android 的
// backend 只描述后续实现方向，ready=false，避免 UI 误以为已经能接管流量。
export struct TunConfig {
    PlatformKind platform = PlatformKind::Unknown;
    TunBackendKind backend = TunBackendKind::Unsupported;
    TunCaptureMode mode = TunCaptureMode::Disabled;
    std::string device;
    std::string stack = "mixed";
    bool autoRoute = true;
    bool strictRoute = true;
    bool autoDetectInterface = true;
    bool ready = false;
    std::vector<std::string> routeAddress;
    std::vector<std::string> routeExcludeAddress;
    std::string note;

    bool operator==(const TunConfig&) const = default;
};

// 这是“一个入口、多条出口”的结果：
//   1. logicalRules 由编排器/sing-box 的规则层消费；
//   2. nativeRoutes 由 Linux/Windows 路由后端安装到原生 VPN 接口；
//   3. routeExcludeAddress 写进 sing-box TUN，避免原生内网又被主 TUN 捕获。
export struct TunRoutePlan {
    TunConfig capture;
    std::string mainConnectionId;
    std::vector<RouteRule> logicalRules;
    std::vector<TunNativeRoute> nativeRoutes;

    bool ready() const noexcept {
        return capture.ready && capture.mode == TunCaptureMode::FullDevice &&
               !mainConnectionId.empty();
    }

    bool requiresNativeRouteBackend() const noexcept {
        return !nativeRoutes.empty();
    }
};

// 由平台注册给编排层的引擎能力。available 是“当前平台/环境可用”，不只是
// 编译进了程序；例如 PPTP 引擎可能存在，但系统没有拨号工具时仍不可用。
export struct EngineDescriptor {
    EngineKind kind = EngineKind::Unknown;
    int priority = 0;
    bool available = false;
    std::vector<ConnectionKind> connectionKinds;
};

export struct EngineSelection {
    std::optional<EngineKind> engine;
    std::string reason;

    bool ok() const noexcept { return engine.has_value(); }
};

// 平台/引擎适配器的最小运行时契约。编排层不直接调用 pppd、RAS 或 sing-box；
// 适配器负责建立连接、安装连接自身和策略层生成的原生路由，并在失败时给出
// 可继续尝试的错误。
export struct EngineAdapter {
    EngineDescriptor descriptor;
    // 适配器可以回填原生隧道建立后的 interfaceName/gateway，供路由计划使用。
    std::function<bool(VpnConnection&, std::string&)> connect;
    std::function<bool(VpnConnection&, std::span<const std::string>,
                       std::string&)>
        applyRoutes;
    std::function<void(VpnConnection&)> disconnect;
};

export inline bool SupportsDesktopSingBoxTun(PlatformKind platform) noexcept {
    return platform == PlatformKind::Linux ||
           platform == PlatformKind::Windows ||
           platform == PlatformKind::MacOS;
}

// 构造全流量 TUN 入口。这里故意只描述入口，不尝试把多条 VPN 都塞进一
// 个 TUN：一个 TUN 可以接管全部流量，但每条原生 VPN 仍必须拥有自己的接口、
// 路由表和生命周期。
export inline TunConfig MakeFullTunConfig(PlatformKind platform,
                                           std::string device = "clash-flux") {
    TunConfig config{
        .platform = platform,
        .device = std::move(device),
        .routeAddress = {"0.0.0.0/1", "128.0.0.0/1", "::/1", "8000::/1"},
    };

    if (SupportsDesktopSingBoxTun(platform)) {
        config.backend = TunBackendKind::SingBox;
        config.mode = TunCaptureMode::FullDevice;
        config.ready = true;
        config.note = "sing-box TUN 作为全流量入口；原生 VPN 网段由系统路由绕过入口";
        return config;
    }

    if (platform == PlatformKind::Android) {
        config.backend = TunBackendKind::AndroidVpnService;
        config.mode = TunCaptureMode::FullDevice;
        config.ready = false;
        config.note =
            "等待 Android VpnService.establish() 提供 TUN fd，当前不能由 sing-box 独立创建系统 VPN";
        return config;
    }

    config.note = "当前平台没有可用的全流量 TUN 后端";
    return config;
}

namespace detail {

inline std::string lower(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const unsigned char c : value) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    while (!out.empty() && out.back() == '.') out.pop_back();
    return out;
}

inline bool parseIpv4(std::string_view text, std::uint32_t& out) {
    std::uint32_t value = 0;
    std::size_t begin = 0;
    for (int part = 0; part < 4; ++part) {
        const std::size_t end = text.find('.', begin);
        const std::size_t length =
            end == std::string_view::npos ? text.size() - begin : end - begin;
        if (length == 0 || length > 3) return false;
        unsigned int octet = 0;
        const auto [ptr, ec] = std::from_chars(text.data() + begin,
                                               text.data() + begin + length,
                                               octet);
        if (ec != std::errc() || ptr != text.data() + begin + length ||
            octet > 255U) {
            return false;
        }
        value = (value << 8U) | octet;
        if (end == std::string_view::npos) {
            if (part != 3) return false;
            out = value;
            return true;
        }
        begin = end + 1;
    }
    return false;
}

inline bool matchIpv4Cidr(std::string_view target, std::string_view cidr) {
    const std::size_t slash = cidr.find('/');
    if (slash == std::string_view::npos) return false;
    std::uint32_t targetAddress = 0;
    std::uint32_t network = 0;
    if (!parseIpv4(target, targetAddress) ||
        !parseIpv4(cidr.substr(0, slash), network)) {
        return false;
    }
    unsigned int prefix = 0;
    const auto [ptr, ec] = std::from_chars(
        cidr.data() + slash + 1, cidr.data() + cidr.size(), prefix);
    if (ec != std::errc() || ptr != cidr.data() + cidr.size() || prefix > 32U) {
        return false;
    }
    if (prefix == 0U) return true;
    const std::uint32_t mask = 0xffffffffU << (32U - prefix);
    return (targetAddress & mask) == (network & mask);
}

inline int specificity(MatchKind kind) noexcept {
    switch (kind) {
    case MatchKind::ExactDomain:
    case MatchKind::ExactIp: return 4;
    case MatchKind::DomainSuffix: return 3;
    case MatchKind::Ipv4Cidr: return 2;
    case MatchKind::Any: return 1;
    }
    return 0;
}

inline bool matches(const RouteRule& rule, std::string_view target) {
    switch (rule.match) {
    case MatchKind::Any: return true;
    case MatchKind::ExactDomain:
    case MatchKind::DomainSuffix: {
        const std::string value = lower(target);
        const std::string pattern = lower(rule.pattern);
        if (rule.match == MatchKind::ExactDomain) return value == pattern;
        return value == pattern ||
               (value.size() > pattern.size() &&
                value.ends_with("." + pattern));
    }
    case MatchKind::ExactIp: return lower(target) == lower(rule.pattern);
    case MatchKind::Ipv4Cidr: return matchIpv4Cidr(target, rule.pattern);
    }
    return false;
}

inline bool supports(const EngineDescriptor& descriptor, ConnectionKind kind) {
    return descriptor.available &&
           std::ranges::find(descriptor.connectionKinds, kind) !=
               descriptor.connectionKinds.end();
}

inline bool nativeConnection(const VpnConnection& connection) noexcept {
    return connection.kind == ConnectionKind::Pptp ||
           connection.kind == ConnectionKind::OpenVpn ||
           connection.kind == ConnectionKind::WireGuard;
}

// 系统辅助出口只安装具体 IPv4 路由。全流量规则留给主核心的绑定接口
// 出站处理，绝不能将其转换成第二条系统默认路由。
inline std::optional<std::string> nativeIpv4Destination(std::string_view value) {
    const std::size_t slash = value.find('/');
    const std::string_view address = value.substr(0, slash);
    std::uint32_t parsedAddress = 0;
    if (!parseIpv4(address, parsedAddress)) return std::nullopt;
    if (slash == std::string_view::npos) return std::string(value) + "/32";

    unsigned int prefix = 0;
    const auto [ptr, ec] = std::from_chars(
        value.data() + slash + 1, value.data() + value.size(), prefix);
    if (ec != std::errc() || ptr != value.data() + value.size() ||
        prefix == 0U || prefix > 32U) {
        return std::nullopt;
    }
    return std::string(value);
}

inline std::optional<std::string> nativeDestination(const RouteRule& rule) {
    switch (rule.match) {
    case MatchKind::Ipv4Cidr:
    case MatchKind::ExactIp:
        return nativeIpv4Destination(rule.pattern);
    case MatchKind::Any:
    case MatchKind::ExactDomain:
    case MatchKind::DomainSuffix:
        // 域名需要 DNS/规则层解析，不能直接变成稳定的内核路由。
        return std::nullopt;
    }
    return std::nullopt;
}

inline bool hasNativeRoute(const std::vector<TunNativeRoute>& routes,
                           std::string_view destination,
                           std::string_view connectionId) {
    return std::ranges::any_of(routes, [&](const TunNativeRoute& route) {
        return route.destination == destination &&
               route.connectionId == connectionId;
    });
}

inline void appendNativeRoute(TunRoutePlan& plan, const VpnConnection& connection,
                             std::string destination) {
    if (destination.empty() ||
        hasNativeRoute(plan.nativeRoutes, destination, connection.id)) {
        return;
    }
    plan.nativeRoutes.push_back({std::move(destination), connection.id,
                                connection.interfaceName, connection.gateway});
    plan.capture.routeExcludeAddress.push_back(
        plan.nativeRoutes.back().destination);
}

} // namespace detail

export inline std::vector<EngineKind> DefaultEngineOrder() {
    // sing-box 是默认实现；系统 VPN 引擎是后备实现。排序只表达偏好，最终仍需
    // 经过 EngineDescriptor 的能力/可用性检查。
    return {EngineKind::SingBox, EngineKind::SystemPptp,
            EngineKind::SystemWireGuard};
}

export inline EngineSelection SelectEngine(
    const VpnConnection& connection,
    std::span<const EngineDescriptor> descriptors) {
    std::vector<EngineKind> preference = connection.enginePreference;
    if (preference.empty()) preference = DefaultEngineOrder();

    for (const EngineKind preferred : preference) {
        for (const EngineDescriptor& descriptor : descriptors) {
            if (descriptor.kind == preferred &&
                detail::supports(descriptor, connection.kind)) {
                return {descriptor.kind, "按连接偏好选择"};
            }
        }
    }
    return {std::nullopt, "没有可用引擎支持该连接类型"};
}

// 统一连接策略的纯领域管理器。实际 connect/disconnect/apply-routes 由平台
// adapter 执行；该类只负责决定“连什么、用哪个引擎、目标交给谁”。
export class VpnManager {
public:
    VpnManager() = default;
    ~VpnManager() { disconnectAll(); }

    VpnManager(const VpnManager&) = delete;
    VpnManager& operator=(const VpnManager&) = delete;

    void setEngines(std::vector<EngineDescriptor> descriptors) {
        disconnectAll();
        adapters_.clear();
        engines_ = std::move(descriptors);
    }

    void setAdapters(std::vector<EngineAdapter> adapters) {
        disconnectAll();
        adapters_ = std::move(adapters);
        engines_.clear();
        engines_.reserve(adapters_.size());
        for (const EngineAdapter& adapter : adapters_) {
            engines_.push_back(adapter.descriptor);
        }
    }

    void setConnections(std::vector<VpnConnection> connections) {
        disconnectAll();
        connections_ = std::move(connections);
    }

    // 添加或更新一条连接，保留同一 id 的运行状态和 activeEngine，便于
    // 多条原生 VPN 逐条建立而不覆盖已经连接的兄弟连接。
    void upsertConnection(VpnConnection connection) {
        if (VpnConnection* current = findMutable(connection.id);
            current != nullptr) {
            const ConnectionState state = current->state;
            const std::optional<EngineKind> active = current->activeEngine;
            const std::string interfaceName = current->interfaceName;
            const std::string gateway = current->gateway;
            const std::string transportAddress = current->transportAddress;
            *current = std::move(connection);
            current->state = state;
            current->activeEngine = active;
            current->interfaceName = interfaceName;
            current->gateway = gateway;
            current->transportAddress = transportAddress;
            return;
        }
        connections_.push_back(std::move(connection));
    }

    void removeConnection(std::string_view id) {
        disconnect(id);
        std::erase_if(connections_, [id](const VpnConnection& connection) {
            return connection.id == id;
        });
    }

    // 初始化/测试装载用；已有连接时应使用 applyPolicy 同步运行时路由。
    inline void setPolicy(VpnPolicy policy) { policy_ = std::move(policy); }

    // applyRoutes 接收完整目标集合（包括空集合），后端负责增删差异和
    // 路由所有权。策略只在全部连接成功后提交；任一失败会还原已尝试的
    // 连接，包括可能已部分修改系统路由的失败连接。
    inline bool applyPolicy(VpnPolicy policy, std::string& error) {
        struct Change {
            VpnConnection* connection;
            EngineAdapter* adapter;
            std::vector<std::string> previous;
            std::vector<std::string> desired;
        };
        std::vector<Change> changes;
        for (VpnConnection& connection : connections_) {
            if (connection.state != ConnectionState::Connected ||
                !connection.activeEngine || !detail::nativeConnection(connection) ||
                *connection.activeEngine == EngineKind::SingBox) {
                continue;
            }
            if (!validateNativeRoutes(connection, error)) return false;
            EngineAdapter* adapter = findAdapter(*connection.activeEngine);
            auto previous = nativeRoutesFor(connection.id, policy_);
            auto desired = nativeRoutesFor(connection.id, policy);
            if (previous == desired) continue;
            if (adapter == nullptr || !adapter->applyRoutes) {
                error = std::format("{}：引擎不支持动态更新路由", connection.name);
                return false;
            }
            changes.push_back({&connection, adapter, std::move(previous),
                               std::move(desired)});
        }

        const auto apply = [](Change& change,
                              std::span<const std::string> routes,
                              std::string& failure) {
            try {
                return change.adapter->applyRoutes(*change.connection, routes,
                                                   failure);
            } catch (const std::exception& exception) {
                failure = exception.what();
            } catch (...) {
                failure = "引擎异常";
            }
            return false;
        };
        for (std::size_t index = 0; index < changes.size(); ++index) {
            std::string failure;
            if (apply(changes[index], changes[index].desired, failure)) continue;
            error = std::format("{}：路由更新失败{}", changes[index].connection->name,
                                failure.empty() ? "" : " · " + failure);
            for (std::size_t rollback = index + 1; rollback > 0; --rollback) {
                Change& change = changes[rollback - 1];
                std::string rollbackError;
                if (!apply(change, change.previous, rollbackError)) {
                    error += std::format("；{} 路由回滚失败{}", change.connection->name,
                                         rollbackError.empty() ? ""
                                                               : " · " + rollbackError);
                }
            }
            return false;
        }
        policy_ = std::move(policy);
        error.clear();
        return true;
    }

    const std::vector<EngineDescriptor>& engines() const noexcept {
        return engines_;
    }

    const std::vector<VpnConnection>& connections() const noexcept {
        return connections_;
    }

    const VpnPolicy& policy() const noexcept { return policy_; }

    const VpnConnection* findConnection(std::string_view id) const noexcept {
        for (const VpnConnection& connection : connections_) {
            if (connection.id == id) return &connection;
        }
        return nullptr;
    }

    std::string mainConnectionId() const {
        const VpnConnection* main = findConnection(policy_.defaultMainId);
        if (main != nullptr && main->enabled && main->canBeMain &&
            selectEngine(main->id).ok()) {
            return main->id;
        }
        // 配置的主 VPN 不可用时，按声明顺序选举第一个可用候选，避免默认
        // 引擎缺失时整机路由直接失去出口。
        for (const VpnConnection& connection : connections_) {
            if (connection.enabled && connection.canBeMain &&
                selectEngine(connection.id).ok()) {
                return connection.id;
            }
        }
        return {};
    }

    EngineSelection selectEngine(std::string_view id) const {
        const VpnConnection* connection = findConnection(id);
        if (connection == nullptr) return {std::nullopt, "VPN 连接不存在"};
        if (!connection->enabled) return {std::nullopt, "VPN 连接未启用"};
        return SelectEngine(*connection, engines_);
    }

    // 连接失败时继续尝试该连接的下一个可用引擎；路由安装失败也会回滚
    // disconnect，避免留下“VPN 已连但路由未生效”的半状态。
    bool connect(std::string_view id, std::string& error) {
        VpnConnection* connection = findMutable(id);
        if (connection == nullptr) {
            error = "VPN 连接不存在";
            return false;
        }
        // Repeated connect requests must not orphan an already-owned session
        // when the adapter refuses a duplicate dial. Reconfiguration callers
        // explicitly disconnect before replacing the connection definition.
        if (connection->state == ConnectionState::Connected && connection->activeEngine) {
            error.clear();
            return true;
        }
        if (!connection->enabled) {
            error = "VPN 连接未启用";
            return false;
        }
        std::size_t activeCount = 0;
        for (const VpnConnection& other : connections_) {
            if (other.id != connection->id &&
                other.state == ConnectionState::Connected &&
                other.activeEngine.has_value()) {
                ++activeCount;
            }
        }
        const std::size_t maxConnections =
            MaxConcurrentAuxiliaryConnections();
        if (activeCount >= maxConnections) {
            connection->state = ConnectionState::Failed;
            connection->activeEngine.reset();
            error = std::format("当前平台同时最多连接 {} 条内网 VPN",
                                maxConnections);
            return false;
        }
        if (!validateNativeRoutes(*connection, error)) return false;

        connection->state = ConnectionState::Connecting;
        std::vector<EngineKind> preference = connection->enginePreference;
        if (preference.empty()) preference = DefaultEngineOrder();
        std::string lastError;
        for (const EngineKind preferred : preference) {
            EngineAdapter* adapter = findAdapter(preferred);
            if (adapter == nullptr ||
                !detail::supports(adapter->descriptor, connection->kind)) {
                continue;
            }
            try {
                std::string attemptError;
                if (!adapter->connect ||
                    !adapter->connect(*connection, attemptError)) {
                    lastError = attemptError.empty() ? "引擎连接失败" : attemptError;
                    if (adapter->disconnect) adapter->disconnect(*connection);
                    connection->interfaceName.clear();
                    connection->gateway.clear();
                    continue;
                }
                const std::vector<std::string> nativeRoutes =
                    nativeRoutesFor(connection->id, policy_);
                if (preferred != EngineKind::SingBox && adapter->applyRoutes &&
                    !adapter->applyRoutes(*connection, nativeRoutes, attemptError)) {
                    if (adapter->disconnect) adapter->disconnect(*connection);
                    lastError = attemptError.empty() ? "VPN 路由安装失败" : attemptError;
                    continue;
                }
            } catch (const std::exception& exception) {
                // 适配器可能在创建 pppd/RAS 会话后抛出异常；尽力回收，再
                // 继续尝试下一个引擎，绝不让连接永远停在 Connecting。
                try {
                    if (adapter->disconnect) adapter->disconnect(*connection);
                } catch (...) {
                }
                lastError = std::format("引擎异常：{}", exception.what());
                continue;
            } catch (...) {
                try {
                    if (adapter->disconnect) adapter->disconnect(*connection);
                } catch (...) {
                }
                lastError = "引擎异常";
                continue;
            }
            connection->state = ConnectionState::Connected;
            connection->activeEngine = preferred;
            error.clear();
            return true;
        }
        connection->state = ConnectionState::Failed;
        connection->activeEngine.reset();
        connection->interfaceName.clear();
        connection->gateway.clear();
        error = lastError.empty() ? "没有可用引擎支持该连接类型" : lastError;
        return false;
    }

    void disconnect(std::string_view id) {
        VpnConnection* connection = findMutable(id);
        if (connection == nullptr) return;
        if (connection->activeEngine.has_value()) {
            if (EngineAdapter* adapter = findAdapter(*connection->activeEngine);
                adapter != nullptr && adapter->disconnect) {
                try {
                    adapter->disconnect(*connection);
                } catch (...) {
                    // 析构/退出路径必须继续清理其余连接，即使单个适配器
                    // 报错也不能把资源释放异常带出析构函数。
                }
            }
        }
        connection->activeEngine.reset();
        connection->state = ConnectionState::Idle;
        connection->interfaceName.clear();
        connection->gateway.clear();
        connection->transportAddress.clear();
    }

    // 统一释放所有仍由适配器持有的系统连接。析构和应用的显式退出流程都
    // 走这里，避免 pppd/RAS 在应用退出后继续保持“连接中”。
    void disconnectAll() {
        std::vector<std::string> activeIds;
        activeIds.reserve(connections_.size());
        for (const VpnConnection& connection : connections_) {
            if (connection.activeEngine.has_value()) {
                activeIds.push_back(connection.id);
            }
        }
        for (const std::string& id : activeIds) disconnect(id);
    }

    // 返回目标应该交给的 VPN；没有命中规则时使用默认主 VPN。返回空表示
    // 直连/交给系统默认路由。规则本身不负责建立连接。
    std::string resolveConnection(std::string_view target) const {
        const RouteRule* selected = nullptr;
        for (const RouteRule& rule : policy_.rules) {
            const VpnConnection* connection = findConnection(rule.connectionId);
            if (connection == nullptr || !connection->enabled ||
                (detail::nativeConnection(*connection) &&
                 connection->state != ConnectionState::Connected) ||
                !detail::matches(rule, target)) {
                continue;
            }
            if (selected == nullptr || rule.priority > selected->priority ||
                (rule.priority == selected->priority &&
                 detail::specificity(rule.match) >
                     detail::specificity(selected->match))) {
                selected = &rule;
            }
        }
        return selected == nullptr ? mainConnectionId() : selected->connectionId;
    }

private:
    static inline bool validateNativeRoutes(const VpnConnection& connection,
                                           std::string& error) {
        if (!detail::nativeConnection(connection)) return true;
        for (const std::string& route : connection.internalRoutes) {
            if (!detail::nativeIpv4Destination(route)) {
                error = std::format("{}：辅助 VPN 路由必须是具体 IPv4 地址或 /1–/32 网段，不能接管系统默认路由",
                                    connection.name);
                return false;
            }
        }
        return true;
    }

    inline std::vector<std::string> nativeRoutesFor(std::string_view id,
                                                   const VpnPolicy& policy) const {
        const VpnConnection* connection = findConnection(id);
        if (connection == nullptr || !detail::nativeConnection(*connection)) {
            return {};
        }
        std::vector<std::string> routes;
        for (const std::string& route : connection->internalRoutes) {
            if (auto destination = detail::nativeIpv4Destination(route);
                destination && std::ranges::find(routes, *destination) == routes.end()) {
                routes.push_back(std::move(*destination));
            }
        }
        for (const RouteRule& rule : policy.rules) {
            if (rule.connectionId != id) continue;
            if (const auto destination = detail::nativeDestination(rule);
                destination.has_value() &&
                std::ranges::find(routes, *destination) == routes.end()) {
                routes.push_back(*destination);
            }
        }
        return routes;
    }

    VpnConnection* findMutable(std::string_view id) noexcept {
        for (VpnConnection& connection : connections_) {
            if (connection.id == id) return &connection;
        }
        return nullptr;
    }

    EngineAdapter* findAdapter(EngineKind kind) noexcept {
        for (EngineAdapter& adapter : adapters_) {
            if (adapter.descriptor.kind == kind) return &adapter;
        }
        return nullptr;
    }

    std::vector<EngineDescriptor> engines_;
    std::vector<EngineAdapter> adapters_;
    std::vector<VpnConnection> connections_;
    VpnPolicy policy_;
};

// 兼容的纯计划生成接口；本函数不安装路由，也不刷新运行中的主核心。
// 运行时主订阅与补偿配置由 store/core 编排，原生路由由 applyPolicy 提交。
//
// 对 sing-box 代理和 OpenVPN endpoint，logicalRules 留给 sing-box/编排器处理；
// 对 PPTP、WireGuard，CIDR/单 IP 规则转成 nativeRoutes，并把这些目标加入
// TUN 排除表。这样主 sing-box TUN 负责默认流量，而 A/B 公司网段按最长前缀
// 进入各自的 ppp/tun/wg 接口。
export inline TunRoutePlan BuildTunRoutePlan(const VpnManager& manager,
                                              PlatformKind platform) {
    TunRoutePlan plan{
        .capture = MakeFullTunConfig(platform),
        .mainConnectionId = manager.mainConnectionId(),
        .logicalRules = manager.policy().rules,
    };

    if (plan.mainConnectionId.empty()) {
        plan.capture.mode = TunCaptureMode::Disabled;
        plan.capture.ready = false;
        plan.capture.note = "没有可用的主 VPN，不能生成全流量接管计划";
        return plan;
    }

    for (const VpnConnection& connection : manager.connections()) {
        if (!connection.enabled || !detail::nativeConnection(connection) ||
            (connection.activeEngine && *connection.activeEngine == EngineKind::SingBox)) {
            continue;
        }
        for (const std::string& route : connection.internalRoutes) {
            if (auto destination = detail::nativeIpv4Destination(route)) {
                detail::appendNativeRoute(plan, connection, std::move(*destination));
            }
        }
    }

    for (const RouteRule& rule : manager.policy().rules) {
        const VpnConnection* connection = manager.findConnection(rule.connectionId);
        if (connection == nullptr || !connection->enabled ||
            !detail::nativeConnection(*connection) ||
            (connection->activeEngine && *connection->activeEngine == EngineKind::SingBox)) {
            continue;
        }
        if (const auto destination = detail::nativeDestination(rule)) {
            detail::appendNativeRoute(plan, *connection, *destination);
        }
    }

    return plan;
}

} // namespace vpn
