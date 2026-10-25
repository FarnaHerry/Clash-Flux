#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <format>
#include <string>
#include <huxerui/huxerui.h>
#include "box_cat.h"
#include "home_control_motion.h"

namespace clashflux::ui {

// A presentation of the authoritative core (desktop) or VPN (mobile) snapshot.
// The owner retains this hook even while its page content is inactive.
struct HomeHeaderSnapshot {
    bool ready = false;
    bool active = false;
    bool running = false;
    bool busy = false;
    bool canStart = false;
    BoxCatPhase phase = BoxCatPhase::Sleeping;
    std::int64_t elapsedSeconds = 0;
    std::chrono::steady_clock::time_point runningSince{};
    bool operator==(const HomeHeaderSnapshot&) const = default;
};

struct HomeHeaderControl {
    huxerui::State<bool> shown;
    huxerui::State<bool> catReady;
    huxerui::State<HomeControlMotion> motion;
    huxerui::State<HomeHeaderSnapshot> latest;
    huxerui::SharedTransitionHandle transition;
};

inline std::string FormatHomeUptime(std::int64_t seconds) {
    seconds = std::max<std::int64_t>(0, seconds);
    return std::format("{:02}:{:02}:{:02}", seconds / 3600, (seconds / 60) % 60, seconds % 60);
}

HomeHeaderControl UseHomeHeaderControl(HomeHeaderSnapshot snapshot);
huxerui::View HomeHeaderTitle(HomeHeaderControl control, std::function<void()> toggle,
                             huxerui::StringVariant stopLabel);
huxerui::View HomeHeaderFloatingAction(huxerui::View page, HomeHeaderControl control,
    std::function<void()> toggle, huxerui::StringVariant actionLabel, float bottomInset);

} // namespace clashflux::ui
