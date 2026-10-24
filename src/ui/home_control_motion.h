#pragma once

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <huxerui/huxerui.h>
#include "core_operation.h"

namespace clashflux::ui {

inline constexpr huxerui::TweenSpec kHomeControlMotion{
    .duration = 0.64, .easing = huxerui::Easing::EaseInOut};
inline constexpr huxerui::TweenSpec kHomeControlCollapseMotion{
    .duration = 0.28, .easing = huxerui::Easing::EaseInOut};
inline constexpr huxerui::TweenSpec kHomeControlReturnMotion{
    .duration = 0.36, .easing = huxerui::Easing::EaseInOut};
inline constexpr float kHomeControlCollapsedScale = 0.20F;

struct HomeControlMotion {
    CoreAction action = CoreAction::Idle;
    std::uint64_t serial = 0;
    bool collapsing = false;
    bool collapsed = false;
    bool operator==(const HomeControlMotion&) const = default;
};

inline void AnimateHomeControlCard(bool showCard, huxerui::State<bool> cardShown,
                                  huxerui::State<HomeControlMotion> motion,
                                  huxerui::SharedTransitionHandle transition,
                                  huxerui::TaskScope tasks, bool reducedMotion = false) {
    if (cardShown.Get() == showCard ||
        (!showCard && motion.Get().action == CoreAction::Stop)) return;
    const auto serial = motion.Get().serial + 1;
    const auto action = showCard ? CoreAction::Start : CoreAction::Stop;
    const auto present = [cardShown, motion, showCard, action, serial] {
        cardShown = showCard;
        motion = HomeControlMotion{action, serial, false};
    };
    if (reducedMotion) {
        present();
        motion = HomeControlMotion{CoreAction::Idle, serial};
        return;
    }
    if (!showCard) {
        // Keep the real card mounted for a complete, opaque scale animation.
        // Only capture its small final pose once that first stage has painted.
        motion = HomeControlMotion{CoreAction::Stop, serial, true};
        tasks.Launch([motion, transition, present, serial]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{
                kHomeControlCollapseMotion.duration + 0.04});
            if (motion.Get().serial != serial) co_return;
            // Commit a genuinely 56pt, unscaled view before taking the next
            // snapshot. Shared capture normalizes a marker's root transform,
            // so capturing the scaled card itself would restore its full size.
            motion = HomeControlMotion{CoreAction::Stop, serial, false, true};
            co_await huxerui::Delay(std::chrono::duration<double>{0.04});
            if (motion.Get().serial != serial) co_return;
            try { transition.Run(kHomeControlReturnMotion, present); }
            catch (const std::logic_error&) { present(); }
            co_await huxerui::Delay(std::chrono::duration<double>{
                kHomeControlReturnMotion.duration + 0.08});
            if (motion.Get().serial == serial)
                motion = HomeControlMotion{CoreAction::Idle, serial};
        });
        return;
    }
    try { transition.Run(kHomeControlMotion, present); }
    catch (const std::logic_error&) { present(); }
    tasks.Launch([motion, serial]() -> huxerui::Task<void> {
        co_await huxerui::Delay(std::chrono::duration<double>{kHomeControlMotion.duration + 0.08});
        if (motion.Get().serial == serial)
            motion = HomeControlMotion{CoreAction::Idle, serial};
    });
}

// The first stage has already shrunk the actual card. This second stage only
// carries that small captured visual back to the dock, with no early retreat.
struct HomeControlReturnToDock {
    huxerui::Rect Evaluate(huxerui::Rect from, huxerui::Rect to, float progress) const {
        const auto t = std::clamp(progress, 0.0F, 1.0F);
        return {from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t,
                from.width + (to.width - from.width) * t,
                from.height + (to.height - from.height) * t};
    }
    bool operator==(const HomeControlReturnToDock&) const = default;
};

// Expand: move the button to the card's center, then grow. Collapse follows
// exactly the same track backwards: shrink in place, then return to the dock.
struct HomeControlMoveExpand {
    static constexpr float kMoveFraction = 0.48F;

    static huxerui::Rect Expand(huxerui::Rect button, huxerui::Rect card, float progress) {
        const float move = std::clamp(progress / kMoveFraction, 0.0F, 1.0F);
        const float grow = std::clamp((progress - kMoveFraction) /
                                          (1.0F - kMoveFraction), 0.0F, 1.0F);
        const float buttonCenterX = button.x + button.width / 2.0F;
        const float buttonCenterY = button.y + button.height / 2.0F;
        const float cardCenterX = card.x + card.width / 2.0F;
        const float cardCenterY = card.y + card.height / 2.0F;
        const float centerX = buttonCenterX + (cardCenterX - buttonCenterX) * move;
        const float centerY = buttonCenterY + (cardCenterY - buttonCenterY) * move;
        const float width = button.width + (card.width - button.width) * grow;
        const float height = button.height + (card.height - button.height) * grow;
        return {centerX - width / 2.0F, centerY - height / 2.0F, width, height};
    }

    huxerui::Rect Evaluate(huxerui::Rect from, huxerui::Rect to, float progress) const {
        if (progress <= 0.0F) return from;
        if (progress >= 1.0F) return to;
        if (from.width * from.height > to.width * to.height)
            return Expand(to, from, 1.0F - progress);
        return Expand(from, to, progress);
    }

    bool operator==(const HomeControlMoveExpand&) const = default;
};

} // namespace clashflux::ui
