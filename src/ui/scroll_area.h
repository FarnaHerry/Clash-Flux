#pragma once

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <huxerui/huxerui.h>
#include "scroll_motion.h"
#include "scroll_insets.h"

namespace clashflux::ui {

inline constexpr float kScrollBarGutter = 16.0F;
// Keep the complete thumb outside the Linux custom-window resize band (6 DIP).
inline constexpr float kScrollBarEdgeInset = 7.0F;
struct ScrollAreaStyle {
    bool step_buttons = true;
    float step_extent = 40.0F;
    double step_duration = 0.12;
    double wheel_duration = 0.12;
    huxerui::StringVariant up_label = "Scroll up", down_label = "Scroll down";
    static ScrollAreaStyle Default() { return {}; }
    bool operator==(const ScrollAreaStyle&) const = default;
};
inline bool ShowPageScrollBars() {
    return huxerui::UseViewportClass() != huxerui::ViewportClass::Compact;
}

// Scaffolds publish the requested body margin; the first content region owns
// it. Nested fields, local lists and popups start with a fresh margin budget.
struct PageBodyInsets {
    float horizontal = 0.0F;
    static PageBodyInsets Default() { return {}; }
    bool operator==(const PageBodyInsets&) const = default;
};

inline float ResolvePageBodyInset(float requested, bool wide) {
    return std::max(requested, wide ? kScrollBarGutter : 0.0F);
}

inline huxerui::View PageBodyRegion(huxerui::View content, float horizontal) {
    return huxerui::Column {
      huxerui::ProvideEnvironment(PageBodyInsets{horizontal}, content),
    }.With(huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// Static/empty/loading content uses the same margin as its scrolling sibling.
inline huxerui::View PageBodyFrame(huxerui::View content) {
    return huxerui::Scope([content] {
        const float requested = huxerui::UseEnvironment<PageBodyInsets>().horizontal;
        const float inset = requested > 0.0F ? ResolvePageBodyInset(requested, ShowPageScrollBars()) : 0.0F;
        return huxerui::Column {
          huxerui::ProvideEnvironment(PageBodyInsets{}, content),
        }.With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(inset, 0.0F)),
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    });
}

// A rail observes the controller; it never owns another scroll offset.
struct ScrollRail {
    class Extension;
    huxerui::ScrollController controller;
    huxerui::ScrollBarStyle style;
    std::shared_ptr<PageScrollOffset> remembered;
    bool visible = false;
    bool scroll_hovered = false;
    huxerui::Color highlight;
    std::shared_ptr<ScrollHoverRoute> route;
    std::shared_ptr<ScrollHoverRegion> region;
    ScrollAreaStyle controls;
    std::string up_label, down_label;
    bool foreground = true;
    std::shared_ptr<ScrollMotion> motion;
    huxerui::EdgeInsets insets;
    bool operator==(const ScrollRail&) const = default;
};

class ScrollRail::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode& node, const ScrollRail& value) : value_(value) { Update(node, value); }
    ~Extension() override {
        value_.motion->CancelArrows();
        if (value_.region->rail_hovered) {
            value_.region->rail_hovered = false;
            value_.route->Refresh();
        }
    }
    void Update(huxerui::ViewNode&, const ScrollRail& value) {
        if (value.route != value_.route || value.region != value_.region) {
            if (value_.region) value_.region->rail_hovered = false;
            if (value_.route) value_.route->Refresh();
        }
        active_ = active_ || value.visible != value_.visible || value.scroll_hovered != value_.scroll_hovered;
        if (!value.visible || value.controller != value_.controller || value.remembered != value_.remembered ||
            (!value.foreground && value_.foreground)) {
            CancelPress(); value_.motion->CancelArrows();
        } else if (!value.controls.step_buttons) {
            value_.motion->CancelArrows(); if (direction_ != 0) CancelPress();
        }
        if (value.up_label != value_.up_label || value.down_label != value_.down_label) InvalidateSemantics();
        value_ = value;
        InvalidatePaint();
    }
    PaintInvalidation PrepareGeometry(huxerui::ViewNode& node, huxerui::TextMeasurer&) override {
        const auto bounds = node.ContentBounds();
        const auto before = thumb_;
        const bool wasAvailable = available_;
        const auto oldUp = up_, oldDown = down_;
        const bool oldCanUp = can_up_, oldCanDown = can_down_;
        // Layout can publish a smaller range after the rail scope composed.
        // Paint and hit testing use that completed layout, not last frame's range.
        const auto metrics = value_.controller.Metrics();
        const auto& style = value_.style;
        const float top = std::max(style.margin, value_.insets.top);
        const float bottom = std::max(style.margin, value_.insets.bottom);
        track_ = {bounds.x + std::max(0.0F, bounds.width - style.margin - style.thickness),
            bounds.y + std::min(top, bounds.height), style.thickness, std::max(0.0F, bounds.height - top - bottom)};
        up_ = down_ = {};
        const float extent = std::min(12.0F, bounds.width);
        if (value_.controls.step_buttons && track_.height >= extent * 2.0F + style.minimum_thumb_extent + 1.0F) {
            const float x = std::clamp(track_.x + track_.width * 0.5F - extent * 0.5F,
                bounds.x, bounds.x + bounds.width - extent);
            up_ = {x, track_.y, extent, extent};
            down_ = {x, track_.y + track_.height - extent, extent, extent};
            track_.y += extent; track_.height -= extent * 2.0F;
        }
        available_ = value_.visible && node.IsEnabled() && value_.controller.IsConnected() &&
            metrics.maximum_offset > 0.0F && bounds.width >= style.thickness && track_.height > 0.0F;
        const float length = std::min(track_.height, std::max(style.minimum_thumb_extent,
            track_.height * metrics.viewport_extent / std::max(1.0F, metrics.content_extent)));
        const float ratio = std::clamp(metrics.offset / std::max(1.0F, metrics.maximum_offset), 0.0F, 1.0F);
        thumb_ = {track_.x, track_.y + (track_.height - length) * ratio, track_.width, length};
        can_up_ = CanStep(-1); can_down_ = CanStep(1);
        if (!available_) { CancelPress(); value_.motion->CancelArrows(); }
        else if (up_.height == 0.0F) { value_.motion->CancelArrows(); if (direction_ != 0) CancelPress(); }
        const bool buttonsChanged = oldUp != up_ || oldDown != down_ || oldCanUp != can_up_ || oldCanDown != can_down_;
        if (buttonsChanged || wasAvailable != available_) InvalidateSemantics();
        return before == thumb_ && wasAvailable == available_ && !buttonsChanged
            ? PaintInvalidation::None : PaintInvalidation::Foreground;
    }
    FrameResult OnFrame(huxerui::ViewNode& node, const huxerui::FrameInfo& frame) override {
        const float before = opacity_.Value();
        // Read outside composition: real movement updates retained paint and
        // fade timing without rebuilding the rail declaration or its labels.
        const auto metrics = value_.controller.Metrics();
        active_ = active_ || metrics.offset != metrics_.offset ||
            (metrics.maximum_offset > 0.0F && metrics_.maximum_offset <= 0.0F);
        metrics_ = metrics;
        if (direction_ != 0 && (!value_.foreground || !value_.motion->IsArrow() ||
            value_.region->input_revision != input_revision_)) {
            CancelPress(); value_.motion->CancelArrows();
        }
        if (!value_.visible || !node.IsEnabled() || metrics.maximum_offset <= 0.0F ||
            !value_.controller.IsConnected()) {
            opacity_.Set(0.0F); CancelPress(); value_.motion->CancelArrows(); hovered_ = false;
            if (value_.region && value_.region->rail_hovered) {
                value_.region->rail_hovered = false;
                value_.route->Refresh();
            }
        } else {
            if (pointer_ && direction_ != 0 && held_inside_ && CanStep(direction_)) {
                if (repeat_at_ == 0.0) repeat_at_ = frame.timestamp + 0.35;
                else if (frame.timestamp >= repeat_at_) {
                    Step(direction_, true); repeat_at_ = frame.timestamp + 0.06;
                }
            }
            if (active_) { deadline_ = frame.timestamp + value_.style.fade_out_delay; active_ = false; }
            const bool shown = hovered_ || value_.scroll_hovered || pointer_.has_value() || frame.timestamp < deadline_;
            opacity_.AnimateTo(shown ? 1.0F : 0.0F, huxerui::TweenSpec{
                .duration = frame.reduced_motion ? 0.0 : shown ? value_.style.fade_in_duration : value_.style.fade_out_duration});
        }
        const auto motion = opacity_.Advance(frame);
        if (before != opacity_.Value()) InvalidatePaint();
        if (motion.needs_frame || value_.motion->IsActive()) return {true, motion.wake_after};
        if (pointer_ && direction_ != 0 && held_inside_ && CanStep(direction_))
            return {false, std::max(0.0, repeat_at_ - frame.timestamp)};
        if (!hovered_ && !value_.scroll_hovered && !pointer_ && opacity_.Value() > 0.0F && frame.timestamp < deadline_)
            return {false, deadline_ - frame.timestamp};
        return {};
    }
    bool HitTest(huxerui::ViewNode& node, huxerui::Point point) const override {
        return node.IsEnabled() && available_ && opacity_.Value() > 0.01F &&
            (thumb_.Contains(point) || up_.Contains(point) || down_.Contains(point));
    }
    bool HoverHitTest(huxerui::ViewNode& node, huxerui::Point point) const override {
        return node.IsEnabled() && available_ && node.Bounds().Contains(point);
    }
    void OnHover(huxerui::ViewNode&, const huxerui::HoverEvent& event) override {
        const bool hovered = event.type != huxerui::HoverEventType::Leave;
        if (hovered_ == hovered) return;
        hovered_ = hovered;
        if (value_.region && value_.region->rail_hovered != hovered_) {
            value_.region->rail_hovered = hovered_;
            value_.route->Refresh();
        }
        active_ = true; InvalidatePaint();
    }
    PointerResult OnPointer(huxerui::ViewNode& node, const huxerui::PointerEvent& event) override {
        if (!node.IsEnabled() || !available_ || !value_.controller.IsConnected()) {
            CancelPress(); value_.motion->Cancel(); return PointerResult::Ignored;
        }
        if (event.type == huxerui::PointerEventType::Down) {
            if (opacity_.Value() <= 0.01F || (event.device_kind == huxerui::PointerDeviceKind::Mouse &&
                event.changed_button != huxerui::PointerButton::Primary)) return PointerResult::Ignored;
            direction_ = up_.Contains(event.position) ? -1 : down_.Contains(event.position) ? 1 : 0;
            if (direction_ != 0) {
                if (!CanStep(direction_)) { CancelPress(); value_.motion->Cancel(); return PointerResult::Handled; }
                Step(direction_); held_inside_ = true; repeat_at_ = 0.0;
            } else {
                if (!thumb_.Contains(event.position)) return PointerResult::Ignored;
                value_.motion->Cancel();
            }
            pointer_ = event.pointer_id; grab_ = event.position.y - thumb_.y;
        }
        if (!pointer_ || *pointer_ != event.pointer_id) return PointerResult::Ignored;
        if (event.type == huxerui::PointerEventType::Move && direction_ != 0) {
            held_inside_ = (direction_ < 0 ? up_ : down_).Contains(event.position);
            if (!held_inside_) { repeat_at_ = 0.0; value_.motion->Cancel(); }
        } else if (event.type == huxerui::PointerEventType::Move) {
            const float travel = track_.height - thumb_.height;
            if (travel > 0.0F) {
                const float ratio = std::clamp((event.position.y - grab_ - track_.y) / travel, 0.0F, 1.0F);
                if (!value_.controller.ScrollTo(ratio * value_.controller.MaxOffset())) pointer_.reset();
                else if (value_.remembered) value_.remembered->value = value_.controller.Offset();
            }
        }
        if (event.type == huxerui::PointerEventType::Up || event.type == huxerui::PointerEventType::Cancel)
            CancelPress();
        if (event.type == huxerui::PointerEventType::Cancel) value_.motion->Cancel();
        active_ = true; InvalidatePaint();
        return event.type == huxerui::PointerEventType::Down ? PointerResult::Capture : PointerResult::Handled;
    }
    void PaintAboveContent(const huxerui::ViewNode&, huxerui::PaintContext& paint) const override {
        if (!available_ || opacity_.Value() <= 0.0F) return;
        auto trackColor = value_.style.track_color;
        auto thumbColor = value_.style.thumb_color;
        if (value_.scroll_hovered || pointer_) thumbColor = value_.highlight;
        trackColor.alpha *= opacity_.Value(); thumbColor.alpha *= opacity_.Value();
        paint.DrawRect(track_, trackColor, value_.style.corner_radius);
        paint.DrawRect(thumb_, thumbColor, value_.style.corner_radius);
        PaintArrow(paint, up_, -1, can_up_, thumbColor);
        PaintArrow(paint, down_, 1, can_down_, thumbColor);
    }
    void BuildSemantics(huxerui::SemanticBuilder& builder) const override {
        if (!available_ || up_.height <= 0.0F) return;
        builder.AddChild(1, up_, {.role = huxerui::SemanticRole::Button, .label = value_.up_label}, can_up_);
        builder.AddAction(1, huxerui::SemanticActionKind::Activate);
        builder.AddChild(2, down_, {.role = huxerui::SemanticRole::Button, .label = value_.down_label}, can_down_);
        builder.AddAction(2, huxerui::SemanticActionKind::Activate);
    }
    bool OnSemanticAction(std::uint64_t id, const huxerui::SemanticAction& action) override {
        if (action.kind != huxerui::SemanticActionKind::Activate || (id != 1 && id != 2)) return false;
        return Step(id == 1 ? -1 : 1);
    }
private:
    void CancelPress() { pointer_.reset(); direction_ = 0; held_inside_ = false; repeat_at_ = 0.0; }
    bool CanStep(int direction) const {
        return available_ && up_.height > 0.0F && value_.controls.step_buttons && value_.foreground &&
            value_.controller.IsConnected() &&
            (direction < 0 ? value_.controller.Offset() > 0.0F
                           : value_.controller.Offset() < value_.controller.MaxOffset());
    }
    bool Step(int direction, bool from_frame = false) {
        if (!CanStep(direction)) return false;
        const float step = std::isfinite(value_.controls.step_extent) && value_.controls.step_extent > 0.0F
            ? value_.controls.step_extent : 40.0F;
        input_revision_ = value_.region->input_revision;
        const float consumed = value_.motion->Request(step * static_cast<float>(direction),
            value_.controls.step_duration, ScrollMotion::Input::Arrow, from_frame);
        active_ = true; InvalidatePaint(); InvalidateSemantics();
        return consumed != 0.0F;
    }
    void PaintArrow(huxerui::PaintContext& paint, huxerui::Rect bounds, int direction,
                    bool enabled, huxerui::Color color) const {
        if (bounds.height <= 0.0F) return;
        if (!enabled) color.alpha *= 0.3F;
        const float x = bounds.x + bounds.width * 0.5F, y = bounds.y + bounds.height * 0.5F;
        const float half = std::min(2.4F, bounds.width * 0.25F);
        const float tip = static_cast<float>(direction) * half * 0.5F;
        paint.DrawLine({x - half, y - tip}, {x, y + tip}, color, {.width = 1.2F});
        paint.DrawLine({x, y + tip}, {x + half, y - tip}, color, {.width = 1.2F});
    }
    ScrollRail value_;
    huxerui::ScrollMetrics metrics_;
    huxerui::Rect track_, thumb_, up_, down_;
    huxerui::MotionController opacity_{0.0F};
    std::optional<std::int64_t> pointer_;
    float grab_ = 0.0F;
    double deadline_ = 0.0;
    bool available_ = false, hovered_ = false, active_ = true;
    bool can_up_ = false, can_down_ = false, held_inside_ = false;
    int direction_ = 0;
    double repeat_at_ = 0.0;
    std::uint64_t input_revision_ = 0;
};

// Finish configuring the real scroll node before wrapping it. The same Row and
// margin nodes stay mounted across viewport changes; the viewport is retained.
template<class Scrollable>
huxerui::View ScrollArea(Scrollable scrollable,
    std::optional<huxerui::ScrollController> supplied = {},
    std::shared_ptr<PageScrollOffset> remembered = {}, std::optional<float> horizontal = {},
    ScrollAreaInsets insets = {}) {
    return huxerui::Scope([scrollable, supplied, remembered, horizontal, insets] {
        const auto retained = huxerui::UseScrollController(remembered ? remembered->value : 0.0F);
        const auto controller = supplied.value_or(retained);
        const bool wide = ShowPageScrollBars();
        const auto parent = huxerui::UseEnvironment<ScrollHoverContext>();
        const auto ownRoute = huxerui::UseState(std::make_shared<ScrollHoverRoute>()).Get();
        const auto route = parent.route ? parent.route : ownRoute;
        const auto region = huxerui::UseState(std::make_shared<ScrollHoverRegion>()).Get();
        const auto motionParent = huxerui::UseEnvironment<ScrollMotionContext>();
        const auto ownWheel = huxerui::UseState(std::make_shared<ScrollWheelRoute>()).Get();
        const auto wheel = motionParent.wheel ? motionParent.wheel : ownWheel;
        const auto motion = huxerui::UseState(std::make_shared<ScrollMotion>(controller, remembered, region,
            huxerui::UseTaskScope(), huxerui::UseWindow())).Get();
        if (motion->controller != controller || motion->remembered != remembered) motion->Cancel();
        motion->controller = controller; motion->remembered = remembered; motion->region = region;
        const float inset = ResolvePageBodyInset(
            horizontal.value_or(huxerui::UseEnvironment<PageBodyInsets>().horizontal), wide);
        auto contentInsets = insets;
        if (contentInsets.top_follows_horizontal) contentInsets.top = inset;
        const auto rail = huxerui::Scope([controller, remembered, wide, route, region, motion, contentInsets] {
            // Compact keeps the same margin node, without rail subscriptions,
            // geometry, paint, semantics, hover or frame callbacks.
            if (!wide) return huxerui::Row {}.Key("scroll-area-rail");
            // Only appearance changes recompose this small scope. The retained
            // rail reads scroll metrics during frame/geometry callbacks.
            const bool selected = region->selected.Get();
            const auto& theme = huxerui::UseTheme();
            const auto controls = huxerui::UseEnvironment<ScrollAreaStyle>();
            auto style = huxerui::ScrollBarStyle::Default();
            style.margin = std::max(style.margin, kScrollBarEdgeInset);
            style.track_color = theme.colors.on_surface; style.track_color.alpha *= 0.08F;
            style.thumb_color = theme.colors.on_surface; style.thumb_color.alpha *= 0.55F;
            style.fade_in_duration = theme.motion.reduced_motion ? 0.0F : theme.motion.fast;
            style.fade_out_duration = theme.motion.reduced_motion ? 0.0F : theme.motion.normal;
            return huxerui::Row {}.With(ScrollRail{controller, style, remembered, wide,
                selected, theme.colors.primary, route, region, controls,
                huxerui::UseString(controls.up_label), huxerui::UseString(controls.down_label),
                huxerui::UseWindow().LifecycleState() != huxerui::WindowLifecycleState::Background, motion,
                ResolveScrollAreaInsets(theme, contentInsets)})
                .Key("scroll-area-rail");
        }).With(huxerui::Frame{.width = inset});
        const auto viewport = huxerui::Scope([scrollable, controller, route, region, parent, wide, motion, wheel] {
            const auto& theme = huxerui::UseTheme();
            const auto controls = huxerui::UseEnvironment<ScrollAreaStyle>();
            motion->tasks = huxerui::UseTaskScope(); motion->window = huxerui::UseWindow();
            huxerui::View content = Scrollable{scrollable}.Controller(controller).With(huxerui::Grow(1.0F),
                ScrollViewportMotion{motion, wheel, !theme.motion.reduced_motion,
                    motion->window.LifecycleState() != huxerui::WindowLifecycleState::Background, controls.wheel_duration});
            if (!wide) return content;
            return huxerui::View{content}.With(ScrollViewportHover{route, region, controller, parent.depth, true})
                .template On<huxerui::ViewEvents::Hover>([route, region](const huxerui::HoverEvent& event) {
                    route->Hover(region, event);
                });
        }).With(huxerui::Grow(1.0F))
            .template On<huxerui::ViewEvents::ScrollInput>(
                [motion, wheel, region](const huxerui::ScrollInputEvent& event) {
                    wheel->pending = event.kind == huxerui::ScrollInputKind::Wheel && event.delta_y != 0.0F;
                    wheel->candidates.clear();
                    wheel->target = motion;
                    if (event.kind != huxerui::ScrollInputKind::Wheel) ++region->input_revision;
                    return false;
                })
            .template On<huxerui::ViewEvents::PointerIntercept>([region](const huxerui::PointerEvent& event) {
                if (event.type == huxerui::PointerEventType::Down) ++region->input_revision;
                return false;
            });
        return huxerui::Row {
          huxerui::Row {}.With(huxerui::Frame{.width = inset}).Key("scroll-area-leading"),
          // Grow belongs to this horizontal Row: fill width while preserving
          // the natural height of menus and other unconstrained scroll hosts.
          huxerui::ProvideEnvironment(
              PageBodyInsets{},
              huxerui::ProvideEnvironment(
                  ScrollHoverContext{route, parent.depth + 1},
                  huxerui::ProvideEnvironment(ScrollMotionContext{wheel},
                      huxerui::ProvideEnvironment(contentInsets, viewport))
              )
          ),
          rail,
        }.With(huxerui::Spacing(0.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
            .template On<huxerui::ViewEvents::KeyIntercept>([region](const huxerui::KeyEvent& event) {
                if (event.type == huxerui::KeyEventType::Down) ++region->input_revision;
                return false;
            });
    });
}

// Local forms already own their field/header spacing. Their default edge
// request is zero; full pages use ScrollablePageContent's themed defaults.
inline huxerui::View PageScrollView(huxerui::View content,
    std::optional<huxerui::ScrollController> controller = {},
    std::shared_ptr<PageScrollOffset> remembered = {}, std::optional<float> horizontal = {},
    ScrollAreaInsets insets = ScrollAreaInsets::None()) {
    return ScrollArea(huxerui::ScrollView(PageScrollContent(content)), controller, remembered, horizontal, insets);
}

} // namespace clashflux::ui
