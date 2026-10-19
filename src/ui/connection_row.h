#pragma once

#include <cstdint>
#include <string>
#include <huxerui/huxerui.h>

#include "ui.h"

namespace clashflux::ui {

struct ConnectionRow {
    std::string id;
    std::string host;       // metadata.host 或 destinationIP:port
    std::string network;    // tcp / udp
    std::string chains;     // 节点链 "DIRECT" / "节点 ← 策略组"
    std::string rule;       // 命中规则
    std::int64_t up = 0;
    std::int64_t down = 0;
    std::string start;      // HH:MM:SS

    bool operator==(const ConnectionRow&) const = default;
};

// VirtualList 的 factory 在布局期执行；资源与主题读取必须等行挂载后进入 Scope。
inline huxerui::View ConnectionRowContent(ConnectionRow row, std::string traffic,
    bool compact, huxerui::View closeButton) {
    return huxerui::Scope([row = std::move(row), traffic = std::move(traffic), compact, closeButton] {
        const auto& theme = huxerui::UseTheme();
        const auto mono = [](huxerui::StringVariant text, huxerui::Color color, float width) {
            huxerui::View view = huxerui::Text(text).Style(huxerui::TextStyle{
                huxerui::Font::Monospace(font_size::kMonoBody), color});
            if (width > 0.0F) return std::move(view).With(huxerui::Frame{.width = width});
            return std::move(view).With(huxerui::Grow(1.0F));
        };
        const huxerui::StringVariant host = row.host == "未知目标"
            ? Localized("未知目标") : huxerui::StringVariant{row.host};
        // 规则既用于直接显示，也作为本地化模板的参数；这里只解析一次拥有型字符串。
        const std::string rule = row.rule == "未匹配规则"
            ? huxerui::UseString(Localized("未匹配规则")) : row.rule;
        if (compact) {
            return huxerui::View{huxerui::Column {
              huxerui::Row {
                mono(host, theme.colors.on_surface, 0.0F),
                mono(row.network, theme.colors.on_surface_variant, 55.0F),
                closeButton,
              }.With(huxerui::Spacing(6.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
              mono(LocalizedFormat("链路：{}", row.chains), theme.colors.on_surface_variant, 0.0F),
              huxerui::Row {
                mono(traffic, theme.colors.on_surface_variant, 0.0F),
                mono(LocalizedFormat("规则：{}", rule), theme.colors.on_surface_variant, 0.0F),
              }.With(huxerui::Spacing(8.0F)),
            }};
        }
        return huxerui::View{huxerui::Row {
          mono(host, theme.colors.on_surface, 0.0F),
          mono(row.network, theme.colors.on_surface_variant, 50.0F),
          mono(row.chains, theme.colors.on_surface_variant, 220.0F),
          mono(traffic, theme.colors.on_surface_variant, 160.0F),
          mono(rule, theme.colors.on_surface_variant, 140.0F),
          closeButton,
        }};
    });
}

} // namespace clashflux::ui
