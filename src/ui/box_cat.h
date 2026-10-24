#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <huxerui/huxerui.h>

namespace clashflux::ui {

enum class BoxCatPhase { Sleeping, Starting, Running, Stopping, Failed };

// UI-thread-only instrumentation; these fields are not observable composition State.
struct BoxCatProbe {
    std::uint64_t ticks = 0;
    bool looping = false;
    bool sawEars = false;
    bool sawTail = false;
    bool sawSleep = false;
    bool sawPetting = false;
    bool sawContent = false;
    bool sawWink = false;
    std::uint64_t sleepTicks = 0;
    bool sleepLooping = false;
    bool handVisible = false;
    std::uint64_t loadingTicks = 0;
    bool loadingLooping = false;
    bool sawHandRetiredAfterRise = false;
    float progress = 0.0F;
    bool transitioning = false;
    bool failurePlaying = false;
    std::uint64_t failurePlays = 0;
    std::uint64_t frames = 0;
    bool visible = true;
    bool reducedFrame = false;
};

// Completion is queued once per settled Running pose, after the paint frame;
// cancellation or retiring the animation discards an undelivered completion.
huxerui::View BoxCatAnimation(BoxCatPhase phase, bool motionEnabled = true,
    std::shared_ptr<BoxCatProbe> probe = {},
    std::optional<huxerui::Color> symbolColor = {},
    std::function<void()> onRunningReady = {},
    std::optional<BoxCatPhase> initialPose = {});

// Shared by the desktop core and mobile VPN buttons; the 48pt artwork is
// centered inside the entire 56pt hit target, including its sleeping symbols.
huxerui::View BoxCatButtonContent(BoxCatPhase phase, huxerui::Color symbolColor,
    bool motionEnabled = true, std::shared_ptr<BoxCatProbe> probe = {},
    std::function<void()> onRunningReady = {},
    std::optional<BoxCatPhase> initialPose = {});

// The running card's badge owns both the container and artwork dimensions.
huxerui::View BoxCatStatusIcon(BoxCatPhase phase, bool motionEnabled = true,
    std::shared_ptr<BoxCatProbe> probe = {});

} // namespace clashflux::ui
