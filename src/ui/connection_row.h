#pragma once

#include <cstdint>
#include <string>
#include <huxerui/huxerui.h>

#include "ui.h"

namespace clashflux::ui {

struct ConnectionRow {
    std::string id;
    std::string host;       // metadata.host 或 destinationIP:port
    std::string destinationIP;
    std::string network;    // tcp / udp
    std::string chains;     // 节点链 "DIRECT" / "节点 ← 策略组"
    std::string rule;       // 命中规则
    std::int64_t up = 0;
    std::int64_t down = 0;
    std::int64_t upRate = 0;
    std::int64_t downRate = 0;
    std::string start;      // 内核提供的连接开始时间

    bool operator==(const ConnectionRow&) const = default;
};

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
        const std::string rule = row.rule == "未匹配规则"
            ? huxerui::UseString(Localized("未匹配规则")) : row.rule;
        const auto mono = [&theme](std::string value, float width) {
            return huxerui::Text(std::move(value)).Style(huxerui::TextStyle{
                huxerui::Font::Monospace(font_size::kMonoBody), theme.colors.on_surface_variant})
                .With(huxerui::Frame{.width = width});
        };
        huxerui::View metrics = huxerui::Column{
            huxerui::Row{mono("↑ " + upRate, 114.0F),
                         mono("↑ " + upTotal, 100.0F)}.With(huxerui::Spacing(8.0F)),
            huxerui::Row{mono("↓ " + downRate, 114.0F),
                         mono("↓ " + downTotal, 100.0F)}.With(huxerui::Spacing(8.0F)),
        }.With(huxerui::Spacing(2.0F));
        huxerui::View metadata = huxerui::Column{
            huxerui::Text(ConnectionPreviewText(row.chains, 24)),
            huxerui::Text(ConnectionPreviewText(rule, 24)),
        }.With(huxerui::Spacing(2.0F),
               huxerui::Frame{.max_width = compact ? 300.0F : 260.0F},
               huxerui::MainAlign(huxerui::MainAxisAlignment::End),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::End));
        huxerui::View lower = compact
            ? huxerui::View{huxerui::Column{metrics, metadata}.With(
                  huxerui::Spacing(4.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))}
            : huxerui::View{huxerui::Row{metrics, huxerui::Spacer(), metadata}.With(
                  huxerui::Spacing(8.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))};
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
