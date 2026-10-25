#include "page_loading.h"
#include "theme_colors.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace clashflux::ui {

struct SkeletonShimmerRegion {
    huxerui::Rect bounds;
    std::optional<double> epoch;
};

namespace {

constexpr double kShimmerPeriod = 1.4;
constexpr double kShimmerFrameInterval = 1.0 / 30.0;

huxerui::Color Mix(huxerui::Color from, huxerui::Color to, float amount) {
    return {std::lerp(from.red, to.red, amount), std::lerp(from.green, to.green, amount),
            std::lerp(from.blue, to.blue, amount), 1.0F};
}

struct ShimmerViewport {
    class Extension;
    std::shared_ptr<SkeletonShimmerRegion> region;
    bool operator==(const ShimmerViewport&) const = default;
};

class ShimmerViewport::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode& node, const ShimmerViewport& value) { Update(node, value); }
    void Update(huxerui::ViewNode&, const ShimmerViewport& value) { region_ = value.region; }
    PaintInvalidation PrepareGeometry(huxerui::ViewNode& node, huxerui::TextMeasurer&) override {
        region_->bounds = node.PresentationBounds();
        return PaintInvalidation::None;
    }
private:
    std::shared_ptr<SkeletonShimmerRegion> region_;
};

struct ShimmerPaint {
    class Extension;
    SkeletonShimmerStyle style;
    bool operator==(const ShimmerPaint&) const = default;
};

class ShimmerPaint::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode& node, const ShimmerPaint& value) { Update(node, value); }
    void Update(huxerui::ViewNode&, const ShimmerPaint& value) {
        style_ = value.style;
        if (!style_.animated) moving_ = false;
        InvalidatePaint();
    }
    FrameResult OnFrame(huxerui::ViewNode& node, const huxerui::FrameInfo& frame) override {
        const bool visible = node.PresentationOpacity() > 0.001F &&
            node.PresentationBounds().Intersects(style_.region->bounds);
        const bool moving = style_.animated && !frame.reduced_motion && visible;
        if (moving_ != moving) {
            moving_ = moving;
            InvalidatePaint();
        }
        if (!style_.animated || frame.reduced_motion) return {};
        if (!geometryReady_) return {.needs_frame = true};
        if (!moving) return {};
        if (!style_.region->epoch) style_.region->epoch = frame.timestamp;
        const double elapsed = std::max(0.0, frame.timestamp - *style_.region->epoch);
        if (frame.timestamp - lastPaint_ >= kShimmerFrameInterval - 0.0001) {
            phase_ = static_cast<float>(std::fmod(elapsed, kShimmerPeriod) / kShimmerPeriod);
            lastPaint_ = frame.timestamp;
            InvalidatePaint();
        }
        return {.wake_after = kShimmerFrameInterval};
    }
    PaintInvalidation PrepareGeometry(huxerui::ViewNode& node, huxerui::TextMeasurer&) override {
        const auto bounds = node.PresentationBounds();
        const auto region = style_.region->bounds;
        const bool visible = node.PresentationOpacity() > 0.001F &&
            bounds.width > 0 && bounds.height > 0 && bounds.Intersects(region);
        const bool changed = bounds != bounds_ || region != region_ || visible != visible_;
        bounds_ = bounds;
        region_ = region;
        visible_ = visible;
        geometryReady_ = true;
        return changed ? PaintInvalidation::Foreground : PaintInvalidation::None;
    }
    void PaintAboveContent(const huxerui::ViewNode& node, huxerui::PaintContext& paint) const override {
        if (!moving_ || bounds_.width <= 0 || bounds_.height <= 0) {
            paint.DrawRect(node.Bounds(), style_.base, huxerui::CornerRadii{4.0F});
            return;
        }
        // One diagonal band in page coordinates also aligns newly materialized
        // virtual rows. Animation updates retained paint, never layout or State.
        const float halfBand = std::clamp(region_.width * 0.18F, 64.0F, 220.0F);
        const float center = region_.x - halfBand + phase_ * (region_.width + halfBand * 2.0F);
        huxerui::LinearGradient gradient{
            .start = {(center - halfBand - bounds_.x) / bounds_.width,
                      (region_.y - bounds_.y) / bounds_.height},
            .end = {(center + halfBand - bounds_.x) / bounds_.width,
                    (region_.y + halfBand * 0.4F - bounds_.y) / bounds_.height},
            .stops = {{0.0F, style_.base}, {0.5F, style_.highlight}, {1.0F, style_.base}},
        };
        paint.DrawRect(node.Bounds(), gradient, huxerui::CornerRadii{4.0F});
    }
private:
    SkeletonShimmerStyle style_;
    huxerui::Rect bounds_, region_;
    bool geometryReady_ = false;
    bool visible_ = false;
    bool moving_ = false;
    float phase_ = 0;
    double lastPaint_ = -1;
};

huxerui::View SkeletonCard(huxerui::View content, const huxerui::ThemeSpec& theme, float height) {
    const auto islands = ResolveIslandTheme(theme);
    return huxerui::Column {content}.With(
        huxerui::Padding(islands.island_padding), huxerui::Frame{.height = height},
        huxerui::Background(islands.raised), huxerui::CornerRadius(islands.nested_radius),
        huxerui::Border(islands.outline_soft, kCardOutlineWidth), huxerui::ClipChildren(),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace

SkeletonShimmerStyle UseSkeletonShimmer(const huxerui::ThemeSpec& theme, bool active) {
    const auto region = huxerui::UseState(std::make_shared<SkeletonShimmerRegion>());
    const auto base = Mix(ResolveIslandTheme(theme).raised, theme.colors.on_surface, 0.08F);
    const bool foreground = huxerui::UseWindow().LifecycleState() != huxerui::WindowLifecycleState::Background;
    return {base, Mix(base, huxerui::Color::White(), IsDarkTheme(theme) ? 0.10F : 0.65F),
            active && foreground && !theme.motion.reduced_motion, region.Get()};
}

huxerui::View SkeletonBlock(SkeletonShimmerStyle style, float width, float height) {
    return huxerui::Row {}.With(huxerui::Frame{.width = width, .height = height}, ShimmerPaint{style});
}

huxerui::View SkeletonLoadingRegion(huxerui::View content, SkeletonShimmerStyle style) {
    return huxerui::Column {content}.With(huxerui::Grow(1.0F), ShimmerViewport{style.region},
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch), huxerui::ClipChildren());
}

[[huxerui::composable]] huxerui::View PageLoadingSkeleton(
    const huxerui::ThemeSpec& theme, bool list, bool active) {
    const auto shimmer = UseSkeletonShimmer(theme, active);
    const auto item = [theme, list, shimmer](std::size_t index) -> huxerui::View {
        return SkeletonCard(huxerui::Column {
          SkeletonBlock(shimmer, index % 2 == 0 ? 112.0F : 88.0F, 16.0F),
          SkeletonBlock(shimmer, index % 2 == 0 ? 72.0F : 128.0F, 12.0F),
        }.With(huxerui::Spacing(12.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Start)),
            theme, list ? 76.0F : 108.0F).Key("page-loading-item-" + std::to_string(index));
    };
    huxerui::View content;
    if (list) {
        content = huxerui::VirtualList(8, [item](std::size_t index) {
            return huxerui::Column {item(index)}.With(
                huxerui::Padding(huxerui::EdgeInsets{.bottom = kSectionCardSpacing}),
                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
        }).EstimatedItemExtent(76.0F + kSectionCardSpacing).With(huxerui::Grow(1.0F));
    } else {
        content = huxerui::VirtualGrid(6, item)
            .Columns(huxerui::GridColumns::Adaptive(220.0F))
            .EstimatedRowExtent(108.0F)
            .ColumnSpacing(kSectionCardSpacing).RowSpacing(kSectionCardSpacing)
            .With(huxerui::Grow(1.0F));
    }
    return SkeletonLoadingRegion(content, shimmer).Key("page-loading-skeleton");
}

[[huxerui::composable]] huxerui::View ProxyLoadingSkeleton(const huxerui::ThemeSpec& theme) {
    const auto shimmer = UseSkeletonShimmer(theme);
    const auto card = [theme, shimmer] {
        return SkeletonCard(huxerui::Column {
          SkeletonBlock(shimmer, 112.0F, 16.0F),
          SkeletonBlock(shimmer, 68.0F, 12.0F),
        }.With(huxerui::Spacing(8.0F)), theme, 76.0F).With(huxerui::Grow(1.0F));
    };
    return SkeletonLoadingRegion(huxerui::Column {
      huxerui::Row {
        SkeletonBlock(shimmer, 92.0F, 20.0F).With(huxerui::Grow(1.0F)),
        SkeletonBlock(shimmer, 76.0F, 20.0F).With(huxerui::Grow(1.0F)),
        SkeletonBlock(shimmer, 104.0F, 20.0F).With(huxerui::Grow(1.0F)),
      }.With(huxerui::Spacing(16.0F)),
      huxerui::Row {card(), card()}.With(huxerui::Spacing(kSectionCardSpacing)),
    }.With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(kSectionCardSpacing, 12.0F)),
        huxerui::Spacing(16.0F), huxerui::Grow(1.0F),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)), shimmer).Key("proxy-loading-skeleton");
}

} // namespace clashflux::ui
