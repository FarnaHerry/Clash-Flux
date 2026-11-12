#pragma once

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <huxerui/huxerui.h>
#include "scroll_hover.h"

namespace clashflux::ui {

inline constexpr float kScrollBarGutter = 16.0F;
// Keep the complete thumb outside the Linux custom-window resize band (6 DIP).
inline constexpr float kScrollBarEdgeInset = 7.0F;
inline bool ShowPageScrollBars() {
    return huxerui::UseViewportClass() != huxerui::ViewportClass::Compact;
}

struct PageScrollOffset { float value = 0.0F; };

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
    huxerui::ScrollMetrics metrics;
    huxerui::ScrollBarStyle style;
    std::shared_ptr<PageScrollOffset> remembered;
    bool visible = false;
    bool scroll_hovered = false;
    huxerui::Color highlight;
    std::shared_ptr<ScrollHoverRoute> route;
    std::shared_ptr<ScrollHoverRegion> region;
    bool operator==(const ScrollRail&) const = default;
};

class ScrollRail::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode& node, const ScrollRail& value) : value_(value) { Update(node, value); }
    void Update(huxerui::ViewNode&, const ScrollRail& value) {
        if (value.route != value_.route || value.region != value_.region) {
            if (value_.region) value_.region->rail_hovered = false;
            if (value_.route) value_.route->Refresh();
        }
        active_ = active_ || value.visible != value_.visible || value.scroll_hovered != value_.scroll_hovered ||
            value.metrics.offset != value_.metrics.offset ||
            (value.metrics.maximum_offset > 0.0F && value_.metrics.maximum_offset <= 0.0F);
        if (!value.visible || value.controller != value_.controller) pointer_.reset();
        value_ = value;
        InvalidatePaint();
    }
    PaintInvalidation PrepareGeometry(huxerui::ViewNode& node, huxerui::TextMeasurer&) override {
        const auto bounds = node.ContentBounds();
        const auto before = thumb_;
        const bool wasAvailable = available_;
        // Layout can publish a smaller range after the rail scope composed.
        // Paint and hit testing use that completed layout, not last frame's range.
        const auto metrics = value_.controller.Metrics();
        const auto& style = value_.style;
        track_ = {bounds.x + std::max(0.0F, bounds.width - style.margin - style.thickness),
            bounds.y + style.margin, style.thickness, std::max(0.0F, bounds.height - 2.0F * style.margin)};
        available_ = value_.visible && node.IsEnabled() && value_.controller.IsConnected() &&
            metrics.maximum_offset > 0.0F && bounds.width >= style.thickness && track_.height > 0.0F;
        const float length = std::min(track_.height, std::max(style.minimum_thumb_extent,
            track_.height * metrics.viewport_extent / std::max(1.0F, metrics.content_extent)));
        const float ratio = std::clamp(metrics.offset / std::max(1.0F, metrics.maximum_offset), 0.0F, 1.0F);
        thumb_ = {track_.x, track_.y + (track_.height - length) * ratio, track_.width, length};
        if (!available_) pointer_.reset();
        return before == thumb_ && wasAvailable == available_
            ? PaintInvalidation::None : PaintInvalidation::Foreground;
    }
    FrameResult OnFrame(huxerui::ViewNode& node, const huxerui::FrameInfo& frame) override {
        const float before = opacity_.Value();
        if (!value_.visible || !node.IsEnabled() || value_.controller.MaxOffset() <= 0.0F ||
            !value_.controller.IsConnected()) {
            opacity_.Set(0.0F); pointer_.reset(); hovered_ = false;
            if (value_.region && value_.region->rail_hovered) {
                value_.region->rail_hovered = false;
                value_.route->Refresh();
            }
        } else {
            if (active_) { deadline_ = frame.timestamp + value_.style.fade_out_delay; active_ = false; }
            const bool shown = hovered_ || value_.scroll_hovered || pointer_.has_value() || frame.timestamp < deadline_;
            opacity_.AnimateTo(shown ? 1.0F : 0.0F, huxerui::TweenSpec{
                .duration = frame.reduced_motion ? 0.0 : shown ? value_.style.fade_in_duration : value_.style.fade_out_duration});
        }
        const auto motion = opacity_.Advance(frame);
        if (before != opacity_.Value()) InvalidatePaint();
        if (motion.needs_frame) return {true, motion.wake_after};
        if (!hovered_ && !value_.scroll_hovered && !pointer_ && opacity_.Value() > 0.0F && frame.timestamp < deadline_)
            return {false, deadline_ - frame.timestamp};
        return {};
    }
    bool HitTest(huxerui::ViewNode& node, huxerui::Point point) const override {
        return node.IsEnabled() && available_ && opacity_.Value() > 0.01F && thumb_.Contains(point);
    }
    bool HoverHitTest(huxerui::ViewNode& node, huxerui::Point point) const override {
        return node.IsEnabled() && available_ && node.Bounds().Contains(point);
    }
    void OnHover(huxerui::ViewNode&, const huxerui::HoverEvent& event) override {
        hovered_ = event.type != huxerui::HoverEventType::Leave;
        if (value_.region && value_.region->rail_hovered != hovered_) {
            value_.region->rail_hovered = hovered_;
            value_.route->Refresh();
        }
        active_ = true; InvalidatePaint();
    }
    PointerResult OnPointer(huxerui::ViewNode& node, const huxerui::PointerEvent& event) override {
        if (!node.IsEnabled() || !available_ || !value_.controller.IsConnected()) {
            pointer_.reset(); return PointerResult::Ignored;
        }
        if (event.type == huxerui::PointerEventType::Down) {
            if (!thumb_.Contains(event.position) || opacity_.Value() <= 0.01F) return PointerResult::Ignored;
            pointer_ = event.pointer_id; grab_ = event.position.y - thumb_.y;
        }
        if (!pointer_ || *pointer_ != event.pointer_id) return PointerResult::Ignored;
        if (event.type == huxerui::PointerEventType::Move) {
            const float travel = track_.height - thumb_.height;
            if (travel > 0.0F) {
                const float ratio = std::clamp((event.position.y - grab_ - track_.y) / travel, 0.0F, 1.0F);
                if (!value_.controller.ScrollTo(ratio * value_.controller.MaxOffset())) pointer_.reset();
                else if (value_.remembered) value_.remembered->value = value_.controller.Offset();
            }
        }
        if (event.type == huxerui::PointerEventType::Up || event.type == huxerui::PointerEventType::Cancel)
            pointer_.reset();
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
    }
private:
    ScrollRail value_;
    huxerui::Rect track_, thumb_;
    huxerui::MotionController opacity_{0.0F};
    std::optional<std::int64_t> pointer_;
    float grab_ = 0.0F;
    double deadline_ = 0.0;
    bool available_ = false, hovered_ = false, active_ = true;
};

// Finish configuring the real scroll node before wrapping it. The same Row and
// rail stay mounted across viewport changes; only the reserved width changes.
template<class Scrollable>
huxerui::View ScrollArea(Scrollable scrollable,
    std::optional<huxerui::ScrollController> supplied = {},
    std::shared_ptr<PageScrollOffset> remembered = {}, std::optional<float> horizontal = {}) {
    return huxerui::Scope([scrollable, supplied, remembered, horizontal] {
        const auto retained = huxerui::UseScrollController(remembered ? remembered->value : 0.0F);
        const auto controller = supplied.value_or(retained);
        const bool wide = ShowPageScrollBars();
        const auto parent = huxerui::UseEnvironment<ScrollHoverContext>();
        const auto ownRoute = huxerui::UseState(std::make_shared<ScrollHoverRoute>()).Get();
        const auto route = parent.route ? parent.route : ownRoute;
        const auto region = huxerui::UseState(std::make_shared<ScrollHoverRegion>()).Get();
        const float inset = ResolvePageBodyInset(
            horizontal.value_or(huxerui::UseEnvironment<PageBodyInsets>().horizontal), wide);
        const auto rail = huxerui::Scope([controller, remembered, wide, route, region] {
            // Observable metrics are read only in this small scope, so scrolling
            // does not recompose the page or recreate its virtual item factory.
            const auto metrics = controller.Metrics();
            const bool selected = region->selected.Get();
            const auto& theme = huxerui::UseTheme();
            auto style = huxerui::ScrollBarStyle::Default();
            style.margin = std::max(style.margin, kScrollBarEdgeInset);
            style.track_color = theme.colors.on_surface; style.track_color.alpha *= 0.08F;
            style.thumb_color = theme.colors.on_surface; style.thumb_color.alpha *= 0.55F;
            style.fade_in_duration = theme.motion.reduced_motion ? 0.0F : theme.motion.fast;
            style.fade_out_duration = theme.motion.reduced_motion ? 0.0F : theme.motion.normal;
            return huxerui::Row {}.With(ScrollRail{controller, metrics, style, remembered, wide,
                selected, theme.colors.primary, route, region})
                .Key("scroll-area-rail");
        }).With(huxerui::Frame{.width = inset});
        return huxerui::Row {
          huxerui::Row {}.With(huxerui::Frame{.width = inset}).Key("scroll-area-leading"),
          huxerui::Column {
            huxerui::ProvideEnvironment(PageBodyInsets{}, huxerui::ProvideEnvironment(
                ScrollHoverContext{route, parent.depth + 1},
                Scrollable{scrollable}.Controller(controller).With(huxerui::Grow(1.0F),
                    ScrollViewportHover{route, region, controller, parent.depth, wide})
                    .template On<huxerui::ViewEvents::Hover>([route, region](const huxerui::HoverEvent& event) {
                        route->Hover(region, event);
                    }))),
          }.With(huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
          rail,
        }.With(huxerui::Spacing(0.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    });
}

inline huxerui::View PageScrollView(huxerui::View content,
    std::optional<huxerui::ScrollController> controller = {},
    std::shared_ptr<PageScrollOffset> remembered = {}, std::optional<float> horizontal = {}) {
    return ScrollArea(huxerui::ScrollView(content), controller, remembered, horizontal);
}

} // namespace clashflux::ui
