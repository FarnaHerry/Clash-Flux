// Shared policy persistence and runtime projection. No store-to-store callbacks:
// the core reads saved profiles and overlays the native session snapshot.
export module clashflux.routing;

import std;
import nlohmann.json;
import clashflux.db;
import clashflux.vpn;
import clashflux.singbox;

namespace routing {

export inline std::string ConnectionId(std::int64_t id) {
    return std::format("profile-{}", id);
}

export inline nlohmann::json EncodePolicy(const vpn::VpnPolicy& policy) {
    nlohmann::json result{{"default_main_id", policy.defaultMainId},
                          {"rules", nlohmann::json::array()}};
    for (const auto& rule : policy.rules) {
        result["rules"].push_back({
            {"match", std::string(vpn::MatchKindName(rule.match))},
            {"pattern", rule.pattern}, {"connection_id", rule.connectionId},
            {"priority", rule.priority}});
    }
    return result;
}

export inline vpn::VpnPolicy DecodePolicy(const std::string& text) {
    if (text.empty()) return {};
    const auto value = nlohmann::json::parse(text);
    if (!value.is_object()) throw std::runtime_error("全局路由配置必须是对象");
    vpn::VpnPolicy policy;
    policy.defaultMainId = value.value("default_main_id", "");
    if (!value.contains("rules")) return policy;
    if (!value["rules"].is_array()) throw std::runtime_error("全局路由规则必须是列表");
    for (const auto& item : value["rules"]) {
        if (!item.is_object()) throw std::runtime_error("全局路由规则格式错误");
        const auto match = vpn::ParseMatchKind(item.value("match", ""));
        if (!match) throw std::runtime_error("全局路由规则匹配类型无效");
        vpn::RouteRule rule{.match = *match,
                            .pattern = item.value("pattern", ""),
                            .connectionId = item.value("connection_id", ""),
                            .priority = item.value("priority", 0)};
        if (rule.connectionId.empty()) throw std::runtime_error("全局路由缺少目标连接");
        policy.rules.push_back(std::move(rule));
    }
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

// Exactly one selected proxy profile is the main VPN. Native profiles are
// always present, even while disconnected, so their rules can fail closed.
// Session state is never persisted: a saved interface name cannot establish
// that an interface still belongs to this connection after a restart.
export inline void PopulateOptions(
    singbox::CompileOptions& options, std::span<const db::Profile> profiles,
    const vpn::VpnPolicy& policy,
    std::span<const singbox::NativeConnection> sessions) {
    options.mainConnectionId.clear();
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
            }
            continue;
        }
        singbox::NativeConnection native;
        native.id = id;
        native.internalRoutes = SplitRoutes(profile.nativeRoutes);
        for (const auto& session : sessions) {
            if (session.id != id) continue;
            native.connected = session.connected && !session.interfaceName.empty();
            native.interfaceName = native.connected ? session.interfaceName : "";
            break;
        }
        options.nativeConnections.push_back(std::move(native));
    }
}

} // namespace routing
