#include "page_loading.h"
#include "theme_colors.h"
#include "profile_card_layout.h"
#include "profile_list_layout.h"
#include "proxy_group_layout.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

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
        if (!moving_ || bounds_.width <= 0 || bounds_.height <= 0 || style_.highlights.empty()) {
            paint.DrawRect(node.Bounds(), style_.base, huxerui::CornerRadii{4.0F});
            return;
        }
        // One diagonal band in page coordinates also aligns newly materialized
        // virtual rows. Animation updates retained paint, never layout or State.
        const bool multiColor = style_.highlights.size() > 1U;
        const float halfBand = multiColor
            ? std::clamp(region_.width * 0.26F, 88.0F, 280.0F)
            : std::clamp(region_.width * 0.18F, 64.0F, 220.0F);
        const float center = region_.x - halfBand + phase_ * (region_.width + halfBand * 2.0F);
        std::vector<huxerui::GradientStop> stops;
        stops.reserve(style_.highlights.size() + 2U);
        stops.push_back({0.0F, style_.base});
        if (style_.highlights.size() == 1U) {
            stops.push_back({0.5F, style_.highlights.front()});
        } else {
            const float denom = static_cast<float>(style_.highlights.size() - 1U);
            for (std::size_t i = 0; i < style_.highlights.size(); ++i) {
                stops.push_back({0.20F + 0.60F * static_cast<float>(i) / denom, style_.highlights[i]});
            }
        }
        stops.push_back({1.0F, style_.base});
        huxerui::LinearGradient gradient{
            .start = {(center - halfBand - bounds_.x) / bounds_.width,
                      (region_.y - bounds_.y) / bounds_.height},
            .end = {(center + halfBand - bounds_.x) / bounds_.width,
                    (region_.y + halfBand * 0.4F - bounds_.y) / bounds_.height},
            .stops = std::move(stops),
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

huxerui::View SkeletonCard(huxerui::View content, const huxerui::ThemeSpec& theme, float height,
    std::optional<huxerui::EdgeInsets> padding = std::nullopt, std::optional<float> radius = std::nullopt) {
    const auto islands = ResolveIslandTheme(theme);
    return huxerui::Column {content}.With(
        huxerui::Padding(padding.value_or(huxerui::EdgeInsets::All(islands.island_padding))),
        huxerui::Frame{.height = height},
        huxerui::Background(islands.raised), huxerui::CornerRadius(radius.value_or(islands.nested_radius)),
        huxerui::ClipChildren(),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace

std::vector<huxerui::Color> ResolveSkeletonShimmerHighlights(
    const huxerui::ThemeSpec& theme, huxerui::Color base, const FluxGradientStyle& gradient) {
    const bool dark = IsDarkTheme(theme);
    if (gradient.enabled) {
        const auto palette = ResolveGradientPalette(gradient);
        const auto& stops = dark ? palette.darkStops : palette.lightStops;
        if (!stops.empty()) {
            std::vector<huxerui::Color> highlights;
            highlights.reserve(stops.size());
            const auto lift = dark ? base : Mix(base, huxerui::Color::White(), 0.50F);
            for (const auto& stop : stops) {
                highlights.push_back(Mix(lift, stop, 0.22F));
            }
            return highlights;
        }
    }
    return {Mix(base, huxerui::Color::White(), dark ? 0.10F : 0.65F)};
}

SkeletonShimmerStyle UseSkeletonShimmer(const huxerui::ThemeSpec& theme, bool active) {
    const auto region = huxerui::UseState(std::make_shared<SkeletonShimmerRegion>());
    const auto gradient = huxerui::UseEnvironment<FluxGradientStyle>();
    const auto base = Mix(ResolveIslandTheme(theme).raised, theme.colors.on_surface, 0.08F);
    const bool foreground = huxerui::UseWindow().LifecycleState() != huxerui::WindowLifecycleState::Background;
    return {base, ResolveSkeletonShimmerHighlights(theme, base, gradient),
            active && foreground && !theme.motion.reduced_motion, region.Get()};
}

huxerui::View SkeletonBlock(SkeletonShimmerStyle style, float width, float height) {
    return huxerui::Row {}.With(huxerui::Frame{.width = width, .height = height}, ShimmerPaint{style});
}

huxerui::View SkeletonLoadingRegion(huxerui::View content, SkeletonShimmerStyle style) {
    return PageBodyFrame(huxerui::Column {content}.With(huxerui::Grow(1.0F), ShimmerViewport{style.region},
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch), huxerui::ClipChildren()))
        .With(huxerui::Grow(1.0F));
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
        content = PageVirtualList(8, [item, theme](std::size_t index) {
            return huxerui::Column {item(index)}.With(
                huxerui::Padding(huxerui::EdgeInsets{
                    .bottom = index == 7 ? 0.0F : PageScrollInset(theme, true)}),
                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
        }, ScrollAreaInsets::Dense()).EstimatedItemExtent(76.0F + PageScrollInset(theme, true))
            .With(huxerui::Grow(1.0F));
    } else {
        content = PageVirtualGrid(6, item, kSectionCardSpacing)
            .Columns(huxerui::GridColumns::Adaptive(220.0F))
            .EstimatedRowExtent(108.0F)
            .ColumnSpacing(kSectionCardSpacing)
            .With(huxerui::Grow(1.0F));
    }
    return SkeletonLoadingRegion(content, shimmer).Key("page-loading-skeleton");
}

[[huxerui::composable]] huxerui::View ProfileLoadingSkeleton(
    const huxerui::ThemeSpec& theme, bool compact, bool groupByType,
    huxerui::State<std::size_t> gridColumns, bool active) {
    const auto shimmer = UseSkeletonShimmer(theme, active);
    const auto tasks = huxerui::UseTaskScope();
    const auto item = [theme, compact, shimmer](std::size_t index) -> huxerui::View {
        std::vector<huxerui::View> rows{
            huxerui::Row {
              SkeletonBlock(shimmer, 140.0F, 16.0F).With(huxerui::Grow(1.0F)),
              SkeletonBlock(shimmer, 20.0F, 20.0F),
            }.With(huxerui::Frame{.height = compact
                ? CompactProfileCardHeadingHeight(false) : kWideProfileCardHeadingHeight},
                huxerui::Spacing(12.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        };
        if (!compact) {
            for (float width : {180.0F, 112.0F, 64.0F})
                rows.push_back(huxerui::Row {
                  SkeletonBlock(shimmer, width, 12.0F),
                }.With(huxerui::Frame{.height = kProfileCardTextRowHeight},
                    huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
        }
        rows.push_back(huxerui::Row {
          SkeletonBlock(shimmer, 112.0F, 12.0F).With(huxerui::Grow(1.0F)),
          SkeletonBlock(shimmer, 72.0F, 12.0F),
        }.With(huxerui::Frame{.height = kProfileCardTextRowHeight}, huxerui::Spacing(6.0F),
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
        rows.push_back(huxerui::Row {
          SkeletonBlock(shimmer, 0.0F, kProfileQuotaBarHeight).With(huxerui::Grow(1.0F)),
        }.Key("profile-loading-quota-bar"));
        rows.push_back(huxerui::Row {
          SkeletonBlock(shimmer, 96.0F, 11.0F),
        }.With(huxerui::Frame{.height = kProfileCardTextRowHeight},
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
        return SkeletonCard(ProfileCardRows(std::move(rows)),
            theme, ProfileCardHeight(compact), huxerui::EdgeInsets::Symmetric(10.0F, 8.0F), kProfileCardRadius)
            .Key("profile-loading-item-" + std::to_string(index));
    };
    const auto grid = ApplyProfileGridColumns(PageVirtualGrid(8, item, kProfileCardSpacing,
        ScrollAreaInsets::Symmetric(kProfileCardSpacing, compact ? kCompactFloatingNavigationInset : 0.0F)),
        compact, groupByType, gridColumns, tasks)
        .EstimatedRowExtent(ProfileCardHeight(compact))
        .ColumnSpacing(kProfileCardSpacing)
        .With(huxerui::Grow(1.0F));
    return SkeletonLoadingRegion(UnifiedProfileList(grid), shimmer).Key("profile-loading-skeleton");
}

[[huxerui::composable]] huxerui::View ProxyLoadingSkeleton(
    const huxerui::ThemeSpec& theme, bool compact, bool grouped, bool active) {
    const auto shimmer = UseSkeletonShimmer(theme, active);
    const float bottomAvoidance = compact ? kCompactFloatingNavigationInset : 0.0F;
    if (grouped) {
        constexpr std::size_t kGroupCount = 6;
        constexpr float kOverviewDotSize = 7.0F;
        constexpr float kGroupCardHeight = kProxyGroupHeaderHeight + kOverviewDotSize + kProxyGroupInset;
        const auto islands = ResolveIslandTheme(theme);
        const float groupGap = islands.nested_radius;
        auto cardPadding = huxerui::EdgeInsets::Symmetric(kProxyGroupInset, 0.0F);
        cardPadding.bottom = kProxyGroupInset;
        auto listInsets = ScrollAreaInsets{
            .bottom = kSectionCardSpacing,
            .bottom_avoidance = bottomAvoidance,
            .rail_bottom_avoidance = 0.0F,
        };
        auto list = PageVirtualList(kGroupCount,
            [theme, shimmer, islands, groupGap, cardPadding](std::size_t index) -> huxerui::View {
                constexpr float kDotSize = 7.0F;
                constexpr float kCardHeight = kProxyGroupHeaderHeight + kDotSize + kProxyGroupInset;
                const float titleWidth = index % 2 == 0 ? 112.0F : 92.0F;
                std::vector<huxerui::View> dots;
                dots.reserve(10);
                for (std::size_t d = 0; d < 10; ++d) {
                    dots.push_back(SkeletonBlock(shimmer, kDotSize, kDotSize));
                }
                auto card = SkeletonCard(
                    huxerui::Column {
                      huxerui::Row {
                        huxerui::Row {
                          SkeletonBlock(shimmer, titleWidth, 16.0F),
                          SkeletonBlock(shimmer, 56.0F, 12.0F),
                        }.With(huxerui::Spacing(8.0F), huxerui::Grow(1.0F),
                            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
                        huxerui::Row {
                          SkeletonBlock(shimmer, 28.0F, 18.0F),
                          SkeletonBlock(shimmer, 24.0F, 24.0F),
                        }.With(huxerui::Spacing(8.0F),
                            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
                      }.With(huxerui::Frame{.height = kProxyGroupHeaderHeight},
                          huxerui::Spacing(8.0F),
                          huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
                      huxerui::Row(std::move(dots)).With(
                          huxerui::Frame{.height = kDotSize},
                          huxerui::Spacing(4.0F),
                          huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
                    }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
                    theme, kCardHeight, cardPadding, islands.nested_radius)
                    .Key("proxy-loading-group-" + std::to_string(index));
                return huxerui::Column {std::move(card)}.With(
                    huxerui::Padding(huxerui::EdgeInsets{
                        .bottom = index + 1 < 6 ? groupGap : 0.0F}),
                    huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
            }, listInsets)
            .EstimatedItemExtent(kGroupCardHeight + groupGap)
            .With(huxerui::Grow(1.0F));
        return SkeletonLoadingRegion(std::move(list), shimmer).Key("proxy-loading-skeleton");
    }

    constexpr std::size_t kNodeCount = 12;
    constexpr float kNodeCardHeight = 56.0F;
    auto gridInsets = ScrollAreaInsets::Symmetric(kSectionCardSpacing, bottomAvoidance);
    gridInsets.rail_bottom_avoidance = 0.0F;
    auto tabBar = huxerui::Row {
      SkeletonBlock(shimmer, 84.0F, 24.0F),
      SkeletonBlock(shimmer, 72.0F, 24.0F),
      SkeletonBlock(shimmer, 96.0F, 24.0F),
    }.With(
        huxerui::Padding(huxerui::EdgeInsets::Symmetric(kSectionCardSpacing, 8.0F)),
        huxerui::Spacing(12.0F),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
        .Key("proxy-loading-tabs");
    auto grid = PageVirtualGrid(kNodeCount,
        [theme, shimmer](std::size_t index) -> huxerui::View {
            const float titleWidth = index % 2 == 0 ? 108.0F : 88.0F;
            return SkeletonCard(
                huxerui::Column {
                  SkeletonBlock(shimmer, titleWidth, 14.0F),
                  huxerui::Row {
                    SkeletonBlock(shimmer, 48.0F, 11.0F),
                    SkeletonBlock(shimmer, 28.0F, 11.0F),
                  }.With(huxerui::MainAlign(huxerui::MainAxisAlignment::SpaceBetween),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
                }.With(huxerui::Spacing(10.0F),
                    huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
                theme, 56.0F,
                huxerui::EdgeInsets::Symmetric(10.0F, 8.0F),
                kSelectableTileRadius).Key("proxy-loading-node-" + std::to_string(index));
        }, kSectionCardSpacing, gridInsets)
        .Columns(compact ? huxerui::GridColumns::Fixed(2) : huxerui::GridColumns::Adaptive(kProxyNodeWidth))
        .EstimatedRowExtent(kNodeCardHeight)
        .ColumnSpacing(kSectionCardSpacing)
        .With(huxerui::Grow(1.0F));
    return SkeletonLoadingRegion(
        huxerui::Column {std::move(tabBar), std::move(grid)}.With(
            huxerui::Grow(1.0F),
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
        shimmer).Key("proxy-loading-skeleton");
}

} // namespace clashflux::ui
