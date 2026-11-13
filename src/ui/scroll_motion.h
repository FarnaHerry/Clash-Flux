#pragma once

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>
#include <huxerui/huxerui.h>
#include "scroll_hover.h"

namespace clashflux::ui {

struct PageScrollOffset { float value = 0.0F; };

// One motion owner per real viewport, including viewports without a visible rail.
// Only UI-thread input and mounted frame callbacks access this object.
class ScrollMotion {
public:
    enum class Input { Arrow, Wheel };
    ScrollMotion(huxerui::ScrollController controller, std::shared_ptr<PageScrollOffset> remembered,
        std::shared_ptr<ScrollHoverRegion> region, huxerui::TaskScope tasks, huxerui::WindowHandle window)
        : controller(controller), remembered(remembered), region(region), tasks(tasks), window(window) {}
    huxerui::ScrollController controller;
    std::shared_ptr<PageScrollOffset> remembered;
    std::shared_ptr<ScrollHoverRegion> region;
    huxerui::TaskScope tasks;
    huxerui::WindowHandle window;
    bool animate = true, foreground = true, enabled = true;
    double wheel_duration = 0.12;

    void Cancel() {
        if (active_ && remembered) remembered->value = controller.Offset();
        ++commit_->generation; commit_->pending = false; commit_->cancelled = false;
        commit_->applied_offset = controller.Offset();
        active_ = false; motion_.Set(controller.Offset());
    }
    void CancelArrows() { if (input_ == Input::Arrow) Cancel(); }
    bool IsArrow() const { return input_ == Input::Arrow; }
    bool IsActive() const { return active_; }
    bool Reserves(float delta) const {
        return active_ && Current() && (motion_.Target() - controller.Offset()) * delta > 0.0F;
    }
    void PrepareWheel(float delta) {
        if (active_ && (!Current() || input_ != Input::Wheel || (motion_.Target() - controller.Offset()) * delta < 0.0F))
            Cancel();
    }
    float Request(float delta, double duration, Input input, bool from_frame = false) {
        if (!enabled || !foreground || !controller.IsConnected() || !std::isfinite(delta) || delta == 0.0F)
            return 0.0F;
        const float before = controller.Offset();
        if (active_ && (!Current() || input_ != input)) Cancel();
        const float maximum = controller.MaxOffset();
        const float pending = std::clamp(motion_.Target(), 0.0F, maximum);
        const float base = active_ && (pending - before) * delta >= 0.0F ? pending : before;
        const float target = std::clamp(base + delta, 0.0F, maximum);
        const float consumed = target - base;
        // A full pending target owns no more distance. Leave the rest to an ancestor.
        if (consumed == 0.0F) return 0.0F;
        if (commit_->pending) {
            ++commit_->generation; commit_->pending = false; motion_.Seek(before);
        }
        input_ = input; revision_ = region->input_revision; duration_ = duration;
        if (!animate || !std::isfinite(duration) || duration <= 0.0) {
            Cancel();
            if (!from_frame) {
                if (!controller.ScrollTo(target)) return 0.0F;
                commit_->applied_offset = controller.Offset(); commit_->geometry_sync = true;
                if (remembered) remembered->value = commit_->applied_offset;
            } else {
                motion_.Set(target); QueueOffset(target); active_ = commit_->pending;
            }
            return consumed;
        }
        if (!active_) {
            if (!controller.ScrollTo(before)) return 0.0F; // Stop native momentum without displacement.
            motion_.Set(before); commit_->applied_offset = before;
        }
        motion_.AnimateTo(target, huxerui::TweenSpec{duration, huxerui::Easing::EaseOut});
        active_ = true;
        return consumed;
    }
    huxerui::NodeExtension::FrameResult Advance(const huxerui::FrameInfo& frame) {
        if (!active_) return {};
        if (!enabled || !foreground || !controller.IsConnected() || !Current() || controller.MaxOffset() <= 0.0F) {
            Cancel(); return {};
        }
        const float target = std::clamp(motion_.Target(), 0.0F, controller.MaxOffset());
        if (!animate || frame.reduced_motion || !std::isfinite(duration_) || duration_ <= 0.0) {
            motion_.Set(target); QueueOffset(target); active_ = commit_->pending;
        } else {
            if (target != motion_.Target()) motion_.AnimateTo(target, huxerui::TweenSpec{duration_, huxerui::Easing::EaseOut});
            const auto progress = motion_.Advance(frame);
            QueueOffset(std::clamp(motion_.Value(), 0.0F, controller.MaxOffset()));
            active_ = progress.needs_frame || commit_->pending;
        }
        return {active_, {}};
    }
    void SyncGeometry() {
        if (!commit_->geometry_sync) return;
        commit_->applied_offset = controller.Offset(); commit_->geometry_sync = false;
        if (remembered) remembered->value = commit_->applied_offset;
    }
private:
    bool Current() const {
        return !commit_->cancelled && region->input_revision == revision_ &&
            std::abs(controller.Offset() - commit_->applied_offset) <= 0.01F;
    }
    void QueueOffset(float offset) {
        if (commit_->pending || controller.Offset() == offset) return;
        commit_->pending = true;
        // OnFrame follows virtual realization. Commit before the next layout so
        // virtual content and the thumb present the same actual offset.
        tasks.Post([weak = std::weak_ptr{commit_}, generation = commit_->generation,
            expected = commit_->applied_offset, offset, controller = controller, remembered = remembered,
            region = region, revision = revision_, window = window] {
            const auto commit = weak.lock();
            if (!commit || generation != commit->generation) return;
            commit->pending = false;
            if (!window.IsOpen() || window.LifecycleState() == huxerui::WindowLifecycleState::Background ||
                !controller.IsConnected() || revision != region->input_revision ||
                std::abs(controller.Offset() - expected) > 0.01F) {
                commit->cancelled = true; return;
            }
            if (!controller.ScrollTo(std::clamp(offset, 0.0F, controller.MaxOffset()))) {
                commit->cancelled = true; return;
            }
            commit->applied_offset = controller.Offset(); commit->geometry_sync = true;
            if (remembered) remembered->value = commit->applied_offset;
        });
    }
    huxerui::MotionController motion_{0.0F};
    bool active_ = false;
    Input input_ = Input::Arrow;
    double duration_ = 0.12;
    std::uint64_t revision_ = 0;
    struct Commit {
        std::uint64_t generation = 0;
        float applied_offset = 0.0F;
        bool pending = false, cancelled = false, geometry_sync = false;
    };
    std::shared_ptr<Commit> commit_ = std::make_shared<Commit>();
};

// ScrollInput identifies the innermost mounted app viewport using the actual
// event route, independently of hover. Pre-scroll consumes only vertical travel;
// native horizontal scrolling and unconsumed boundary travel remain available.
struct ScrollWheelRoute {
    std::weak_ptr<ScrollMotion> target;
    std::vector<std::weak_ptr<ScrollMotion>> candidates;
    bool pending = false;
    float Consume(const std::shared_ptr<ScrollMotion>& candidate, float available) {
        if (!pending) return 0.0F;
        // Pre-scroll visits outer to inner. Wait for the input's actual target,
        // then allocate in reverse order. Captured popup environments must not
        // make an unrelated background viewport an ancestor of the popup.
        candidates.push_back(candidate);
        if (candidate != target.lock()) return 0.0F;
        pending = false;
        // Match native input takeover for the complete nested route, including
        // ancestors whose pending motion would otherwise outlive a reversal.
        for (const auto& weak : candidates)
            if (const auto motion = weak.lock()) motion->PrepareWheel(available);
        float remaining = available;
        bool reserved = false;
        for (auto it = candidates.rbegin(); it != candidates.rend() && remaining != 0.0F; ++it) {
            if (const auto motion = it->lock()) {
                remaining -= motion->Request(remaining, motion->wheel_duration, ScrollMotion::Input::Wheel);
                reserved = reserved || motion->Reserves(available);
            }
        }
        candidates.clear();
        // Pending travel has already reserved the remaining physical range.
        // Discard terminal overshoot rather than letting the native fallback
        // jump into that range and cancel the animation.
        if (remaining != 0.0F && reserved) return available;
        return available - remaining;
    }
};
struct ScrollMotionContext {
    std::shared_ptr<ScrollWheelRoute> wheel;
    static ScrollMotionContext Default() { return {}; }
    bool operator==(const ScrollMotionContext&) const = default;
};
struct ScrollViewportMotion {
    class Extension;
    std::shared_ptr<ScrollMotion> motion;
    std::shared_ptr<ScrollWheelRoute> wheel;
    bool animate = true, foreground = true;
    double wheel_duration = 0.12;
    bool operator==(const ScrollViewportMotion&) const = default;
};
class ScrollViewportMotion::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode& node, const ScrollViewportMotion& value) : value_(value) { Update(node, value); }
    ~Extension() override { value_.motion->Cancel(); }
    void Update(huxerui::ViewNode& node, const ScrollViewportMotion& value) {
        if (value_.motion != value.motion) value_.motion->Cancel();
        value_ = value;
        auto& motion = *value_.motion;
        motion.animate = value.animate; motion.foreground = value.foreground;
        motion.enabled = node.IsEnabled(); motion.wheel_duration = value.wheel_duration;
        if (!motion.foreground || !motion.enabled) motion.Cancel();
    }
    FrameResult OnFrame(huxerui::ViewNode& node, const huxerui::FrameInfo& frame) override {
        value_.motion->enabled = node.IsEnabled();
        return value_.motion->Advance(frame);
    }
    PaintInvalidation PrepareGeometry(huxerui::ViewNode& node, huxerui::TextMeasurer&) override {
        value_.motion->enabled = node.IsEnabled();
        value_.motion->SyncGeometry();
        return PaintInvalidation::None;
    }
    float OnPreScroll(huxerui::ViewNode&, huxerui::Axis axis, float available, huxerui::ScrollSource source) override {
        // Input takeover belongs to the viewport even when no rail/hover
        // extension is mounted in Compact.
        if (source != huxerui::ScrollSource::Wheel) ++value_.motion->region->input_revision;
        if (source == huxerui::ScrollSource::Wheel && axis == huxerui::Axis::Vertical) {
            const float consumed = value_.wheel->Consume(value_.motion, available);
            if (consumed != 0.0F) InvalidatePaint(); // Start frames even when no offset has changed yet.
            return consumed;
        }
        return 0.0F;
    }
private:
    ScrollViewportMotion value_;
};

} // namespace clashflux::ui
