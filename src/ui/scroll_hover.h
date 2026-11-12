#pragma once

#include <algorithm>
#include <memory>
#include <vector>
#include <huxerui/huxerui.h>

namespace clashflux::ui {

// Overflow below this is not a usable scroll target; keeps estimate jitter out.
inline constexpr float kScrollHoverMinimumOverflow = 0.5F;

struct ScrollHoverRegion {
    huxerui::ScrollController controller;
    huxerui::State<bool> selected{false};
    std::size_t depth = 0;
    bool hovered = false, rail_hovered = false, enabled = false;
};

// Hover events follow the mounted hit route, so clipped and occluded siblings
// leave automatically. Only nested ScrollAreas share this selection channel.
// Exactly one target is selected: the deepest hovered, enabled container with
// real overflow. Scroll direction and offset never take part, so scrolling
// cannot add, remove, or move the hover highlight.
struct ScrollHoverRoute {
    std::vector<std::weak_ptr<ScrollHoverRegion>> regions;

    void Refresh() {
        std::erase_if(regions, [](const auto& region) { return region.expired(); });
        std::shared_ptr<ScrollHoverRegion> target;
        for (const auto& weak : regions) {
            const auto region = weak.lock();
            if (!region || (!region->hovered && !region->rail_hovered) || !region->enabled ||
                !region->controller.IsConnected() ||
                region->controller.Metrics().maximum_offset <= kScrollHoverMinimumOverflow) {
                continue;
            }
            if (!target || region->depth > target->depth) target = region;
        }
        for (const auto& weak : regions)
            if (const auto region = weak.lock()) region->selected = region == target;
    }
    void Add(const std::shared_ptr<ScrollHoverRegion>& region) {
        if (std::none_of(regions.begin(), regions.end(), [&](const auto& weak) { return weak.lock() == region; }))
            regions.push_back(region);
    }
    void Remove(const std::shared_ptr<ScrollHoverRegion>& region) {
        region->hovered = false;
        region->rail_hovered = false;
        region->enabled = false;
        region->selected = false;
        std::erase_if(regions, [&](const auto& weak) { return weak.lock() == region; });
        Refresh();
    }
    void Hover(const std::shared_ptr<ScrollHoverRegion>& region, const huxerui::HoverEvent& event) {
        region->hovered = event.type != huxerui::HoverEventType::Leave;
        Refresh();
    }
};

struct ScrollHoverContext {
    std::shared_ptr<ScrollHoverRoute> route;
    std::size_t depth = 0;
    static ScrollHoverContext Default() { return {}; }
    bool operator==(const ScrollHoverContext&) const = default;
};

struct ScrollViewportHover {
    class Extension;
    std::shared_ptr<ScrollHoverRoute> route;
    std::shared_ptr<ScrollHoverRegion> region;
    huxerui::ScrollController controller;
    std::size_t depth = 0;
    bool enabled = true;
    bool operator==(const ScrollViewportHover&) const = default;
};

class ScrollViewportHover::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode& node, const ScrollViewportHover& value) : value_(value) { Update(node, value); }
    ~Extension() override { value_.route->Remove(value_.region); }
    void Update(huxerui::ViewNode& node, const ScrollViewportHover& value) {
        if (value_.route != value.route || value_.region != value.region) value_.route->Remove(value_.region);
        value_ = value;
        value_.region->controller = value.controller;
        value_.region->depth = value.depth;
        value_.region->enabled = value.enabled && node.IsEnabled();
        value_.route->Add(value_.region);
        value_.route->Refresh();
    }
    PaintInvalidation PrepareGeometry(huxerui::ViewNode& node, huxerui::TextMeasurer&) override {
        // Qualification can change without pointer movement: disabling a subtree
        // or content becoming short must hand the highlight to an eligible ancestor.
        const bool enabled = value_.enabled && node.IsEnabled();
        const bool overflowing = enabled && value_.region->controller.IsConnected() &&
            value_.region->controller.Metrics().maximum_offset > kScrollHoverMinimumOverflow;
        if (enabled != value_.region->enabled || overflowing != overflowing_) {
            value_.region->enabled = enabled;
            overflowing_ = overflowing;
            value_.route->Refresh();
        }
        return PaintInvalidation::None;
    }
private:
    ScrollViewportHover value_;
    bool overflowing_ = false;
};

} // namespace clashflux::ui
