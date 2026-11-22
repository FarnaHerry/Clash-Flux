#pragma once

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <huxerui/huxerui.h>

#include "card_hover_outline.h"
#include "ui.h"

namespace clashflux::ui {

inline constexpr double kHomeCardRevealDuration = 0.20;

inline huxerui::IconButton HomeCardLeadingIconButton(
    huxerui::ImageVariant icon, huxerui::StringVariant label, const huxerui::IconButtonStyle& style) {
    // Align the glyph with the heading by moving the entire button together with its interaction layer.
    const float inset = std::max(0.0F, (style.minimum_interactive_size - style.icon_size) * 0.5F);
    return huxerui::IconButton(std::move(icon), std::move(label))
        .With(huxerui::Offset(huxerui::Point{-inset, 0.0F}));
}

inline huxerui::View HomeTrafficCardLayout(huxerui::View heading, huxerui::View rate,
    huxerui::View chart, float cardInset) {
    huxerui::View plot;
    if (chart) {
        // Lib-Charts reserves left/right/bottom gutters even with both axes hidden.
        plot = huxerui::Column {std::move(chart).With(huxerui::Grow(1.0F))}.With(
            huxerui::Padding(huxerui::EdgeInsets{.right = -(cardInset + 8.0F),
                .bottom = -(cardInset + 4.0F), .left = -(cardInset + 4.0F)}),
            huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }
    return huxerui::Column {
      std::move(heading),
      std::move(rate),
      plot,
    }.With(huxerui::Spacing(6.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// Build the dashboard surface on its own node so its fill and hover layer share
// the same rounded geometry. A passive card does not acquire a click action.
inline huxerui::View HomeCardSurface(const huxerui::ThemeSpec& theme,
                                    huxerui::View content, std::string cardId,
                                    huxerui::ViewFactory makeBack = {},
                                    huxerui::State<std::string> expandedPicker = {}) {
    return huxerui::Scope([theme, content = std::move(content), cardId, makeBack = std::move(makeBack),
                           expandedPicker]() -> huxerui::View {
        auto back = huxerui::UseState(false);
        auto backContent = huxerui::UseState(huxerui::View{});
        auto hovered = huxerui::UseState(false);
        const auto tasks = huxerui::UseTaskScope();
        const bool showingBack = back.Get();
        const bool hasBackContent = static_cast<bool>(backContent.Get());
        const bool reducedMotion = theme.motion.reduced_motion;
        const auto turn = [back, backContent, makeBack, reducedMotion](const huxerui::MultiTapEvent&) {
            const bool reveal = !back.Get();
            if (reveal) backContent = makeBack();
            else if (reducedMotion) backContent = huxerui::View{};
            back = reveal;
        };
        huxerui::Lifecycle([tasks, back, backContent, showingBack, hasBackContent, reducedMotion] {
            huxerui::TaskHandle retirement;
            if (!showingBack && hasBackContent) {
                if (reducedMotion) {
                    backContent = huxerui::View{};
                } else {
                    retirement = tasks.Launch([back, backContent]() -> huxerui::Task<void> {
                        co_await huxerui::Delay(std::chrono::duration<double>{kHomeCardRevealDuration});
                        if (!back.Get()) backContent = huxerui::View{};
                    });
                }
            }
            return [retirement] { retirement.Cancel(); };
        }, showingBack, hasBackContent, reducedMotion);
        const huxerui::AnimationSpec faceMotion = reducedMotion
            ? huxerui::AnimationSpec{huxerui::SnapSpec{}}
            : huxerui::AnimationSpec{huxerui::TweenSpec{kHomeCardRevealDuration, huxerui::Easing::EaseOut}};
        const auto islands = ResolveIslandTheme(theme);
        const auto hoverStyle = ResolveCardIndication(theme).hover.value();
        const bool pickerOpen = expandedPicker.IsValid() && !expandedPicker.Get().empty();
        const bool showOutline = hovered.Get() || pickerOpen;
        huxerui::View faceContent = content;
        if (makeBack) {
            const auto frontFace = huxerui::Column {content}.With(huxerui::Grow(1.0F),
                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                huxerui::Transition{huxerui::AnimateTo(showingBack ? 0.0F : 1.0F, faceMotion)}
                    .Opacity(0.0F, 1.0F).Scale(0.96F, 1.0F),
                huxerui::Enabled(!showingBack), huxerui::Semantics{.hidden = showingBack})
                .Key("home-card-front");
            const auto backFace = huxerui::Column {backContent.Get()}.With(huxerui::Grow(1.0F),
                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                huxerui::Transition{huxerui::AnimateTo(showingBack ? 1.0F : 0.0F, faceMotion)}
                    .Opacity(0.0F, 1.0F).Scale(0.96F, 1.0F),
                huxerui::Enabled(showingBack), huxerui::Semantics{.hidden = !showingBack})
                .Key("home-card-back-content");
            // Keep the active face above the shield, including during the fade.
            // The back shell stays mounted so its next reveal can animate from zero.
            faceContent = huxerui::Stack {
              showingBack ? frontFace : backFace,
              huxerui::Row {}.With(
                  huxerui::Grow(1.0F), huxerui::MultiTapGesture{},
                  huxerui::PointerCursor(huxerui::PointerCursorKind::Default))
                  .On<huxerui::MultiTapEvents::Recognized>(turn)
                  .Key(showingBack ? "home-card-back" : "home-card-front-shield"),
              showingBack ? backFace : frontFace,
            }.With(huxerui::Grow(1.0F),
                   huxerui::Align(huxerui::HorizontalAlignment::Stretch, huxerui::VerticalAlignment::Stretch));
        }
        huxerui::View surface = huxerui::Column{faceContent}.With(
            huxerui::Padding(islands.island_padding),
            huxerui::Background(islands.raised),
            huxerui::CornerRadius(islands.nested_radius),
            huxerui::ClipChildren(),
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
            CardHoverOutline{hoverStyle.border.value(), huxerui::CornerRadii{islands.nested_radius},
                showOutline ? hoverStyle.enter : hoverStyle.exit, showOutline}
        ).On<huxerui::ViewEvents::Hover>([hovered](const huxerui::HoverEvent& event) {
            // A nested control owns its indication. The card observes containment independently.
            if (event.type != huxerui::HoverEventType::Move) hovered = event.type == huxerui::HoverEventType::Enter;
        });
        if (!makeBack) return surface;
        return huxerui::View{surface}.With(huxerui::Grow(1.0F), huxerui::MultiTapGesture{})
            .On<huxerui::MultiTapEvents::Recognized>(turn).Key("home-card-face-" + cardId);
    });
}

} // namespace clashflux::ui
