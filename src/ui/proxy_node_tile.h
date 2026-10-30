#pragma once

#include <huxerui/huxerui.h>
#include "ui.h"
#include "refresh_motion.h"

namespace clashflux::ui {
inline constexpr float kProxyDelaySlotWidth = 62.0F;
inline constexpr float kProxyDelaySlotHeight = 16.0F;
inline constexpr float kProxyProbeActionSize = 16.0F;

// The same delay slot remains mounted while its text, action and progress change.
inline huxerui::View ProxyNodeTile(huxerui::View title, huxerui::View detail,
    huxerui::View delay, bool selected, std::string label, std::function<void()> activate,
    SelectableTileInteraction interaction, bool canProbe, bool testing, bool blocked,
    std::function<void()> startProbe) {
    return huxerui::Scope([=] {
        auto hovered = huxerui::UseState(false);
        auto animationReady = huxerui::UseState(false);
        huxerui::Lifecycle([animationReady] { animationReady = true; });
        const auto& theme = huxerui::UseTheme();
        auto style = huxerui::UseEnvironment<huxerui::IconButtonStyle>();
        const auto colors = ResolveSelectableTileColors(theme, selected);
        const auto probeLabel = huxerui::UseString(Localized("测速")) + ": " + label;
        const bool showAction = canProbe && hovered.Get();
        const bool showButton = showAction || (canProbe && testing);
        huxerui::View slot = huxerui::Row{delay}.With(
            huxerui::Frame{.width = kProxyDelaySlotWidth, .height = kProxyDelaySlotHeight},
            huxerui::MainAlign(huxerui::MainAxisAlignment::End),
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
            huxerui::Opacity(showButton ? 0.0F : 1.0F));
        if (canProbe) {
            style.icon_size = 14.0F;
            style.minimum_interactive_size = kProxyProbeActionSize;
            style.state_layer_size = kProxyProbeActionSize;
            style.corner_radius = kProxyProbeActionSize / 2;
            style.foreground = colors.fg;
            style.indication = ResolveFluxIndication(theme);
            auto action = huxerui::IconButton(huxerui::ImageResource{"app", "images/refresh"}, probeLabel).With(
                huxerui::Frame{.width = kProxyProbeActionSize, .height = kProxyProbeActionSize},
                huxerui::Rotation(RefreshIconRotation(theme, testing && animationReady.Get())),
                huxerui::Opacity(showButton ? 1.0F : 0.0F),
                huxerui::Tooltip(Localized("测速")),
                huxerui::Enabled(!blocked),
                huxerui::Semantics{.role = huxerui::SemanticRole::Button, .label = probeLabel,
                    .identifier = "node-probe:" + label, .busy = testing})
                .OnClick([startProbe, testing] { if (!testing) startProbe(); }).Key("node-probe-action");
            slot = huxerui::Stack{
                std::move(slot),
                huxerui::ProvideEnvironment(style, std::move(action)),
            }.With(huxerui::Frame{.width = kProxyDelaySlotWidth, .height = kProxyDelaySlotHeight},
                huxerui::Align(huxerui::HorizontalAlignment::End, huxerui::VerticalAlignment::Center));
        }
        slot = std::move(slot).Key("node-probe-slot");
        return SelectableTile(
            huxerui::Column{
                title,
                huxerui::Row{detail, huxerui::Spacer(), std::move(slot)}
                    .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::End)),
            }.With(huxerui::Spacing(2.0F),
                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
            selected, label, activate, interaction)
            .On<huxerui::ViewEvents::Hover>([hovered](const huxerui::HoverEvent& event) {
                hovered = event.type != huxerui::HoverEventType::Leave;
            });
    });
}
} // namespace clashflux::ui
