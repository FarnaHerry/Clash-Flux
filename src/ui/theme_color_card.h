#pragma once

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <huxerui/huxerui.h>

#include "theme_colors.h"
#include "ui.h"

namespace clashflux::ui {

// 页面特有的几何；间距、圆角、图标从 ThemeSpec 取值，描边使用统一卡片主题常量。
inline constexpr float kThemeColorCardEdge = 160.0F;
inline constexpr float kThemeColorCompactCardEdge = 128.0F;
inline constexpr float kThemeColorCardHeightRatio = 0.8F;
inline constexpr float kThemePreviewLineHeight = 4.0F;
inline constexpr float kThemePreviewSpacing = 4.0F;
inline constexpr float kThemeModeCardHeight = 48.0F;
inline constexpr float kThemeModeIconSize = 20.0F;
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

inline huxerui::View ThemeModeCardSurface(const huxerui::ThemeSpec& theme,
    huxerui::ImageResource symbol, bool selected, std::string label, std::function<void()> onClick) {
    const auto colors = ResolveSelectableTileColors(theme, selected);
    return huxerui::Row {
      huxerui::Image(symbol).Tint(colors.fg)
          .With(huxerui::Frame{.width = kThemeModeIconSize, .height = kThemeModeIconSize})
          .Key("theme-mode-symbol"),
      FittedSingleLineLabel(label, huxerui::TextStyle{
          huxerui::Font::System(theme.typography.body_small), colors.fg})
          .With(huxerui::Grow(1.0F)).Key("theme-mode-label"),
    }.With(huxerui::Frame{.height = kThemeModeCardHeight}, huxerui::Grow(1.0F),
        huxerui::Spacing(theme.spacing.small), huxerui::Padding(theme.spacing.small),
        huxerui::Background(colors.surface), huxerui::CornerRadius(theme.shapes.medium),
        huxerui::Border(selected ? theme.colors.primary : huxerui::Color::Transparent(),
            selected ? kCardSelectedOutlineWidth : 0.0F), huxerui::ClipChildren(),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
        theme.interactions.indication, huxerui::Focusable(true),
        huxerui::Semantics{.role = huxerui::SemanticRole::Button, .label = std::move(label), .selected = selected})
        .OnClick(std::move(onClick));
}

// 用真实候选主题 tokens 绘制微型界面；内部仅展示，不创建可交互控件。
inline huxerui::View ThemeColorCardSurface(const huxerui::ThemeSpec& theme,
    huxerui::Color fill, huxerui::Color foreground, std::optional<huxerui::ImageResource> symbol,
    bool selected, std::string label, std::function<void()> onClick,
    float edge = kThemeColorCardEdge,
    std::string accentId = {}) {
    auto candidate = theme;
    if (!accentId.empty()) ApplyFluxAccent(candidate.colors, IsDarkTheme(theme), accentId);
    const auto islands = ResolveIslandTheme(candidate);
    const auto bar = [](huxerui::Color color, float width) {
        return huxerui::Row{}.With(huxerui::Frame{.width = width, .height = kThemePreviewLineHeight},
            huxerui::Background(color), huxerui::CornerRadius(kThemePreviewLineHeight / 2));
    };
    huxerui::View content;
    if (accentId.empty()) {
        content = huxerui::Column {
          symbol ? huxerui::Image(*symbol).Tint(theme.colors.on_surface_variant)
              .With(huxerui::Frame{.width = theme.spacing.extra_large, .height = theme.spacing.extra_large})
              .Key("theme-color-symbol") : huxerui::View{},
        }.With(huxerui::Grow(1.0F), huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    } else {
        content = huxerui::Column {
          huxerui::Row {
            bar(candidate.colors.primary, edge * 0.25F), huxerui::Spacer(),
            selected && symbol ? huxerui::Image(*symbol).Tint(candidate.colors.primary)
                .With(huxerui::Frame{.width = kThemeModeIconSize, .height = kThemeModeIconSize})
                .Key("theme-color-symbol") : huxerui::View{},
          }
              .With(huxerui::Frame{.height = kThemeModeIconSize + 2 * kThemePreviewSpacing},
                  huxerui::Padding(kThemePreviewSpacing), huxerui::Background(candidate.colors.background),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
          huxerui::Row {
            huxerui::Column {bar(candidate.colors.primary, edge * 0.08F)}
                .With(huxerui::Padding(kThemePreviewSpacing), huxerui::Background(candidate.colors.background)),
            huxerui::Column {
              bar(candidate.colors.on_surface_variant, edge * 0.3F),
              huxerui::Row {bar(candidate.colors.on_primary_container, edge * 0.2F)}
                  .With(huxerui::Padding(kThemePreviewSpacing),
                      huxerui::Background(candidate.colors.primary_container),
                      huxerui::CornerRadius(theme.shapes.small)).Key("theme-preview-selection"),
              huxerui::Row {bar(foreground, edge * 0.12F)}
                  .With(huxerui::Padding(kThemePreviewSpacing), huxerui::Background(fill),
                      huxerui::CornerRadius(theme.shapes.small)).Key("theme-preview-button"),
            }.With(huxerui::Grow(1.0F), huxerui::Spacing(kThemePreviewSpacing),
                huxerui::Padding(kThemePreviewSpacing), huxerui::Background(islands.base),
                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
          }.With(huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
        }.With(huxerui::Grow(1.0F), huxerui::ClipChildren(),
            huxerui::CornerRadius(theme.shapes.small),
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)).Key("theme-color-preview");
    }
    huxerui::View card = huxerui::Column {content}.With(
        huxerui::Frame{.width = edge, .height = edge * kThemeColorCardHeightRatio},
        huxerui::Spacing(theme.spacing.small),
        huxerui::Padding(theme.spacing.small), huxerui::Background(ResolveIslandTheme(theme).raised),
        huxerui::CornerRadius(theme.shapes.medium), huxerui::ClipChildren(),
        huxerui::Border(ResolveIslandTheme(theme).outline_soft, kCardOutlineWidth),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
        theme.interactions.indication, huxerui::Focusable(true),
        huxerui::Semantics{.role = huxerui::SemanticRole::Button, .label = std::move(label), .selected = selected});
    if (selected) card = std::move(card).With(huxerui::Border{theme.colors.primary, kCardStrongOutlineWidth});
    return std::move(card).OnClick(std::move(onClick));
}

} // namespace clashflux::ui
