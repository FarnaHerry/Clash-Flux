#pragma once

#include <algorithm>
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
inline constexpr float kThemeColorSliderTrackRadiusFactor = 0.5F;
inline constexpr float kThemeColorSliderThumbInsetFactor = 0.5F;

// HSB channel gradients are painted beneath the native Slider; the native component keeps input,
// semantics, focus, and the selected-color thumb while its own theme-colored tracks are hidden.
inline huxerui::SliderStyle HsbGradientSliderStyle(huxerui::SliderStyle style, huxerui::Color selected) {
    const auto disabledColor = [selected](huxerui::Color appearance) {
        huxerui::Color color = selected;
        color.alpha *= appearance.alpha;
        return color;
    };
    const auto transparent = huxerui::Color::Transparent();
    style.active_track = transparent;
    style.inactive_track = transparent;
    style.active_tick = transparent;
    style.inactive_tick = transparent;
    style.stop_indicator = transparent;
    style.disabled_active_track = transparent;
    style.disabled_inactive_track = transparent;
    style.disabled_active_tick = transparent;
    style.disabled_inactive_tick = transparent;
    style.disabled_stop_indicator = transparent;
    style.thumb = selected;
    style.disabled_thumb = disabledColor(style.disabled_thumb);
    if (style.focus_ring) style.focus_ring->color = selected;
    return style;
}

inline huxerui::View HsbGradientSliderTrack(
    huxerui::LinearGradient gradient, const huxerui::SliderStyle& style) {
    const float thumbInset = std::max({style.thumb_width, style.hovered_thumb_width,
                                       style.pressed_thumb_width}) * kThemeColorSliderThumbInsetFactor;
    return huxerui::Row {
      huxerui::Row {}.With(huxerui::Grow(1.0F), huxerui::Frame{.height = style.track_height},
          huxerui::Background(std::move(gradient)),
          huxerui::CornerRadius(style.track_height * kThemeColorSliderTrackRadiusFactor)),
    }.With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(thumbInset, 0.0F)),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

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
