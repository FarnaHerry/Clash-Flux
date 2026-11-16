#pragma once

#include <huxerui/huxerui.h>
#include "ui.h"
#include "refresh_motion.h"

namespace clashflux::ui {
inline constexpr float kProxyDelaySlotWidth = 62.0F;
inline constexpr float kProxyDelaySlotHeight = 16.0F;
inline constexpr float kProxyProbeActionSize = 16.0F;

inline huxerui::View ProxyNodeTile(huxerui::View title, huxerui::View detail,
    huxerui::View delay, bool selected, std::string label, std::function<void()> activate,
    SelectableTileInteraction interaction, bool canProbe, bool testing, bool blocked,
    std::function<void()> startProbe) {
    return huxerui::Scope([=] {
        auto hovered = huxerui::UseState(false);
        auto buttonFocused = huxerui::UseState(false);
        auto animationReady = huxerui::UseState(false);
        huxerui::Lifecycle([animationReady] { animationReady = true; });
        const auto& theme = huxerui::UseTheme();
        auto style = huxerui::UseEnvironment<huxerui::IconButtonStyle>();
        const auto colors = ResolveSelectableTileColors(theme, selected);
        const auto probeLabel = huxerui::UseString(Localized("测速")) + ": " + label;
        const bool showButton = canProbe && (!delay || testing || hovered.Get() || buttonFocused.Get());
        huxerui::View action;
        if (canProbe) {
            style.icon_size = 14.0F;
            style.minimum_interactive_size = kProxyProbeActionSize;
            style.state_layer_size = kProxyProbeActionSize;
            style.corner_radius = kProxyProbeActionSize / 2;
            style.foreground = showButton ? colors.fg : huxerui::Color::Transparent();
            if (!showButton) style.disabled_foreground = huxerui::Color::Transparent();
            style.indication = ResolveFluxIndication(theme);
            auto button = huxerui::IconButton(huxerui::ImageResource{"app", "images/refresh"}, probeLabel).With(
                huxerui::Frame{.width = kProxyProbeActionSize, .height = kProxyProbeActionSize},
                huxerui::Rotation(RefreshIconRotation(theme, testing && animationReady.Get())),
                huxerui::Tooltip(Localized("测速")),
                huxerui::Enabled(!blocked),
                huxerui::Semantics{.role = huxerui::SemanticRole::Button, .label = probeLabel,
                    .identifier = "node-probe:" + label, .busy = testing})
                .OnClick([startProbe, testing] { if (!testing) startProbe(); })
                .On<huxerui::ViewEvents::FocusChanged>([buttonFocused, hovered](bool focused) {
                    buttonFocused = focused && !hovered.Get();
                }).Key("node-probe-action");
            action = huxerui::Row {
              huxerui::ProvideEnvironment(style, button),
            }.With(huxerui::MainAlign(huxerui::MainAxisAlignment::End));
        }
        return SelectableTile(
            huxerui::Row {
              huxerui::Column {
                title,
                detail,
              }.With(huxerui::Grow(1.0F), huxerui::Spacing(2.0F),
                     huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
              huxerui::Stack {
                action,
                showButton ? huxerui::View{} : delay,
              }.With(huxerui::Frame{.width = kProxyDelaySlotWidth, .min_height = kProxyDelaySlotHeight},
                     huxerui::Align(huxerui::HorizontalAlignment::End, huxerui::VerticalAlignment::Center))
                  .Key("node-probe-slot"),
            }.With(huxerui::Spacing(4.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
            selected, label, activate, interaction)
            .On<huxerui::ViewEvents::Hover>([hovered](const huxerui::HoverEvent& event) {
                hovered = event.type != huxerui::HoverEventType::Leave;
            });
    });
}
} // namespace clashflux::ui
