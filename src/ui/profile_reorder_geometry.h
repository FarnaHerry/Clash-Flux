#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>

#include <huxerui/huxerui.h>

namespace clashflux::ui {

struct ProfileGridGeometry {
    std::unordered_map<std::int64_t, huxerui::Rect> slots;

    // 命中固定槽位，包含卡片间隙；位移动画不会改变这个结果。
    std::int64_t Closest(const std::vector<std::int64_t>& ids,
                         huxerui::Point position) const {
        std::int64_t closestId = 0;
        float closestDistance = std::numeric_limits<float>::infinity();
        for (const std::int64_t candidate : ids) {
            const auto slot = slots.find(candidate);
            if (slot == slots.end()) continue;
            const auto& bounds = slot->second;
            const float dx = position.x - std::clamp(position.x, bounds.x, bounds.x + bounds.width);
            const float dy = position.y - std::clamp(position.y, bounds.y, bounds.y + bounds.height);
            const float distance = dx * dx + dy * dy;
            if (distance < closestDistance) {
                closestDistance = distance;
                closestId = candidate;
            }
        }
        return closestId;
    }
};

// 记录未位移的网格槽位，而不是卡片动画后的呈现位置。
struct ProfileSlotBounds {
    std::shared_ptr<ProfileGridGeometry> geometry;
    std::int64_t id = 0;

    class Extension final : public huxerui::NodeExtension {
    public:
        Extension(huxerui::ViewNode&, const ProfileSlotBounds& spec) { Update(spec); }
        void Update(huxerui::ViewNode&, const ProfileSlotBounds& spec) { Update(spec); }
        [[nodiscard]] PaintInvalidation PrepareGeometry(
            huxerui::ViewNode& node, huxerui::TextMeasurer&) override {
            geometry_->slots[id_] = node.LocalToWindowBounds(node.Bounds());
            return PaintInvalidation::None;
        }

    private:
        void Update(const ProfileSlotBounds& spec) {
            geometry_ = spec.geometry;
            id_ = spec.id;
        }
        std::shared_ptr<ProfileGridGeometry> geometry_;
        std::int64_t id_ = 0;
    };

    bool operator==(const ProfileSlotBounds&) const = default;
};

// 网格节点保持原位，只有呈现位置移动：拖动卡片占目标槽，其间的卡片依次让位。
inline std::size_t ProfileReorderDestination(std::size_t index,
                                             std::size_t from,
                                             std::size_t to) {
    if (index == from) return to;
    if (from < to && index > from && index <= to) return index - 1;
    if (from > to && index >= to && index < from) return index + 1;
    return index;
}

} // namespace clashflux::ui
