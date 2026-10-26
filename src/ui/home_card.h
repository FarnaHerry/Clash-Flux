#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <huxerui/huxerui.h>

#include "ui.h"

namespace clashflux::ui {

// Build the dashboard surface on its own node so its fill and hover layer share
// the same rounded geometry. A passive card does not acquire a click action.
inline huxerui::View HomeCardSurface(const huxerui::ThemeSpec& theme,
                                    huxerui::View content) {
    return huxerui::Scope([theme, content = std::move(content)] {
        auto pointer = huxerui::UseState<std::optional<std::int64_t>>(std::nullopt);
        const bool pressed = pointer.Get().has_value();
        const bool compress = pressed && !theme.motion.reduced_motion;
        const huxerui::AnimationSpec animation = theme.motion.reduced_motion
            ? huxerui::AnimationSpec{huxerui::SnapSpec{}}
            : pressed ? huxerui::AnimationSpec{huxerui::TweenSpec{.duration = 0.07}}
                      : huxerui::AnimationSpec{huxerui::SpringSpec{.stiffness = 480.0F, .damping_ratio = 0.72F}};
        const auto islands = ResolveIslandTheme(theme);
        auto hoverFill = theme.colors.primary;
        hoverFill.alpha = 0.05F;
        return huxerui::Column{content}.With(
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
    });
}

} // namespace clashflux::ui
