#pragma once

#include <cstdint>
#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
#include <huxerui/huxerui.h>

#include "ui.h"

namespace clashflux::ui {

inline constexpr double kHomeCardFlipDuration = 0.62;

// Turn around the vertical center, with a shallow lift and lateral depth arc.
// The local shared transition retains both faces and never changes grid layout.
struct HomeCardFlipBounds {
    huxerui::Rect Evaluate(huxerui::Rect from, huxerui::Rect to, float progress) const {
        if (progress <= 0.0F) return from;
        if (progress >= 1.0F) return to;
        const float p = std::clamp(progress, 0.0F, 1.0F);
        const float baseWidth = std::lerp(from.width, to.width, p);
        const float turn = std::sin(3.14159265F * p);
        const float width = std::max(2.5F, baseWidth * std::abs(std::cos(3.14159265F * p)));
        const float baseHeight = std::lerp(from.height, to.height, p);
        const float height = baseHeight * (1.0F - 0.06F * turn);
        const float depth = std::min(12.0F, baseWidth * 0.04F) * std::sin(6.2831853F * p);
        return {std::lerp(from.x + from.width * 0.5F, to.x + to.width * 0.5F, p) - width * 0.5F + depth,
                std::lerp(from.y, to.y, p) + (baseHeight - height) * 0.5F - 5.0F * turn, width, height};
    }
    bool operator==(const HomeCardFlipBounds&) const = default;
};

// Local lighting stays live above the shared snapshots. It never handles input
// or changes layout, and its retained clock stops as soon as the turn completes.
struct HomeCardFlipLighting {
    class Extension;
    bool back = false;
    bool reduced = false;
    huxerui::Color shade;
    huxerui::Color edge;
    float radius = 0.0F;
    bool operator==(const HomeCardFlipLighting&) const = default;
};

class HomeCardFlipLighting::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode&, const HomeCardFlipLighting& value) : value_(value) {}
    void Update(huxerui::ViewNode&, const HomeCardFlipLighting& value) {
        if (value.reduced) motion_.Set(1.0F);
        else if (value.back != value_.back) {
            motion_.Set(0.0F);
            motion_.AnimateTo(1.0F, huxerui::TweenSpec{kHomeCardFlipDuration, huxerui::Easing::EaseInOut});
        }
        value_ = value;
        InvalidatePaint(PaintInvalidation::Foreground);
    }
    FrameResult OnFrame(huxerui::ViewNode&, const huxerui::FrameInfo& frame) override {
        const auto result = motion_.Advance(frame);
        if (result.changed) InvalidatePaint(PaintInvalidation::Foreground);
        return {.needs_frame = result.needs_frame, .wake_after = result.wake_after};
    }
    void PaintAboveContent(const huxerui::ViewNode& node, huxerui::PaintContext& paint) const override {
        const float p = motion_.Value();
        if (p <= 0.0F || p >= 1.0F || value_.reduced) return;
        const auto bounds = node.ContentBounds();
        const auto face = HomeCardFlipBounds{}.Evaluate(bounds, bounds, p);
        const float turn = std::sin(3.14159265F * p);
        auto dark = value_.shade;
        dark.alpha = 0.32F * turn;
        auto light = value_.edge;
        light.alpha = 0.16F * turn;
        paint.DrawRect(face, huxerui::LinearGradient{
            .start = {p < 0.5F ? 0.0F : 1.0F, 0.5F},
            .end = {p < 0.5F ? 1.0F : 0.0F, 0.5F},
            .stops = {{0.0F, light}, {1.0F, dark}}},
            huxerui::CornerRadii{std::min(value_.radius, face.width * 0.5F)});
        if (face.width < 8.0F) {
            auto edge = value_.edge;
            edge.alpha = 0.75F * turn;
            paint.DrawRect(face, edge, huxerui::CornerRadii{1.25F});
        }
    }
private:
    HomeCardFlipLighting value_;
    huxerui::MotionController motion_{1.0F};
};

// Build the dashboard surface on its own node so its fill and hover layer share
// the same rounded geometry. A passive card does not acquire a click action.
inline huxerui::View HomeCardSurface(const huxerui::ThemeSpec& theme,
                                    huxerui::View content, bool allowFlip = true) {
    return huxerui::Scope([theme, content = std::move(content), allowFlip]() -> huxerui::View {
        auto pointer = huxerui::UseState<std::optional<std::int64_t>>(std::nullopt);
        auto back = huxerui::UseState(false);
        const auto flip = huxerui::UseSharedTransition();
        const auto turn = [back, flip, reduced = theme.motion.reduced_motion](const huxerui::MultiTapEvent&) {
            const huxerui::AnimationSpec motion = reduced
                ? huxerui::AnimationSpec{huxerui::SnapSpec{}}
                : huxerui::AnimationSpec{huxerui::TweenSpec{.duration = kHomeCardFlipDuration,
                    .easing = huxerui::Easing::EaseInOut}};
            flip.Run(motion, [back] { back = !back.Get(); });
        };
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
                  huxerui::Opacity(back.Get() ? 0.0F : 1.0F), huxerui::Enabled(!back.Get()),
                  huxerui::Semantics{.hidden = back.Get()}).Key("home-card-front"),
              // A real input surface above the retained front keeps nested controls
              // out of the pointer route, even when they enable themselves locally.
              back.Get() ? huxerui::View{huxerui::Row {}.With(
                  huxerui::Grow(1.0F), huxerui::MultiTapGesture{},
                  huxerui::PointerCursor(huxerui::PointerCursorKind::Default))
                  .On<huxerui::MultiTapEvents::Recognized>(turn).Key("home-card-back")} : huxerui::View{},
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
        surface = std::move(surface).With(huxerui::Grow(1.0F), huxerui::MultiTapGesture{},
            huxerui::SharedBounds("home-card-face").BoundsTransform(HomeCardFlipBounds{}))
            .On<huxerui::MultiTapEvents::Recognized>(turn);
        return huxerui::Column {surface}.With(flip.Scope(),
            HomeCardFlipLighting{back.Get(), theme.motion.reduced_motion,
                theme.colors.scrim, theme.colors.outline, islands.nested_radius},
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    });
}

} // namespace clashflux::ui
