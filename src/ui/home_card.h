#pragma once

#include <cstdint>
#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <huxerui/huxerui.h>

#include "ui.h"

namespace clashflux::ui {

inline constexpr double kHomeCardRevealDuration = 0.20;

// Build the dashboard surface on its own node so its fill and hover layer share
// the same rounded geometry. A passive card does not acquire a click action.
inline huxerui::View HomeCardSurface(const huxerui::ThemeSpec& theme,
                                    huxerui::View content, std::string cardId, bool allowFlip = true,
                                    huxerui::View backContent = huxerui::View{},
                                    std::function<void()> onReveal = {}) {
    return huxerui::Scope([theme, content = std::move(content), cardId, allowFlip,
                           backContent = std::move(backContent), onReveal = std::move(onReveal)]() -> huxerui::View {
        auto pointer = huxerui::UseState<std::optional<std::int64_t>>(std::nullopt);
        auto back = huxerui::UseState(false);
        const auto turn = [back, onReveal](const huxerui::MultiTapEvent&) {
            if (!back.Get() && onReveal) onReveal();
            back = !back.Get();
        };
        const huxerui::AnimationSpec faceMotion = theme.motion.reduced_motion
            ? huxerui::AnimationSpec{huxerui::SnapSpec{}}
            : huxerui::AnimationSpec{huxerui::TweenSpec{kHomeCardRevealDuration, huxerui::Easing::EaseOut}};
        const bool pressed = pointer.Get().has_value();
        const bool compress = pressed && !theme.motion.reduced_motion;
        const huxerui::AnimationSpec animation = theme.motion.reduced_motion
            ? huxerui::AnimationSpec{huxerui::SnapSpec{}}
            : pressed ? huxerui::AnimationSpec{huxerui::TweenSpec{.duration = 0.07}}
                      : huxerui::AnimationSpec{huxerui::SpringSpec{.stiffness = 480.0F, .damping_ratio = 0.72F}};
        const auto islands = ResolveIslandTheme(theme);
        auto hoverFill = theme.colors.primary;
        hoverFill.alpha = 0.05F;
        huxerui::View faceContent = content;
        if (allowFlip) {
            faceContent = huxerui::Stack {
              huxerui::Column {content}.With(huxerui::Grow(1.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                  huxerui::Transition{huxerui::AnimateTo(back.Get() ? 0.0F : 1.0F, faceMotion)}
                      .Opacity(0.0F, 1.0F).Scale(0.96F, 1.0F),
                  huxerui::Enabled(!back.Get()), huxerui::Semantics{.hidden = back.Get()})
                  .Key("home-card-front"),
              // Keep an input shield above the retained front. Child controls may
              // explicitly enable themselves, so ancestor Enabled alone is insufficient.
              back.Get() ? huxerui::View{huxerui::Row {}.With(
                  huxerui::Grow(1.0F), huxerui::MultiTapGesture{},
                  huxerui::PointerCursor(huxerui::PointerCursorKind::Default))
                  .On<huxerui::MultiTapEvents::Recognized>(turn).Key("home-card-back")} : huxerui::View{},
              huxerui::Column {backContent}.With(huxerui::Grow(1.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                  huxerui::Transition{huxerui::AnimateTo(back.Get() ? 1.0F : 0.0F, faceMotion)}
                      .Opacity(0.0F, 1.0F).Scale(0.96F, 1.0F),
                  huxerui::Enabled(back.Get()), huxerui::Semantics{.hidden = !back.Get()})
                  .Key("home-card-back-content"),
            }.With(huxerui::Grow(1.0F),
                   huxerui::Align(huxerui::HorizontalAlignment::Stretch, huxerui::VerticalAlignment::Stretch));
        }
        huxerui::View surface = huxerui::Column{faceContent}.With(
            huxerui::Padding(islands.island_padding),
            huxerui::Background(islands.raised),
            huxerui::CornerRadius(islands.nested_radius),
            huxerui::ClipChildren(),
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
            huxerui::Scale(huxerui::AnimateTo(compress ? 0.985F : 1.0F, animation)),
            huxerui::Offset(huxerui::AnimateTo(huxerui::Point{0.0F, compress ? 1.0F : 0.0F}, animation)),
            huxerui::Indication{
                .hover = huxerui::IndicationLayer{
                    .fill = huxerui::VisualFill{huxerui::Brush{hoverFill}},
                    .placement = huxerui::IndicationPlacement::BehindContent,
                },
            }).On<huxerui::ViewEvents::PointerIntercept>([pointer](const huxerui::PointerEvent& event) {
                if (event.type == huxerui::PointerEventType::Down && !pointer.Get() &&
                    (event.device_kind == huxerui::PointerDeviceKind::Touch ||
                     event.changed_button == huxerui::PointerButton::Primary))
                    pointer = event.pointer_id;
                else if ((event.type == huxerui::PointerEventType::Up || event.type == huxerui::PointerEventType::Cancel) &&
                         pointer.Get() == event.pointer_id)
                    pointer = std::nullopt;
                // Observe the whole card, including its controls, without taking
                // ownership from a child button, switch, or scrolling gesture.
                return false;
            });
        if (!allowFlip) return surface;
        return huxerui::View{surface}.With(huxerui::Grow(1.0F), huxerui::MultiTapGesture{})
            .On<huxerui::MultiTapEvents::Recognized>(turn).Key("home-card-face-" + cardId);
    });
}

} // namespace clashflux::ui
