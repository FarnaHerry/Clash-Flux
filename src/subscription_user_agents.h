#pragma once

#include <array>
#include <algorithm>
#include <string>
#include <string_view>

namespace clashflux::subscription_http {

// Compatibility pins, deliberately independent of the application version and
// upstream releases. Raise these only with converter/kernel regression evidence.
inline constexpr std::array<std::string_view, 2> kUserAgents{
    "ClashMeta/1.19.32; mihomo/1.19.32", "sing-box/1.14.4"};
inline constexpr std::array<std::string_view, 2> kNames{"Clash (Mihomo)", "sing-box"};
inline constexpr std::string_view kOrderSetting = "subscription.ua_order";
inline constexpr std::array<std::string_view, 2> kOrders{"clash,sing-box", "sing-box,clash"};
inline constexpr std::array<std::array<std::size_t, 2>, 2> kOrderIndices{{{0, 1}, {1, 0}}};

inline std::size_t OrderIndex(std::string_view value) {
    const auto found = std::find(kOrders.begin(), kOrders.end(), value);
    return found == kOrders.end() ? 0 : static_cast<std::size_t>(found - kOrders.begin());
}

inline std::array<std::string_view, 2> UserAgents(std::string_view order) {
    const auto& indices = kOrderIndices[OrderIndex(order)];
    return {kUserAgents[indices[0]], kUserAgents[indices[1]]};
}

inline std::string Identity(std::string_view order) {
    std::string value;
    for (const auto agent : UserAgents(order)) {
        value += std::to_string(agent.size()) + ":";
        value += agent;
    }
    return value;
}

inline std::string OrderLabel(std::size_t index) {
    const auto& order = kOrderIndices.at(index);
    return std::string{kNames[order[0]]} + " → " + std::string{kNames[order[1]]};
}

} // namespace clashflux::subscription_http
