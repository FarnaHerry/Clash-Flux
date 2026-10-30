#pragma once

#include <huxerui/huxerui.h>

namespace clashflux::ui {
inline auto RefreshIconRotation(const huxerui::ThemeSpec& theme, bool spinning) {
    return spinning && !theme.motion.reduced_motion
        ? huxerui::AnimateTo(360.0F, huxerui::TweenSpec{0.9, huxerui::Easing::Linear},
            huxerui::AnimationPlayback{.iterations = std::nullopt})
        : huxerui::AnimateTo(0.0F, huxerui::SnapSpec{});
}
} // namespace clashflux::ui
