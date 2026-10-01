// Shared policy persistence and runtime projection. No store-to-store callbacks:
// the core reads saved profiles and overlays the native session snapshot.
module;
#include "wire_codec.h"

export module clashflux.routing;

import std;
import clashflux.db;
import clashflux.vpn;
import clashflux.singbox;

namespace routing {

export inline std::string ConnectionId(std::int64_t id) {
    return std::format("profile-{}", id);
}

export inline std::string EncodePolicy(const vpn::VpnPolicy& policy) {
    clashflux::wire::Policy wire;
    wire.format_version = 2;
    wire.revision = policy.revision;
    wire.default_main_id = policy.defaultMainId;
    for (const auto& rule : policy.rules) {
        wire.rules.push_back({std::string(vpn::MatchKindName(rule.match)), rule.pattern,
                              rule.connectionId, rule.priority, rule.id, std::string(vpn::RuleTierKey(rule.tier)),
            rule.enabled, std::string(vpn::UnavailablePolicyKey(rule.unavailable)),
            std::string(vpn::TargetKindKey(rule.targetKind)), rule.targetObject, rule.order});
    }
    auto result = clashflux::wire::EncodePolicy(wire);
    if (!result) throw std::runtime_error("全局路由配置编码失败：" + result.error);
    return std::move(result.value);
}

export inline vpn::VpnPolicy DecodePolicy(const std::string& text) {
    if (text.empty()) return {};
    const auto value = clashflux::wire::DecodePolicy(text);
    if (!value) throw std::runtime_error("全局路由配置格式错误：" + value.error);
    if (value.value.format_version < 1 || value.value.format_version > 2)
        throw std::runtime_error("不支持的编排策略版本；保留原配置");
    vpn::VpnPolicy policy;
    policy.revision = value.value.revision;
    policy.defaultMainId = value.value.default_main_id;
    for (const auto& item : value.value.rules) {
        const auto match = vpn::ParseMatchKind(item.match);
        if (!match) throw std::runtime_error("全局路由规则匹配类型无效");
        if (item.connection_id.empty()) throw std::runtime_error("全局路由缺少目标连接");
        const auto tier = vpn::ParseRuleTier(item.tier);
        const auto unavailable = vpn::ParseUnavailablePolicy(item.unavailable);
        const auto kind = vpn::ParseTargetKind(item.target_kind);
        if (!tier || !unavailable || !kind) throw std::runtime_error("编排规则枚举值无效");
        policy.rules.push_back({.match = *match, .pattern = item.pattern,
            .connectionId = item.connection_id, .priority = item.priority,
            .id = item.id, .tier = *tier, .enabled = item.enabled, .unavailable = *unavailable,
            .targetKind = *kind, .targetObject = item.target_object, .order = item.order});
    }
    if (value.value.format_version == 1) {
        // 将旧版同权重的匹配类型排序固化成顺序；新规则只按显式 order。
        const auto specificity = [](vpn::MatchKind kind) {
            switch (kind) {
            case vpn::MatchKind::ExactDomain: case vpn::MatchKind::ExactIp: return 4;
            case vpn::MatchKind::DomainSuffix: return 3;
            case vpn::MatchKind::Ipv4Cidr: return 2;
            case vpn::MatchKind::Any: return 1;
            }
            return 0;
        };
        std::stable_sort(policy.rules.begin(), policy.rules.end(), [&](const auto& a, const auto& b) {
            if (a.priority != b.priority) return a.priority > b.priority;
            return specificity(a.match) > specificity(b.match);
        });
        for (std::size_t i = 0; i < policy.rules.size(); ++i) {
            policy.rules[i].id = "legacy-" + std::to_string(i);
            policy.rules[i].order = static_cast<std::int64_t>(i);
        }
    }
    std::string error;
    if (!vpn::ValidatePolicyRules(policy.rules, error)) throw std::runtime_error(error);
    return policy;
}

export inline std::vector<std::string> SplitRoutes(std::string_view text) {
    std::vector<std::string> result;
    while (!text.empty()) {
        const auto end = text.find_first_of(",\n");
        auto route = text.substr(0, end);
        const auto first = route.find_first_not_of(" \t\r");
        if (first != std::string_view::npos) {
            route = route.substr(first, route.find_last_not_of(" \t\r") - first + 1);
            if (std::ranges::find(result, route) == result.end()) result.emplace_back(route);
        }
        if (end == std::string_view::npos) break;
        text.remove_prefix(end + 1);
    }
    return result;
}

export inline bool ValidatePlatformPolicy(const vpn::VpnPolicy& policy,
    std::span<const db::Profile> profiles, std::string& error) {
    if (!vpn::ValidatePolicyRules(policy.rules, error)) return false;
    for (const auto& rule : policy.rules) {
        for (const auto& profile : profiles) {
            if (ConnectionId(profile.id) == rule.connectionId &&
                (profile.type == "pptp" || profile.type == "openvpn") && rule.targetKind != vpn::TargetKind::Default) {
                error = "原生连接只支持默认出口引用"; return false;
            }
        }
    }
#if defined(__ANDROID__) || defined(CLASHFLUX_IOS)
    for (const auto& rule : policy.rules) {
        if (!rule.enabled) continue;
        for (const auto& profile : profiles) {
            if (ConnectionId(profile.id) != rule.connectionId) continue;
            if (profile.type != "pptp" && profile.type != "openvpn" && !profile.selected) {
                error = "手机平台只允许规则引用当前活动代理订阅";
                return false;
            }
        }
    }
#endif
    return true;
}

// Exactly one selected proxy profile is the main VPN. Native profiles are
// always present, even while disconnected, so their rules can fail closed.
// Session state is never persisted: a saved interface name cannot establish
// that an interface still belongs to this connection after a restart.
export inline void PopulateOptions(
    singbox::CompileOptions& options, std::span<const db::Profile> profiles,
    const vpn::VpnPolicy& policy,
    std::span<const singbox::NativeConnection> sessions) {
    options.mainConnectionId.clear();
    options.mainSourceName.clear();
    options.auxiliarySources.clear();
    options.planRevision = policy.revision;
    options.globalRules = policy.rules;
    options.nativeConnections.clear();
    for (const auto& profile : profiles) {
        const auto id = ConnectionId(profile.id);
        if (profile.type != "pptp" && profile.type != "openvpn") {
            if (profile.selected) {
                if (!options.mainConnectionId.empty()) {
                    throw std::runtime_error("只能启用一个主 VPN 订阅");
                }
                options.mainConnectionId = id;
                options.mainSourceName = profile.name;
            }
            continue;
        }
        singbox::NativeConnection native;
        native.id = id;
        native.kind = profile.type == "pptp" ? vpn::ConnectionKind::Pptp : vpn::ConnectionKind::OpenVpn;
        native.internalRoutes = SplitRoutes(profile.nativeRoutes);
        for (const auto& session : sessions) {
            if (session.id != id) continue;
            // PPTP needs a live OS interface. OpenVPN is a sing-box endpoint
            // and deliberately has no interface name in userspace mode.
            native.connected = session.connected &&
                (native.kind == vpn::ConnectionKind::OpenVpn ||
                 !session.interfaceName.empty());
            native.interfaceName = native.connected &&
                    native.kind == vpn::ConnectionKind::Pptp
                ? session.interfaceName
                : "";
            native.transportAddress = native.connected ? session.transportAddress : "";
            native.nativeConfig = native.connected ? session.nativeConfig : "";
            break;
        }
        options.nativeConnections.push_back(std::move(native));
    }
}

} // namespace routing
