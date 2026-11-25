#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app_resources.h"
#include "page_layout.h"

namespace clashflux::ui {

inline constexpr float kProxyGroupInset = 12.0F;
inline constexpr float kProxyNodeWidth = 196.0F;
inline constexpr float kProxyGroupHeaderHeight = 48.0F;

struct ProxyGroupRow {
    struct Value {
        std::size_t headerIndex;
        bool expanded;
        // Only the last row of an expanded group carries its trailing gap (zero at the end of the list).
        std::optional<float> trailingGap;
    };
};

struct ProxyGroupHeaderName {
    using Value = std::string;
};

struct PinnedProxyGroup {
    using Value = std::shared_ptr<PinnedProxyGroup>;
    struct HeaderPosition {
        std::size_t index;
        float top;
    };
    struct ToggleAnchor {
        HeaderPosition position;
        std::string name;
        bool rowsInvalidated = false;
    };
    std::optional<std::size_t> index;
    std::string pinnedName;
    bool scrollChanged = false;
    std::shared_ptr<PageScrollOffset> remembered;
    std::vector<HeaderPosition> visibleHeaders;
    std::optional<ToggleAnchor> toggleAnchor;
    std::optional<std::size_t> retainedEndItemCount;

    void AnchorBeforeToggle(std::size_t header, const std::string& name) {
        const auto visible = std::ranges::find(visibleHeaders, header, &HeaderPosition::index);
        if (visible == visibleHeaders.end()) return;
        toggleAnchor = ToggleAnchor{*visible, name};
    }
};

struct TrackProxyGroupScroll {
    class Extension;
    std::shared_ptr<PinnedProxyGroup> position;
    bool operator==(const TrackProxyGroupScroll&) const = default;
};

class TrackProxyGroupScroll::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode&, const TrackProxyGroupScroll& spec) : position_(spec.position) {
        if (position_) position_->scrollChanged = true;
    }
    void Update(huxerui::ViewNode&, const TrackProxyGroupScroll& spec) { position_ = spec.position; }
    void OnScrollActivity(huxerui::ViewNode&, const huxerui::ScrollActivity& activity) override {
        if (position_ && activity.phase == huxerui::ScrollPhase::Update && activity.delta != 0.0F &&
            activity.source != huxerui::ScrollSource::Overscroll) position_->scrollChanged = true;
    }
    PaintInvalidation PrepareGeometry(huxerui::ViewNode&, huxerui::TextMeasurer&) override {
        // Parent layouts can measure the list at multiple widths before placing it.
        if (position_) position_->toggleAnchor.reset();
        return PaintInvalidation::None;
    }

private:
    std::shared_ptr<PinnedProxyGroup> position_;
};

class ProxyGroupRowMetrics {
public:
    void Prepare(std::size_t count, std::optional<std::size_t> unchangedRows) {
        if (count == Count() && !unchangedRows) return;
        const std::size_t keep = std::min(unchangedRows.value_or(0), Count());
        extents_.resize(keep);
        extents_.resize(count, 56.0F);
        // Previously measured heights remain estimates when the available width changes.
        sums_.assign(count + 1, 0.0);
        for (std::size_t i = 1; i <= count; ++i) {
            sums_[i] += extents_[i - 1];
            const auto parent = i + LowBit(i);
            if (parent <= count) sums_[parent] += sums_[i];
        }
    }

    std::size_t Count() const { return extents_.size(); }
    float Extent(std::size_t index) const { return extents_[index]; }

    float Offset(std::size_t index) const {
        double offset = 0.0;
        for (; index > 0; index -= LowBit(index)) offset += sums_[index];
        return static_cast<float>(offset);
    }

    std::size_t IndexAt(double offset) const {
        if (extents_.empty()) return 0;
        std::size_t index = 0;
        for (auto step = std::bit_floor(Count()); step != 0; step >>= 1) {
            const auto next = index + step;
            if (next <= Count() && sums_[next] <= offset) {
                offset -= sums_[next];
                index = next;
            }
        }
        return std::min(index, Count() - 1);
    }

    void Measure(std::size_t index, float height) {
        const double delta = static_cast<double>(height) - std::exchange(extents_[index], height);
        if (delta == 0.0) return;
        for (++index; index <= Count(); index += LowBit(index)) sums_[index] += delta;
    }

private:
    static std::size_t LowBit(std::size_t index) { return index & (~index + 1); }
    std::vector<float> extents_;
    std::vector<double> sums_;
};

class StickyProxyGroupList final : public huxerui::VirtualLayout<StickyProxyGroupList> {
public:
    using VirtualLayout::VirtualLayout;
    static std::optional<float> ScrollOffsetForItem(huxerui::ViewNode& node, std::size_t index,
        huxerui::ScrollAlignment alignment, float extent) {
        const auto& rows = node.Cache<ProxyGroupRowMetrics>();
        if (index >= rows.Count()) return std::nullopt;
        const float start = rows.Offset(index);
        const float available = extent - rows.Extent(index);
        if (alignment == huxerui::ScrollAlignment::Center) return start - available * 0.5F;
        if (alignment == huxerui::ScrollAlignment::End) return start - available;
        return start;
    }

    static huxerui::VirtualLayoutResult Measure(huxerui::VirtualLayoutContext& context,
        huxerui::ViewNode& node, huxerui::Constraints constraints) {
        const auto position = node.LayoutValueOr<PinnedProxyGroup>({});
        // Content reflow can move the measured viewport anchor without a scroll. Keep the existing pin in that case.
        const bool selectPin = !position || (std::exchange(position->scrollChanged, false) && !position->toggleAnchor);
        const auto previousPin = position ? std::exchange(position->index, std::nullopt) : std::nullopt;
        auto* anchor = position && position->toggleAnchor ? &*position->toggleAnchor : nullptr;
        if (anchor) {
            const auto* name = anchor->position.index < context.ItemCount()
                ? context.Item(anchor->position.index).LayoutValue<ProxyGroupHeaderName>() : nullptr;
            if (!name || *name != anchor->name) anchor = nullptr;
        }
        auto& rows = node.Cache<ProxyGroupRowMetrics>();
        const auto viewport = context.Viewport();
        const auto unchangedRows = anchor && !std::exchange(anchor->rowsInvalidated, true)
            ? std::optional{anchor->position.index + 1} : std::nullopt;
        rows.Prepare(context.ItemCount(), unchangedRows);
        const bool retainEnd = position && (anchor || position->retainedEndItemCount == context.ItemCount());
        const auto normal = MeasureRows(context, constraints, rows, anchor ? &anchor->position : nullptr, retainEnd);
        const float viewportTop = *normal.CorrectedScrollOffset();
        if (position) {
            position->visibleHeaders.clear();
            position->retainedEndItemCount = normal.ContentSize().height > rows.Offset(rows.Count())
                ? std::optional{rows.Count()} : std::nullopt;
            if (anchor && position->remembered) position->remembered->value = viewportTop;
        }
        std::optional<std::size_t> active;
        const float viewportBottom = viewportTop + viewport.size.height;
        for (const auto& placement : normal.Placements()) {
            const float top = placement.offset.y;
            if (top >= viewportBottom) break;
            const auto* row = placement.item->LayoutValue<ProxyGroupRow>();
            if (row && top + placement.item->LayoutSize().height > viewportTop) {
                const bool eligible = selectPin ? top <= viewportTop : previousPin == row->headerIndex;
                if (row->expanded && eligible) active = row->headerIndex;
                if (position && row->headerIndex == placement.collection_item->index) {
                    position->visibleHeaders.push_back({row->headerIndex, top - viewportTop});
                }
            }
        }
        if (!active) return normal;
        if (rows.Offset(*active) >= viewportTop) return normal;

        // If the tail is outside the realized range, it is far enough below the header to leave it unconstrained.
        float headerBottomLimit = std::numeric_limits<float>::infinity();
        for (const auto& placement : normal.Placements()) {
            const auto* row = placement.item->LayoutValue<ProxyGroupRow>();
            if (row && row->headerIndex == *active && row->trailingGap) {
                // Leave the last row's rounded bottom padding visible below the header.
                headerBottomLimit = placement.offset.y + placement.item->LayoutSize().height
                    - *row->trailingGap - kProxyGroupInset;
                break;
            }
        }
        if (headerBottomLimit <= viewportTop) return normal;

        // Keep the same native header mounted even when its natural row leaves the cache.
        auto& header = context.Item(*active);
        const auto* name = header.LayoutValue<ProxyGroupHeaderName>();
        if (!selectPin && (!name || *name != position->pinnedName)) return normal;
        const float width = normal.MeasuredSize().width;
        const auto size = context.Measure(header, {width, width, 0.0F, std::numeric_limits<float>::infinity()});
        const float pinnedTop = std::min(viewportTop, headerBottomLimit - size.height);
        if (position) {
            position->index = active;
            if (selectPin) position->pinnedName = name ? *name : std::string{};
            if (!position->visibleHeaders.empty() && position->visibleHeaders.front().index == *active)
                position->visibleHeaders.front().top = pinnedTop - viewportTop;
            else position->visibleHeaders.push_back({*active, pinnedTop - viewportTop});
        }

        huxerui::VirtualLayoutResult result;
        result.SetSize(normal.MeasuredSize()).SetContentSize(normal.ContentSize()).SetScrollOffset(viewportTop)
            .SetCollectionSemantics(huxerui::SemanticRole::List, huxerui::SemanticRole::ListItem,
                {.item_count = context.ItemCount(), .row_count = context.ItemCount(), .column_count = 1});
        for (const auto& placement : normal.Placements()) {
            if (placement.item != &header) result.Place(*placement.item, placement.offset, *placement.collection_item);
        }
        // The last placement draws above the node rows and wins hit testing in the same scroll container.
        result.Place(header, {0.0F, pinnedTop}, {.index = *active, .row_index = *active, .column_index = 0});
        return result;
    }

private:
    static huxerui::VirtualLayoutResult MeasureRows(huxerui::VirtualLayoutContext& context,
        huxerui::Constraints constraints, ProxyGroupRowMetrics& rows,
        const PinnedProxyGroup::HeaderPosition* disclosure, bool retainEnd) {
        const auto viewport = context.Viewport();
        const auto count = rows.Count();
        const auto offset = [&] {
            const float requested = disclosure ? std::max(0.0F, rows.Offset(disclosure->index) - disclosure->top)
                                               : viewport.offset.y;
            const float maximum = std::max(0.0F, rows.Offset(count) - viewport.size.height);
            return retainEnd ? requested : std::min(requested, maximum);
        };
        const float width = viewport.size.width;
        const huxerui::Constraints itemConstraints{width, width, 0.0F, std::numeric_limits<float>::infinity()};
        const float cache = std::max(200.0F, viewport.size.height * 0.5F);
        const auto rangeFor = [&](float top) {
            return std::pair{rows.IndexAt(std::max(0.0F, top - cache)),
                std::min(count, rows.IndexAt(top + viewport.size.height + cache) + 1)};
        };
        const auto measureRange = [&](std::size_t first, std::size_t end) {
            for (auto i = first; i < end; ++i)
                rows.Measure(i, context.Measure(context.Item(i), itemConstraints).height);
        };
        auto range = rangeFor(offset());
        auto covered = range;
        measureRange(range.first, range.second);
        for (int pass = 0; pass < 4; ++pass) {
            const auto refined = rangeFor(offset());
            if (range == refined) break;
            const auto [first, end] = refined;
            if (end <= covered.first || first >= covered.second) {
                measureRange(first, end);
                covered = refined;
            } else {
                measureRange(first, covered.first);
                measureRange(covered.second, end);
                covered = {std::min(first, covered.first), std::max(end, covered.second)};
            }
            range = refined;
        }

        const float top = offset();
        const float height = rows.Offset(count);
        // As the user scrolls up, the spare extent at the end retreats with the viewport.
        const float contentHeight = retainEnd && top > 0.0F ? std::max(height, top + viewport.size.height) : height;
        huxerui::VirtualLayoutResult result;
        result.SetSize(constraints.Constrain({width, height})).SetContentSize({width, contentHeight}).SetScrollOffset(top)
            .SetCollectionSemantics(huxerui::SemanticRole::List, huxerui::SemanticRole::ListItem,
                {.item_count = count, .row_count = count, .column_count = 1});
        for (auto i = range.first; i < range.second; ++i)
            result.Place(context.Item(i), {0.0F, rows.Offset(i)}, {.index = i, .row_index = i, .column_index = 0});
        return result;
    }
};

template<class Factory>
StickyProxyGroupList MakeStickyProxyGroupList(std::size_t count, Factory factory,
    std::shared_ptr<PinnedProxyGroup> pinned) {
    return StickyProxyGroupList(count + 2, [=](std::size_t index) -> huxerui::View {
        if (index == 0 || index == count + 1) return ScrollContentEdge(index != 0, 0.0F, {});
        return factory(index - 1);
    }).LayoutValue<PinnedProxyGroup>(pinned)
        .With(TrackProxyGroupScroll{std::move(pinned)}, huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

inline huxerui::View ProxyGroupHeader(std::string title, std::string type, std::size_t nodeCount,
    bool degraded, bool expanded, std::string selectedNode, huxerui::View speed, std::function<void()> toggle) {
    return huxerui::Scope([=] {
        const auto& theme = huxerui::UseTheme();
        const auto islands = ResolveIslandTheme(theme);
        constexpr float controlSize = 32.0F;
        constexpr float controlGap = 4.0F;
        constexpr float verticalInset = 6.0F;
        const huxerui::TextStyle caption{
            huxerui::Font::System(font_size::kCaption), theme.colors.on_surface_variant};
        const auto heading = huxerui::Text(title).LineLimit(1).Overflow(huxerui::TextOverflow::Ellipsis)
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kBody)
                .WithWeight(huxerui::FontWeight::Medium), theme.colors.on_surface})
            .With(huxerui::LayoutPriority(1.0F)).Key("proxy-group-title");
        const auto count = huxerui::Stack {
          huxerui::Text(std::to_string(nodeCount)).LineLimit(1).Style(caption),
        }.With(huxerui::Align(huxerui::HorizontalAlignment::Center, huxerui::VerticalAlignment::Center),
               huxerui::Padding(huxerui::EdgeInsets::Symmetric(6.0F, 0.0F)),
               huxerui::Frame{.min_width = 24.0F, .min_height = 22.0F},
               huxerui::Background(islands.active), huxerui::CornerRadius(theme.shapes.small))
            .Key("proxy-group-count");
        const auto& iconStyle = huxerui::UseEnvironment<huxerui::IconButtonStyle>();
        const auto action = Localized(expanded ? "收起" : "展开");
        auto arrow = huxerui::Stack {
          huxerui::Image(app::images::chevron_down)
              .Tint(toggle ? theme.colors.on_surface_variant : iconStyle.disabled_foreground)
              .With(huxerui::Frame{.width = iconStyle.icon_size, .height = iconStyle.icon_size},
                    huxerui::Rotation(expanded ? 180.0F : 0.0F)),
        }.With(huxerui::Frame{.width = controlSize, .height = controlSize},
               huxerui::Align(huxerui::HorizontalAlignment::Center, huxerui::VerticalAlignment::Center))
            .Key("proxy-group-expand");
        if (toggle) arrow = std::move(arrow).With(huxerui::Tooltip(action));
        const auto warning = degraded ? huxerui::View{huxerui::Text("!").Style(
            huxerui::TextStyle{huxerui::Font::System(font_size::kCaption), theme.colors.error})}
            : huxerui::View{};
        auto metadata = huxerui::Row {count, speed}.With(
            huxerui::Padding(huxerui::EdgeInsets{.right = controlGap}),
            huxerui::Frame{.min_height = kProxyGroupHeaderHeight},
            huxerui::Spacing(controlGap), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
        if (toggle) {
            // Keep the badge and the busy indicator passive without claiming the scroll gesture.
            metadata = std::move(metadata).With(huxerui::PointerCursor(huxerui::PointerCursorKind::Default))
                .On<huxerui::ViewEvents::Pointer>([](const huxerui::PointerEvent&) {});
        }
        auto header = huxerui::Row {
          huxerui::Row {
            heading,
            huxerui::Text(type).LineLimit(1).Overflow(huxerui::TextOverflow::Ellipsis).Style(caption),
            warning,
          }.With(
              huxerui::Padding(huxerui::EdgeInsets{
                  .top = verticalInset, .right = controlGap, .bottom = verticalInset, .left = kProxyGroupInset}),
              huxerui::Spacing(6.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
              huxerui::ClipChildren(), huxerui::Frame{.min_height = kProxyGroupHeaderHeight}, huxerui::Grow(1.0F))
              .Key("proxy-group-title-region"),
          metadata,
          arrow,
        }.With(huxerui::Padding(huxerui::EdgeInsets{.right = kProxyGroupInset}),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
            .Key("proxy-group-header");
        if (!toggle) {
            // A frozen read-only header still shields the node rows behind it.
            return std::move(header).On<huxerui::ViewEvents::Pointer>([](const huxerui::PointerEvent&) {});
        }

        huxerui::Semantics semantics{
            .role = huxerui::SemanticRole::Button, .label = title, .hint = action, .expanded = expanded};
        if (!selectedNode.empty()) semantics.state_description = selectedNode;
        huxerui::Indication indication{.focus = theme.interactions.indication.focus,
                                       .ripple = theme.interactions.indication.ripple};
        if (indication.ripple) {
            indication.ripple->placement = huxerui::IndicationPlacement::BehindContent;
            indication.ripple->expansion = huxerui::TweenSpec{.duration = 0.24, .easing = huxerui::Easing::EaseOut};
            indication.ripple->fade_out = huxerui::TweenSpec{.duration = 0.36, .easing = huxerui::Easing::Linear};
        }
        return std::move(header).With(indication, huxerui::Focusable(true),
            huxerui::PointerCursor(huxerui::PointerCursorKind::Hand), semantics).OnClick(toggle);
    });
}

// The list virtualizes individual rows. Each row paints its part of the
// same panel, so a large expanded group never needs to mount all its nodes.
inline huxerui::View ProxyGroupPanelPart(huxerui::View content,
    bool first, bool last, bool followedByGroup) {
    return huxerui::Scope([=] {
        const auto islands = ResolveIslandTheme(huxerui::UseTheme());
        const float radius = islands.nested_radius;
        const float top = first ? radius : 0.0F;
        const float bottom = last ? radius : 0.0F;
        return huxerui::Column {
          huxerui::Column {content}.With(
              huxerui::Background(islands.raised), huxerui::CornerRadius(top, top, bottom, bottom),
              huxerui::ClipChildren(), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
          followedByGroup
              ? huxerui::View{huxerui::Spacer().With(huxerui::Frame{.height = radius})}
              : huxerui::View{},
        }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    });
}

inline huxerui::View ProxyGroupNodeRow(std::vector<huxerui::View> nodes, std::size_t columns, bool last) {
    // Keep the last incomplete row aligned with the complete rows.
    while (nodes.size() < columns) {
        nodes.push_back(huxerui::Spacer().With(huxerui::Semantics{.hidden = true}));
    }
    return huxerui::Row(std::move(nodes)).With(
        huxerui::Padding(huxerui::EdgeInsets{
            .right = kProxyGroupInset, .bottom = last ? kProxyGroupInset : kSectionCardSpacing,
            .left = kProxyGroupInset}),
        huxerui::Spacing(kSectionCardSpacing),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

struct MeasureProxyGroupColumns {
    class Extension;
    huxerui::State<std::size_t> columns;
    huxerui::TaskScope tasks;
    bool compact;
};

class MeasureProxyGroupColumns::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode&, const MeasureProxyGroupColumns& spec) : spec_(spec) {}

    void Update(huxerui::ViewNode&, const MeasureProxyGroupColumns& spec) { spec_ = spec; }

    PaintInvalidation PrepareGeometry(huxerui::ViewNode& node, huxerui::TextMeasurer&) override {
        const float width = node.ContentBounds().width;
        if (width <= 0.0F || spec_.compact) return PaintInvalidation::None;
        const auto count = huxerui::GridColumns::Adaptive(kProxyNodeWidth).Resolve(
            std::max(0.0F, width - 2.0F * kProxyGroupInset), kSectionCardSpacing);
        if (spec_.columns.Get() != count) {
            spec_.tasks.Post([columns = spec_.columns, count] { columns = count; });
        }
        return PaintInvalidation::None;
    }

private:
    MeasureProxyGroupColumns spec_;
};

} // namespace clashflux::ui
