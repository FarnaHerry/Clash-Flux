#pragma once

#include <cstdint>
#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <huxerui/huxerui.h>

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

struct HomeCardHoverOutline {
    class Extension;
    huxerui::Border border;
    huxerui::CornerRadii corners;
    huxerui::AnimationSpec fade;
    bool hovered;
    bool operator==(const HomeCardHoverOutline&) const = default;
};

class HomeCardHoverOutline::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode&, const HomeCardHoverOutline& outline)
        : outline_(outline), opacity_(outline.hovered ? 1.0F : 0.0F) {}

    void Update(huxerui::ViewNode&, const HomeCardHoverOutline& outline) {
        outline_ = outline;
        opacity_.AnimateTo(outline.hovered ? 1.0F : 0.0F, outline.fade);
        InvalidatePaint(PaintInvalidation::Foreground);
    }

    FrameResult OnFrame(huxerui::ViewNode&, const huxerui::FrameInfo& frame) override {
        const auto result = opacity_.Advance(frame);
        if (result.changed) InvalidatePaint(PaintInvalidation::Foreground);
        return {.needs_frame = result.needs_frame, .wake_after = result.wake_after};
    }

    void PaintAboveContent(const huxerui::ViewNode& node, huxerui::PaintContext& paint) const override {
        auto color = outline_.border.color;
        color.alpha *= std::clamp(opacity_.Value(), 0.0F, 1.0F);
        if (color.alpha <= 0.0F) return;
        paint.DrawBorder(node.Bounds(), color, huxerui::StrokeStyle{.width = outline_.border.width}, outline_.corners);
    }

private:
    HomeCardHoverOutline outline_;
    huxerui::MotionController opacity_;
};

// Build the dashboard surface on its own node so its fill and hover layer share
// the same rounded geometry. A passive card does not acquire a click action.
inline huxerui::View HomeCardSurface(const huxerui::ThemeSpec& theme,
                                    huxerui::View content, std::string cardId,
                                    huxerui::ViewFactory makeBack = {},
                                    huxerui::State<std::string> expandedPicker = {}) {
    return huxerui::Scope([theme, content = std::move(content), cardId, makeBack = std::move(makeBack),
                           expandedPicker]() -> huxerui::View {
        auto pointer = huxerui::UseState<std::optional<std::int64_t>>(std::nullopt);
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
        const bool pressed = pointer.Get().has_value();
        const bool compress = pressed && !reducedMotion;
        const huxerui::AnimationSpec animation = reducedMotion
            ? huxerui::AnimationSpec{huxerui::SnapSpec{}}
            : pressed ? huxerui::AnimationSpec{huxerui::TweenSpec{.duration = 0.07}}
                      : huxerui::AnimationSpec{huxerui::SpringSpec{.stiffness = 480.0F, .damping_ratio = 0.72F}};
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
            huxerui::Scale(huxerui::AnimateTo(compress ? 0.985F : 1.0F, animation)),
            huxerui::Offset(huxerui::AnimateTo(huxerui::Point{0.0F, compress ? 1.0F : 0.0F}, animation)),
            HomeCardHoverOutline{hoverStyle.border.value(), huxerui::CornerRadii{islands.nested_radius},
                showOutline ? hoverStyle.enter : hoverStyle.exit, showOutline}
        ).On<huxerui::ViewEvents::Hover>([hovered](const huxerui::HoverEvent& event) {
            // A nested control owns its indication. The card observes containment independently.
            if (event.type != huxerui::HoverEventType::Move) hovered = event.type == huxerui::HoverEventType::Enter;
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
        if (!makeBack) return surface;
        return huxerui::View{surface}.With(huxerui::Grow(1.0F), huxerui::MultiTapGesture{})
            .On<huxerui::MultiTapEvents::Recognized>(turn).Key("home-card-face-" + cardId);
    });
}

} // namespace clashflux::ui
