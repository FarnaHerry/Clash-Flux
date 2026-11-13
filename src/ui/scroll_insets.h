#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include "ui.h"

namespace clashflux::ui {

// One request for the scrollable content edges and the rail's end clearance.
// Floating controls add bottom avoidance without shortening the viewport.
struct ScrollAreaInsets {
    std::optional<float> top, bottom;
    float bottom_avoidance = 0.0F;
    bool dense = false;
    bool top_follows_horizontal = false;
    static ScrollAreaInsets Default() { return {}; }
    static ScrollAreaInsets Dense(float avoidance = 0.0F) {
        return {.bottom_avoidance = avoidance, .dense = true};
    }
    static ScrollAreaInsets Symmetric(float inset, float avoidance = 0.0F) {
        return {.top = inset, .bottom = inset, .bottom_avoidance = avoidance};
    }
    static ScrollAreaInsets None() { return Symmetric(0.0F); }
    bool operator==(const ScrollAreaInsets&) const = default;
};

inline float PageScrollInset(const huxerui::ThemeSpec& theme, bool dense = false) {
    const auto islands = ResolveIslandTheme(theme);
    return dense ? islands.dense_radius : islands.nested_radius;
}

inline huxerui::EdgeInsets ResolveScrollAreaInsets(const huxerui::ThemeSpec& theme, ScrollAreaInsets request) {
    const auto positive = [](float value) { return std::isfinite(value) ? std::max(0.0F, value) : 0.0F; };
    const float fallback = PageScrollInset(theme, request.dense);
    // Page-leading space is independent of the tighter gaps between dense rows.
    const float leading = request.dense ? std::max(fallback, kSectionCardSpacing) : fallback;
    return {.top = positive(request.top.value_or(leading)),
        .bottom = positive(request.bottom.value_or(fallback)) + positive(request.bottom_avoidance)};
}

inline huxerui::View PageScrollContent(huxerui::View content) {
    return huxerui::Scope([content] {
        const auto insets = ResolveScrollAreaInsets(huxerui::UseTheme(),
            huxerui::UseEnvironment<ScrollAreaInsets>());
        return huxerui::Column {content}.With(huxerui::Padding(insets),
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    });
}

inline huxerui::View PageScrollSpace(float height) {
    // Zero measured extents retain HUI's estimated card height. A positive
    // epsilon collapses visually while still replacing that estimate.
    return huxerui::Row {}.With(
        huxerui::Frame{.height = std::max(std::numeric_limits<float>::epsilon(), height)},
        huxerui::Semantics{.hidden = true});
}

inline huxerui::View ScrollContentEdge(bool trailing, float rowSpacing,
    std::optional<ScrollAreaInsets> requested) {
    return huxerui::Scope([=] {
        const auto inherited = huxerui::UseEnvironment<ScrollAreaInsets>();
        const auto insets = ResolveScrollAreaInsets(huxerui::UseTheme(), requested.value_or(inherited));
        return PageScrollSpace((trailing ? insets.bottom : insets.top) - rowSpacing);
    });
}

// Business indices and keys stay unchanged. Edge items read the same request
// as their owning ScrollArea; skeletons may supply a request without a rail.
template<class Factory>
huxerui::VirtualList PageVirtualList(std::size_t count, Factory factory,
    std::optional<ScrollAreaInsets> insets = {}) {
    return huxerui::VirtualList(count + 2, [=](std::size_t index) -> huxerui::View {
        if (index == 0) return ScrollContentEdge(false, 0.0F, insets);
        if (index == count + 1) return ScrollContentEdge(true, 0.0F, insets);
        return factory(index - 1);
    });
}

template<class Factory>
huxerui::VirtualGrid PageVirtualGrid(std::size_t count, Factory factory, float rowSpacing,
    std::optional<ScrollAreaInsets> insets = {}) {
    const float gap = count == 0 ? 0.0F : rowSpacing;
    std::vector<std::size_t> spans(count + 2, 1);
    spans.front() = spans.back() = std::numeric_limits<std::size_t>::max();
    return huxerui::VirtualGrid(count + 2, [=](std::size_t index) -> huxerui::View {
        if (index == 0) return ScrollContentEdge(false, gap, insets);
        if (index == count + 1) return ScrollContentEdge(true, gap, insets);
        return factory(index - 1);
    }).ItemSpans(std::move(spans)).RowSpacing(gap);
}

} // namespace clashflux::ui
