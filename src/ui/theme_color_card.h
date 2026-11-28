#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <huxerui/huxerui.h>

#include "card_hover_outline.h"
#include "theme_colors.h"
#include "ui.h"

namespace clashflux::ui {

// 页面特有的几何；间距、圆角、图标从 ThemeSpec 取值，描边使用统一卡片主题常量。
inline constexpr int kThemePreviewWideRatioWidth = 16;
inline constexpr int kThemePreviewWideRatioHeight = 9;
inline constexpr int kThemePreviewCompactRatioWidth = 9;
inline constexpr int kThemePreviewCompactRatioHeight = 16;
inline constexpr float kThemePreviewCompactMaxHeight = 384.0F;
inline constexpr float kThemePreviewTopBarHeight = 28.0F;
inline constexpr float kThemePreviewBottomNavHeight = 28.0F;
inline constexpr float kThemeColorCardEdge = 52.0F;
inline constexpr float kThemeColorCompactCardEdge = 48.0F;
inline constexpr float kThemeColorSwatchInset = 4.0F;
inline constexpr float kThemePreviewLineHeight = 4.0F;
inline constexpr float kThemePreviewSpacing = 4.0F;
inline constexpr float kThemeModeCardHeight = 48.0F;
inline constexpr float kThemeModeCardWidth = 160.0F;
inline constexpr float kThemeModeCompactCardWidth = 96.0F;
inline constexpr float kThemeModeIconSize = 20.0F;
inline constexpr float kThemeColorDialogWidth = 280.0F;
inline constexpr float kThemeColorPreviewHeight = 48.0F;
inline constexpr float kThemeColorSliderTrackRadiusFactor = 0.5F;
inline constexpr float kThemeColorSliderThumbInsetFactor = 0.5F;

inline void RunThemeModeReveal(const huxerui::SceneTransitionHandle& transition, double duration,
    std::function<void()> mutation) {
    // Ease in so the first visible frames show the click point before the circle fills the window.
    const huxerui::TransitionSpec reveal{
        huxerui::CircularRevealTransition{},
        huxerui::TweenSpec{.duration = duration, .easing = huxerui::Easing::EaseInOut}};
    transition.RunFromCurrentInteraction(reveal, std::move(mutation));
}

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

inline huxerui::View WithThemeCardOutline(huxerui::View card, const huxerui::ThemeSpec& theme,
    bool selected, std::function<void()> onClick) {
    return huxerui::Scope([card = std::move(card), theme, selected, onClick = std::move(onClick)] {
        const auto& gradient = huxerui::UseEnvironment<FluxGradientStyle>();
        const auto hovered = huxerui::UseState(false);
        if (gradient.enabled) {
            return WithSelectableCardHover(card, theme, hovered, theme.shapes.medium, selected, gradient)
                .OnClick(onClick);
        }
        return huxerui::View{card}.With(
            huxerui::Border(theme.colors.primary, selected ? kCardOutlineWidth : 0.0F),
            ResolveCardIndication(theme, selected)).OnClick(onClick);
    });
}

inline huxerui::View ThemeModeCardSurface(const huxerui::ThemeSpec& theme,
    huxerui::ImageResource symbol, bool selected, std::string label,
    std::function<void()> onClick, float width = kThemeModeCardWidth) {
    const auto colors = ResolveSelectableTileColors(theme, selected);
    auto card = huxerui::Row {
      huxerui::Image(symbol).Tint(colors.fg)
          .With(huxerui::Frame{.width = kThemeModeIconSize, .height = kThemeModeIconSize})
          .Key("theme-mode-symbol"),
      FittedSingleLineLabel(label, huxerui::TextStyle{
          huxerui::Font::System(theme.typography.body_small), colors.fg})
          .With(huxerui::Grow(1.0F)).Key("theme-mode-label"),
    }.With(huxerui::Frame{.width = width, .height = kThemeModeCardHeight},
        huxerui::Spacing(theme.spacing.small), huxerui::Padding(theme.spacing.small),
        huxerui::Background(colors.surface), huxerui::CornerRadius(theme.shapes.medium),
        huxerui::ClipChildren(),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
        huxerui::Focusable(true),
        huxerui::Semantics{.role = huxerui::SemanticRole::Button, .label = std::move(label), .selected = selected});
    return WithThemeCardOutline(std::move(card), theme, selected, std::move(onClick));
}

template <int WidthRatio, int HeightRatio, int MaxHeightUnits = 0>
class ThemePreviewAspectBox final
    : public huxerui::Layout<ThemePreviewAspectBox<WidthRatio, HeightRatio, MaxHeightUnits>> {
    using Base = huxerui::Layout<ThemePreviewAspectBox<WidthRatio, HeightRatio, MaxHeightUnits>>;
public:
    using Base::Base;

    static huxerui::LayoutResult Measure(huxerui::LayoutContext& context,
                                         huxerui::ViewNode& node,
                                         huxerui::Constraints constraints) {
        constexpr float kRatio = static_cast<float>(WidthRatio) / static_cast<float>(HeightRatio);
        constexpr float kMaxHeight = static_cast<float>(MaxHeightUnits);
        const float boxWidth = std::isfinite(constraints.max_width)
            ? constraints.max_width
            : std::max(constraints.min_width, 720.0F);
        float height = boxWidth / kRatio;
        if (kMaxHeight > 0.0F && height > kMaxHeight) height = kMaxHeight;
        if (std::isfinite(constraints.max_height) && height > constraints.max_height)
            height = constraints.max_height;
        const huxerui::Size measured = constraints.Constrain({boxWidth, height});
        height = measured.height;
        const float childWidth = std::min(measured.width, height * kRatio);
        const float offsetX = std::max(0.0F, (measured.width - childWidth) * 0.5F);
        const huxerui::Constraints childConstraints{
            .min_width = childWidth,
            .max_width = childWidth,
            .min_height = height,
            .max_height = height,
        };
        huxerui::LayoutResult result;
        for (huxerui::ViewNode& child : node.Children()) {
            static_cast<void>(context.Measure(child, childConstraints));
            result.Place(child, {offsetX, 0.0F});
        }
        return result.SetSize(measured);
    }
};

// 顶部实时界面预览板：按宽屏 16:9 / 窄屏 9:16 比例微缩呈现首页布局，随下方设置同步重绘。
inline huxerui::View ThemePreviewSurface(const huxerui::ThemeSpec& theme,
    const std::optional<FluxAccent>& accent = std::nullopt, bool compact = false) {
    auto candidate = theme;
    const bool dark = IsDarkTheme(theme);
    const FluxGradientStyle candidateGradient = accent
        ? ResolveFluxGradientStyle(*accent) : FluxGradientStyle{};
    const bool multiColor = candidateGradient.enabled;
    if (accent) candidate.colors = dark ? FluxDarkColors(*accent) : FluxLightColors(*accent);
    const auto islands = ResolveIslandTheme(candidate);
    const std::optional<FluxMultiColorPalette> palette = multiColor
        ? std::optional{ResolveGradientPalette(candidateGradient)} : std::nullopt;
    const huxerui::VisualFill selectionFill = palette
        ? huxerui::VisualFill{MultiColorSelectionGradient(*palette, dark)}
        : huxerui::VisualFill{candidate.colors.primary_container};
    const huxerui::VisualFill buttonFill = multiColor
        ? ResolveFluxPrimaryFill(candidate, candidateGradient) : huxerui::VisualFill{candidate.colors.primary};
    const auto buttonForeground = multiColor
        ? ResolveFluxPrimaryForeground(candidate, candidateGradient) : candidate.colors.on_primary;
    const huxerui::VisualFill accentBarFill = palette
        ? huxerui::VisualFill{MultiColorLinearGradient(*palette, candidate.brightness)}
        : huxerui::VisualFill{candidate.colors.primary};
    const auto traffic = ResolveTrafficColors(candidate, multiColor);
    const auto bar = [](huxerui::VisualFill barFill, float width, float height = kThemePreviewLineHeight) {
        return huxerui::Row{}.With(huxerui::Frame{.width = width, .height = height},
            huxerui::Background(std::move(barFill)), huxerui::CornerRadius(height * 0.5F));
    };
    const auto stretchBar = [](huxerui::VisualFill barFill, float height = kThemePreviewLineHeight) {
        return huxerui::Row{}.With(huxerui::Frame{.height = height},
            huxerui::Background(std::move(barFill)), huxerui::CornerRadius(height * 0.5F));
    };
    const auto dot = [](huxerui::VisualFill dotFill, float size = 6.0F) {
        return huxerui::Row{}.With(huxerui::Frame{.width = size, .height = size},
            huxerui::Background(std::move(dotFill)), huxerui::CornerRadius(size * 0.5F));
    };
    const auto miniCard = [&](std::vector<huxerui::View> children, float padding, float spacing,
                              bool activeOutline, float grow = 1.0F) {
        auto view = huxerui::Column(std::move(children)).With(
            huxerui::Grow(grow), huxerui::Padding(padding), huxerui::Spacing(spacing),
            huxerui::Background(islands.raised), huxerui::CornerRadius(theme.shapes.small),
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
        if (activeOutline) {
            return std::move(view).With(FourColorOverlayOutline{
                .active = true,
                .fourColor = multiColor,
                .brightness = candidate.brightness,
                .fallbackColor = candidate.colors.primary,
                .corners = huxerui::CornerRadii{theme.shapes.small},
                .width = kCardOutlineWidth,
                .palette = palette,
            });
        }
        return std::move(view).With(huxerui::Border(candidate.colors.outline, kCardOutlineWidth));
    };
    huxerui::View window;
    if (compact) {
        std::vector<huxerui::View> bottomTabs;
        bottomTabs.push_back(
            huxerui::Row {bar(accentBarFill, 16.0F)}.With(
                huxerui::Grow(1.0F), huxerui::Frame{.height = 20.0F},
                huxerui::Background(CompactNavigationIndicatorColor(candidate)),
                huxerui::CornerRadius(10.0F),
                huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
        for (int tab = 1; tab < 4; ++tab) {
            bottomTabs.push_back(
                huxerui::Row {bar(candidate.colors.outline, 14.0F)}.With(
                    huxerui::Grow(1.0F), huxerui::Frame{.height = 20.0F},
                    huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                    huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
        }
        huxerui::View downloadCard = miniCard({
            bar(candidate.colors.on_surface_variant, 28.0F),
            bar(traffic.download, 38.0F, 5.0F),
            huxerui::Spacer(),
            stretchBar(accentBarFill),
        }, 7.0F, 5.0F, true).Key("theme-preview-card-primary");
        huxerui::View uploadCard = miniCard({
            bar(candidate.colors.on_surface_variant, 28.0F),
            bar(traffic.upload, 34.0F, 5.0F),
            huxerui::Spacer(),
            stretchBar(traffic.upload),
        }, 7.0F, 5.0F, false).Key("theme-preview-card-secondary");
        huxerui::View totalCard = miniCard({
            bar(candidate.colors.on_surface_variant, 32.0F),
            huxerui::Spacer(),
            huxerui::Row {dot(traffic.upload, 5.0F), bar(candidate.colors.on_surface, 32.0F)}.With(
                huxerui::Spacing(4.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
            huxerui::Row {dot(traffic.download, 5.0F), bar(candidate.colors.on_surface, 38.0F)}.With(
                huxerui::Spacing(4.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        }, 7.0F, 4.0F, false).Key("theme-preview-home-total");
        huxerui::View modeCard = miniCard({
            bar(candidate.colors.on_surface_variant, 32.0F),
            huxerui::Row {
                dot(accentBarFill, 4.0F),
                bar(candidate.colors.on_primary_container, 28.0F),
            }.With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(6.0F, 4.0F)),
                huxerui::Spacing(4.0F), huxerui::Background(selectionFill),
                huxerui::CornerRadius(theme.shapes.small),
                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)).Key("theme-preview-selection"),
            bar(candidate.colors.outline, 24.0F),
            bar(candidate.colors.outline, 20.0F),
        }, 7.0F, 4.0F, false);
        huxerui::View profileCard = miniCard({
            bar(candidate.colors.on_surface_variant, 30.0F),
            bar(candidate.colors.on_surface, 40.0F),
            stretchBar(accentBarFill, 3.0F),
            huxerui::Spacer(),
            huxerui::Row {bar(candidate.colors.on_secondary_container, 22.0F)}.With(
                huxerui::Frame{.height = 10.0F},
                huxerui::Padding(huxerui::EdgeInsets::Symmetric(5.0F, 0.0F)),
                huxerui::Background(candidate.colors.secondary_container),
                huxerui::CornerRadius(theme.shapes.small),
                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        }, 7.0F, 4.0F, false);
        huxerui::View lanCard = miniCard({
            bar(candidate.colors.on_surface_variant, 24.0F),
            huxerui::Spacer(),
            bar(candidate.colors.on_surface, 34.0F),
        }, 6.0F, 3.0F, false);
        huxerui::View netCard = miniCard({
            bar(candidate.colors.on_surface_variant, 24.0F),
            huxerui::Spacer(),
            huxerui::Row {dot(accentBarFill, 4.0F), bar(candidate.colors.on_surface, 30.0F)}.With(
                huxerui::Spacing(4.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        }, 6.0F, 3.0F, false);
        window = huxerui::Column {
          huxerui::Row {
            bar(candidate.colors.on_surface, 34.0F, 5.0F),
            bar(accentBarFill, 22.0F),
            huxerui::Spacer(),
            huxerui::Row {bar(buttonForeground, 22.0F)}
                .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 5.0F)),
                    huxerui::Background(buttonFill),
                    huxerui::CornerRadius(theme.shapes.small)).Key("theme-preview-button"),
          }.With(huxerui::Frame{.height = 34.0F},
              huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 5.0F)),
              huxerui::Spacing(6.0F), huxerui::Background(candidate.colors.background),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
          huxerui::Column {
            huxerui::Row {std::move(downloadCard), std::move(uploadCard)}.With(
                huxerui::Grow(1.0F), huxerui::Spacing(6.0F),
                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
            huxerui::Row {std::move(totalCard), std::move(modeCard)}.With(
                huxerui::Grow(1.15F), huxerui::Spacing(6.0F),
                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
            huxerui::Row {
              std::move(profileCard),
              huxerui::Column {std::move(lanCard), std::move(netCard)}.With(
                  huxerui::Grow(1.0F), huxerui::Spacing(5.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
            }.With(huxerui::Grow(1.05F), huxerui::Spacing(6.0F),
                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
          }.With(huxerui::Grow(1.0F), huxerui::Padding(8.0F),
              huxerui::Spacing(6.0F), huxerui::Background(islands.base),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
          huxerui::Row {
            huxerui::Row(std::move(bottomTabs)).With(
                huxerui::Grow(1.0F),
                huxerui::Frame{.height = kThemePreviewBottomNavHeight},
                huxerui::Padding(huxerui::EdgeInsets::Symmetric(6.0F, 4.0F)),
                huxerui::Spacing(4.0F),
                huxerui::Background(CompactNavigationSurfaceColor(candidate)),
                huxerui::Border(CompactNavigationBorderColor(candidate), kCardOutlineWidth),
                huxerui::CornerRadius(kThemePreviewBottomNavHeight * 0.5F),
                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)).Key("theme-preview-bottom-nav"),
          }.With(huxerui::Padding(huxerui::EdgeInsets{.right = 10.0F, .bottom = 8.0F, .left = 10.0F}),
              huxerui::Background(islands.base),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
        }.With(huxerui::Grow(1.0F), huxerui::Background(islands.base),
            huxerui::CornerRadius(theme.shapes.small), huxerui::ClipChildren(),
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    } else {
        std::vector<huxerui::View> railItems;
        railItems.push_back(
            huxerui::Row {dot(accentBarFill, 6.0F)}.With(
                huxerui::Frame{.height = 14.0F},
                huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
        railItems.push_back(
            huxerui::Row {bar(accentBarFill, 18.0F)}.With(
                huxerui::Frame{.height = 22.0F}, huxerui::Background(selectionFill),
                huxerui::CornerRadius(theme.shapes.small),
                huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
        for (int item = 0; item < 4; ++item) {
            railItems.push_back(
                huxerui::Row {bar(candidate.colors.outline, 18.0F)}.With(
                    huxerui::Frame{.height = 18.0F},
                    huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                    huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
        }
        railItems.push_back(huxerui::Spacer());
        railItems.push_back(
            huxerui::Row {bar(candidate.colors.outline, 16.0F)}.With(
                huxerui::Frame{.height = 18.0F},
                huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
        const auto togglePill = [&](bool active) {
            return huxerui::Row {
                dot(active ? candidate.colors.on_primary : candidate.colors.on_surface_variant, 8.0F),
            }.With(huxerui::Frame{.width = 24.0F, .height = 12.0F},
                huxerui::Padding(huxerui::EdgeInsets::Symmetric(2.0F, 0.0F)),
                huxerui::Background(active ? huxerui::VisualFill{candidate.colors.primary}
                                           : huxerui::VisualFill{candidate.colors.secondary_container}),
                huxerui::CornerRadius(6.0F),
                huxerui::MainAlign(active ? huxerui::MainAxisAlignment::End : huxerui::MainAxisAlignment::Start),
                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
        };
        huxerui::View downloadCard = miniCard({
            bar(candidate.colors.on_surface_variant, 42.0F),
            bar(traffic.download, 58.0F, 6.0F),
            huxerui::Spacer(),
            stretchBar(accentBarFill),
        }, 10.0F, 6.0F, true).Key("theme-preview-card-primary");
        huxerui::View uploadCard = miniCard({
            bar(candidate.colors.on_surface_variant, 42.0F),
            bar(traffic.upload, 52.0F, 6.0F),
            huxerui::Spacer(),
            stretchBar(traffic.upload),
        }, 10.0F, 6.0F, false).Key("theme-preview-card-secondary");
        huxerui::View totalCard = miniCard({
            bar(candidate.colors.on_surface_variant, 48.0F),
            huxerui::Spacer(),
            huxerui::Row {dot(traffic.upload, 5.0F), bar(candidate.colors.on_surface, 56.0F)}.With(
                huxerui::Spacing(6.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
            huxerui::Row {dot(traffic.download, 5.0F), bar(candidate.colors.on_surface, 64.0F)}.With(
                huxerui::Spacing(6.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        }, 10.0F, 6.0F, false).Key("theme-preview-home-total");
        huxerui::View modeCard = miniCard({
            bar(candidate.colors.on_surface_variant, 48.0F),
            huxerui::Row {
                dot(accentBarFill, 5.0F),
                bar(candidate.colors.on_primary_container, 46.0F),
            }.With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(8.0F, 6.0F)),
                huxerui::Spacing(6.0F), huxerui::Background(selectionFill),
                huxerui::CornerRadius(theme.shapes.small),
                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)).Key("theme-preview-selection"),
            bar(candidate.colors.outline, 40.0F),
            bar(candidate.colors.outline, 34.0F),
        }, 10.0F, 5.0F, false);
        huxerui::View profileCard = miniCard({
            bar(candidate.colors.on_surface_variant, 48.0F),
            bar(candidate.colors.on_surface, 64.0F),
            stretchBar(accentBarFill, 3.0F),
            huxerui::Spacer(),
            huxerui::Row {
                huxerui::Row {bar(candidate.colors.on_secondary_container, 24.0F)}.With(
                    huxerui::Grow(1.0F), huxerui::Frame{.height = 12.0F},
                    huxerui::Padding(huxerui::EdgeInsets::Symmetric(6.0F, 0.0F)),
                    huxerui::Background(candidate.colors.secondary_container),
                    huxerui::CornerRadius(theme.shapes.small),
                    huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
                huxerui::Row {bar(candidate.colors.on_secondary_container, 20.0F)}.With(
                    huxerui::Grow(1.0F), huxerui::Frame{.height = 12.0F},
                    huxerui::Padding(huxerui::EdgeInsets::Symmetric(6.0F, 0.0F)),
                    huxerui::Background(candidate.colors.secondary_container),
                    huxerui::CornerRadius(theme.shapes.small),
                    huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
            }.With(huxerui::Spacing(6.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        }, 10.0F, 5.0F, false);
        huxerui::View lanCard = miniCard({
            bar(candidate.colors.on_surface_variant, 36.0F),
            huxerui::Spacer(),
            bar(candidate.colors.on_surface, 52.0F),
        }, 8.0F, 4.0F, false);
        huxerui::View netCard = miniCard({
            bar(candidate.colors.on_surface_variant, 36.0F),
            huxerui::Spacer(),
            huxerui::Row {dot(accentBarFill, 5.0F), bar(candidate.colors.on_surface, 44.0F)}.With(
                huxerui::Spacing(5.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        }, 8.0F, 4.0F, false);
        huxerui::View proxyCard = miniCard({
            huxerui::Row {
                bar(candidate.colors.on_surface_variant, 48.0F),
                huxerui::Spacer(),
                togglePill(true),
            }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        }, 8.0F, 0.0F, false);
        huxerui::View tunCard = miniCard({
            huxerui::Row {
                bar(candidate.colors.on_surface_variant, 36.0F),
                huxerui::Spacer(),
                togglePill(false),
            }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        }, 8.0F, 0.0F, false);
        huxerui::View controlCard = miniCard({
            huxerui::Row {
                dot(accentBarFill, 6.0F),
                bar(candidate.colors.on_surface, 36.0F),
                huxerui::Spacer(),
                huxerui::Row {bar(buttonForeground, 28.0F)}
                    .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(12.0F, 6.0F)),
                        huxerui::Background(buttonFill),
                        huxerui::CornerRadius(theme.shapes.small)).Key("theme-preview-button"),
            }.With(huxerui::Spacing(6.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        }, 8.0F, 0.0F, false);
        window = huxerui::Column {
          huxerui::Row {
            bar(candidate.colors.on_surface, 40.0F, 5.0F),
            huxerui::Spacer(),
            dot(candidate.colors.outline),
            dot(candidate.colors.outline),
            dot(candidate.colors.outline),
          }.With(huxerui::Frame{.height = kThemePreviewTopBarHeight},
              huxerui::Padding(huxerui::EdgeInsets::Symmetric(12.0F, 0.0F)),
              huxerui::Spacing(6.0F), huxerui::Background(candidate.colors.background),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
          huxerui::Row {
            huxerui::Column(std::move(railItems)).With(
                huxerui::Frame{.width = 48.0F},
                huxerui::Padding(huxerui::EdgeInsets::Symmetric(8.0F, 10.0F)),
                huxerui::Spacing(8.0F), huxerui::Background(candidate.colors.background),
                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)).Key("theme-preview-rail"),
            huxerui::Column {
              huxerui::Row {std::move(downloadCard), std::move(uploadCard), std::move(totalCard)}.With(
                  huxerui::Grow(1.0F), huxerui::Spacing(8.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
              huxerui::Row {
                std::move(modeCard),
                std::move(profileCard),
                huxerui::Column {std::move(lanCard), std::move(netCard)}.With(
                    huxerui::Grow(1.0F), huxerui::Spacing(8.0F),
                    huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
              }.With(huxerui::Grow(1.05F), huxerui::Spacing(8.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
              huxerui::Row {std::move(proxyCard), std::move(tunCard), std::move(controlCard)}.With(
                  huxerui::Grow(0.62F), huxerui::Spacing(8.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
            }.With(huxerui::Grow(1.0F), huxerui::Padding(12.0F), huxerui::Spacing(8.0F),
                huxerui::Background(islands.base), huxerui::CornerRadius(theme.shapes.small),
                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
          }.With(huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
        }.With(huxerui::Grow(1.0F), huxerui::Background(candidate.colors.background),
            huxerui::CornerRadius(theme.shapes.small), huxerui::ClipChildren(),
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }
    huxerui::View board = huxerui::Column {std::move(window)}.With(
        huxerui::Padding(theme.spacing.small), huxerui::Background(islands.raised),
        huxerui::CornerRadius(theme.shapes.medium),
        huxerui::Border(candidate.colors.outline, kCardOutlineWidth),
        huxerui::ClipChildren(),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)).Key("theme-color-preview");
    if (compact) {
        return ThemePreviewAspectBox<kThemePreviewCompactRatioWidth,
            kThemePreviewCompactRatioHeight,
            static_cast<int>(kThemePreviewCompactMaxHeight)>{std::move(board)};
    }
    return ThemePreviewAspectBox<kThemePreviewWideRatioWidth,
        kThemePreviewWideRatioHeight>{std::move(board)};
}

// 紧凑方形主题色块；多色使用多色径向渐变，单色使用自适应实色，添加入口复用相同尺寸与描边。
inline huxerui::View ThemeColorCardSurface(const huxerui::ThemeSpec& theme,
    huxerui::Color fill, huxerui::Color foreground, std::optional<huxerui::ImageResource> symbol,
    bool selected, std::string label, std::function<void()> onClick,
    float edge = kThemeColorCardEdge,
    std::optional<FluxAccent> accent = std::nullopt) {
    huxerui::View content;
    if (!accent) {
        content = huxerui::Column {
          symbol ? huxerui::Image(*symbol).Tint(theme.colors.on_surface_variant)
              .With(huxerui::Frame{.width = kThemeModeIconSize, .height = kThemeModeIconSize})
              .Key("theme-color-symbol") : huxerui::View{},
        }.With(huxerui::Grow(1.0F), huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    } else {
        const bool dark = IsDarkTheme(theme);
        const FluxGradientStyle candidateGradient = ResolveFluxGradientStyle(*accent);
        const bool multiColor = candidateGradient.enabled;
        huxerui::VisualFill swatchFill{fill};
        huxerui::Color checkTint = foreground;
        if (multiColor) {
            const auto palette = ResolveGradientPalette(candidateGradient);
            const auto base = dark ? palette.darkBase : palette.lightBase;
            const auto& stops = dark ? palette.darkStops : palette.lightStops;
            swatchFill = MakeMultiColorGradient(base, stops);
            checkTint = ThemeColorLuminance(base) > kThemeColorBlackWhiteSplit
                ? huxerui::Color::Black() : huxerui::Color::White();
        }
        const float innerRadius = std::max(0.0F, theme.shapes.medium - kThemeColorSwatchInset);
        content = huxerui::Column {
          selected && symbol ? huxerui::Image(*symbol).Tint(checkTint)
              .With(huxerui::Frame{.width = kThemeModeIconSize, .height = kThemeModeIconSize})
              .Key("theme-color-symbol") : huxerui::View{},
        }.With(huxerui::Grow(1.0F), huxerui::Background(std::move(swatchFill)),
            huxerui::CornerRadius(innerRadius),
            huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    }
    auto card = huxerui::Column {std::move(content)}.With(
        huxerui::Frame{.width = edge, .height = edge},
        huxerui::Padding(kThemeColorSwatchInset), huxerui::Background(ResolveIslandTheme(theme).raised),
        huxerui::CornerRadius(theme.shapes.medium), huxerui::ClipChildren(),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
        huxerui::Focusable(true),
        huxerui::Semantics{.role = huxerui::SemanticRole::Button, .label = std::move(label), .selected = selected}
    );
    return WithThemeCardOutline(std::move(card), theme, selected, std::move(onClick));
}

struct RemovedThemeColorMixStop {
    std::vector<std::string> stops;
    std::size_t selectedIndex = 0U;
    std::size_t primaryIndex = 0U;
};

inline RemovedThemeColorMixStop RemoveThemeColorMixStop(
    std::vector<std::string> stops, std::size_t removeIndex, std::size_t primaryIndex) {
    if (stops.empty()) return {};
    if (stops.size() <= 1U || removeIndex >= stops.size()) {
        return {std::move(stops), 0U, 0U};
    }
    stops.erase(stops.begin() + static_cast<std::ptrdiff_t>(removeIndex));
    const std::size_t nextSelected = removeIndex >= stops.size() ? stops.size() - 1U : removeIndex;
    const std::size_t nextPrimary = removeIndex == primaryIndex
        ? 0U
        : (removeIndex < primaryIndex ? primaryIndex - 1U : primaryIndex);
    return {std::move(stops), nextSelected, std::min(nextPrimary, stops.size() - 1U)};
}

inline void ApplyThemeColorMixStopSelection(
    huxerui::State<std::vector<std::string>> stops,
    huxerui::State<std::size_t> activeIndex,
    huxerui::State<huxerui::TextEditingValue> input,
    huxerui::State<HsbColor> hsb,
    std::size_t nextIndex,
    std::vector<std::string> nextStops) {
    if (nextStops.empty()) return;
    nextIndex = std::min(nextIndex, nextStops.size() - 1U);
    const std::string nextHex = nextStops[nextIndex];
    stops = std::move(nextStops);
    activeIndex = nextIndex;
    input = huxerui::TextEditingValue{nextHex};
    hsb = ThemeColorToHsb(ThemeColorFromHex(nextHex));
}

inline huxerui::View ThemeColorMixSwatch(
    const huxerui::ThemeSpec& theme, std::size_t index, huxerui::Color swatchColor,
    bool current, bool showPrimaryDot, std::function<void()> onClick) {
    const auto dotColor = ThemeColorLuminance(swatchColor) > kThemeColorBlackWhiteSplit
        ? huxerui::Color::Black() : huxerui::Color::White();
    return huxerui::Row {
        showPrimaryDot ? huxerui::View{huxerui::Row {}
            .With(huxerui::Frame{.width = 8.0F, .height = 8.0F},
                  huxerui::Background(dotColor),
                  huxerui::CornerRadius(4.0F))
            .Key("theme-mix-primary-dot")} : huxerui::View{},
    }
        .With(huxerui::Frame{.width = 32.0F, .height = 28.0F},
              huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
              huxerui::Background(swatchColor),
              huxerui::CornerRadius(theme.shapes.small),
              huxerui::Border(current ? theme.colors.on_surface : theme.colors.outline,
                              current ? 2.0F : 1.0F),
              huxerui::Focusable(true),
              huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                 .label = LocalizedFormat("混合色 {}", index + 1U),
                                 .selected = current})
        .OnClick(std::move(onClick))
        .Key("theme-mix-stop-" + std::to_string(index));
}

inline huxerui::View ThemeColorPrimarySelectorRow(
    const huxerui::ThemeSpec& theme, huxerui::Color primaryColor,
    bool selectedIsPrimary, std::function<void()> onSetPrimary) {
    return huxerui::Row {
        huxerui::Row {}.With(
            huxerui::Frame{.width = 12.0F, .height = 12.0F},
            huxerui::Background(primaryColor),
            huxerui::CornerRadius(6.0F),
            huxerui::Border(theme.colors.outline, 1.0F)),
        huxerui::Text(Localized("主色（开关与单色控件）"))
            .Style({huxerui::Font::System(font_size::kCaption), theme.colors.on_surface_variant})
            .With(huxerui::Grow(1.0F)),
        huxerui::Button(Localized(selectedIsPrimary ? "当前主色" : "设为主色"))
            .With(huxerui::Enabled(!selectedIsPrimary))
            .OnClick(std::move(onSetPrimary))
            .Key("theme-mix-set-primary"),
    }.With(huxerui::Spacing(theme.spacing.small),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

} // namespace clashflux::ui
