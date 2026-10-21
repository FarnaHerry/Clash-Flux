#pragma once

#include <cstdint>
#include <array>
#include <string>
#include <string_view>
#include <huxerui/huxerui.h>

#include "ui.h"

namespace clashflux::ui {

struct ConnectionRow {
    std::string id;
    std::string host;       // metadata.host 或 destinationIP:port
    std::string inboundType; // 连接实际进入的 sing-box inbound 类型
    std::string inbound;     // inbound 标签（详情页展示）
    std::string destinationIP;
    std::string network;    // tcp / udp
    std::string chains;     // 节点链 "DIRECT" / "节点 ← 策略组"
    std::string rule;       // 命中规则
    bool direct = false;    // 实际出口为直连，与全局 rule/global/direct 模式无关
    std::int64_t up = 0;
    std::int64_t down = 0;
    std::int64_t upRate = 0;
    std::int64_t downRate = 0;
    std::string start;      // 内核提供的连接开始时间

    bool operator==(const ConnectionRow&) const = default;
};

// sing-box 的 rule 是匹配项描述（如 rule_set=tag、domain_suffix=...），
// 后面还可能附有 " => route(...)"。卡片只提取匹配项类型，保留原文给详情页。
inline std::string ConnectionRuleTypes(std::string_view rule) {
    rule = rule.substr(0, rule.find(" => "));
    if (rule == "final") return "FINAL";
    constexpr std::array<std::string_view, 41> keys{
        "rule_set", "domain", "domain_suffix", "domain_keyword", "domain_regex",
        "ip_cidr", "ip_is_private", "source_ip_cidr", "source_ip_is_private",
        "port", "port_range", "source_port", "source_port_range",
        "process_name", "process_path", "process_path_regex", "package_name",
        "package_name_regex",
        "inbound", "ip_version", "network", "protocol", "client", "auth_user",
        "user", "user_id", "clash_mode", "network_type", "network_is_expensive",
        "network_is_constrained", "interface_address", "network_interface_address",
        "default_interface_address", "preferred_by", "wifi_ssid", "wifi_bssid",
        "source_mac_address", "source_hostname", "source_geoip", "geoip", "geosite"};
    std::string result;
    std::array<bool, keys.size()> seen{};
    for (std::size_t index = 0; index < rule.size();) {
        const auto equals = rule.find('=', index);
        if (equals == std::string_view::npos) break;
        auto begin = equals;
        while (begin > 0 && (rule[begin - 1] == '_' || rule[begin - 1] == '/' ||
                             (rule[begin - 1] >= 'a' && rule[begin - 1] <= 'z'))) --begin;
        const auto key = rule.substr(begin, equals - begin);
        for (std::size_t candidate = 0; candidate < keys.size(); ++candidate) {
            const auto known = keys[candidate];
            if (key != known || seen[candidate]) continue;
            seen[candidate] = true;
            if (!result.empty()) result += " + ";
            for (const char ch : known)
                result += ch == '_' ? '-' : static_cast<char>(ch - 'a' + 'A');
            break;
        }
        index = equals + 1;
    }
    if (!result.empty()) return result;
    if (rule.find("&&") != std::string_view::npos ||
        rule.find("||") != std::string_view::npos) return "LOGICAL";
    return "—";
}

inline std::string ConnectionPreviewText(const std::string& text,
                                         std::size_t maxCodePoints) {
    std::size_t count = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if ((static_cast<unsigned char>(text[index]) & 0xc0U) == 0x80U) continue;
        if (count++ == maxCodePoints) return text.substr(0, index) + "…";
    }
    return text;
}

// VirtualList 的 factory 在布局期执行；资源与主题读取必须等行挂载后进入 Scope。
inline huxerui::View ConnectionRowContent(ConnectionRow row, std::string upRate,
    std::string downRate, std::string upTotal, std::string downTotal,
    bool compact, huxerui::View moreButton) {
    return huxerui::Scope([row = std::move(row), upRate = std::move(upRate),
                           downRate = std::move(downRate), upTotal = std::move(upTotal),
                           downTotal = std::move(downTotal), compact, moreButton] {
        const auto& theme = huxerui::UseTheme();
        const std::string host = row.host == "未知目标"
            ? huxerui::UseString(Localized("未知目标")) : row.host;
        const std::string ruleTypes = ConnectionRuleTypes(row.rule);
        const auto mono = [&theme](std::string value, float width) {
            return huxerui::Text(std::move(value)).Style(huxerui::TextStyle{
                huxerui::Font::Monospace(font_size::kMonoBody), theme.colors.on_surface_variant})
                .With(huxerui::Frame{.width = width});
        };
        huxerui::View metrics = huxerui::Column{
            huxerui::Row{mono("↑ " + upRate, 90.0F),
                         mono("↑ " + upTotal, 75.0F)}.With(huxerui::Spacing(4.0F)),
            huxerui::Row{mono("↓ " + downRate, 90.0F),
                         mono("↓ " + downTotal, 75.0F)}.With(huxerui::Spacing(4.0F)),
        }.With(huxerui::Spacing(2.0F));
        const auto summary = [&theme](std::string value) {
            return huxerui::Text(std::move(value)).Style(huxerui::TextStyle{
                huxerui::Font::Monospace(font_size::kCaption), theme.colors.on_surface_variant});
        };
        huxerui::View metadata = huxerui::Column{
            summary(row.inboundType.empty() ? "—" : row.inboundType),
            summary(ConnectionPreviewText(
                row.direct ? "DIRECT · " + ruleTypes : ruleTypes, compact ? 20 : 30)),
        }.With(huxerui::Spacing(2.0F),
               huxerui::Frame{.width = compact ? 120.0F : 160.0F},
               huxerui::MainAlign(huxerui::MainAxisAlignment::End),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::End));
        huxerui::View lower = huxerui::Row{metrics, huxerui::Spacer(), metadata}.With(
            huxerui::Spacing(4.0F),
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
        return huxerui::View{huxerui::Column{
            huxerui::Row{
                huxerui::Text(row.network).Style(huxerui::TextStyle{
                    huxerui::Font::Monospace(font_size::kCaption), theme.colors.primary})
                    .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(5.0F, 2.0F)),
                          huxerui::Background(theme.colors.primary_container),
                          huxerui::CornerRadius(4.0F)),
                huxerui::Text(ConnectionPreviewText(host, compact ? 34 : 50)).Style(huxerui::TextStyle{
                    huxerui::Font::Monospace(font_size::kMonoBody), theme.colors.on_surface})
                    .With(huxerui::Grow(1.0F)),
                moreButton,
            }.With(huxerui::Spacing(6.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
            lower,
        }.With(huxerui::Spacing(5.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))};
    });
}

} // namespace clashflux::ui
