#pragma once

#include <memory>
#include <vector>
#include <huxerui/huxerui.h>
#include "page_layout.h"
#include "theme_colors.h"

namespace clashflux::ui {

struct SkeletonShimmerRegion;
struct SkeletonShimmerStyle {
    huxerui::Color base;
    std::vector<huxerui::Color> highlights;
    bool animated = false;
    std::shared_ptr<SkeletonShimmerRegion> region;
    bool operator==(const SkeletonShimmerStyle&) const = default;
};

std::vector<huxerui::Color> ResolveSkeletonShimmerHighlights(
    const huxerui::ThemeSpec& theme, huxerui::Color base, const FluxGradientStyle& gradient);

// Resolve hooks before capturing this owning style in deferred virtual-item factories.
SkeletonShimmerStyle UseSkeletonShimmer(const huxerui::ThemeSpec& theme, bool active = true);
huxerui::View SkeletonBlock(SkeletonShimmerStyle style, float width, float height);
huxerui::View SkeletonLoadingRegion(huxerui::View content, SkeletonShimmerStyle style);

// First real snapshot wins immediately; there is no minimum loading duration.
huxerui::View PageLoadingSkeleton(const huxerui::ThemeSpec& theme, bool list = false, bool active = true);
huxerui::View ProxyLoadingSkeleton(const huxerui::ThemeSpec& theme);
huxerui::View ProfileLoadingSkeleton(const huxerui::ThemeSpec& theme,
    bool compact, bool groupByType, huxerui::State<std::size_t> gridColumns, bool active = true);

inline huxerui::View PageLoadingScaffold(huxerui::StringVariant title,
    const huxerui::ThemeSpec& theme, bool list = false) {
    return PageScaffold(title, {}, PageLoadingSkeleton(theme, list), false, false, true);
}

} // namespace clashflux::ui
