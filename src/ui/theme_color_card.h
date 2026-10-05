#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <huxerui/huxerui.h>

namespace clashflux::ui {

// 页面特有的几何；间距、圆角、图标及选中描边仍从当前 ThemeSpec 取值。
inline constexpr float kThemeColorCardEdge = 112.0F;
inline constexpr float kThemeColorCompactCardEdge = 80.0F;
inline constexpr float kThemeColorDialogWidth = 280.0F;
inline constexpr float kThemeColorPreviewHeight = 48.0F;

// 模式、预设、自定义色及添加入口共用同一个正方形卡片表面。
inline huxerui::View ThemeColorCardSurface(const huxerui::ThemeSpec& theme,
    huxerui::Color fill, huxerui::Color foreground, std::optional<huxerui::ImageResource> symbol,
    bool selected, std::string label, std::function<void()> onClick,
    std::optional<huxerui::StringVariant> caption = std::nullopt, float edge = kThemeColorCardEdge) {
    huxerui::View content;
    if (symbol) content = huxerui::Image(*symbol).Tint(foreground)
        .With(huxerui::Frame{.width = theme.spacing.extra_large, .height = theme.spacing.extra_large})
        .Key("theme-color-symbol");
    huxerui::View text;
    if (caption) text = huxerui::Text(*caption).Style(huxerui::TextStyle{
        huxerui::Font::System(theme.typography.body_small), foreground});
    huxerui::View card = huxerui::Column {content, text}.With(
        huxerui::Frame{.width = edge, .height = edge}, huxerui::Spacing(theme.spacing.small),
        huxerui::Padding(theme.spacing.small), huxerui::Background(fill),
        huxerui::CornerRadius(theme.shapes.medium), huxerui::ClipChildren(),
        huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
        theme.interactions.indication, huxerui::Focusable(true),
        huxerui::Semantics{.role = huxerui::SemanticRole::Button, .label = std::move(label), .selected = selected});
    if (selected) card = std::move(card).With(huxerui::Border{foreground, theme.interactions.focus_ring.width});
    return std::move(card).OnClick(std::move(onClick));
}

} // namespace clashflux::ui
