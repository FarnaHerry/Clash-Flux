#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <huxerui/huxerui.h>

#if __has_include("wire_codec.h")
#include "wire_codec.h"
#else
#include "../wire_codec.h"
#endif

namespace clashflux::ui {

// Application-owned results for the Home LAN and public-IP cards.
//
// Values survive Home remounts and window recreation. Each card owns at most
// one detection in flight: a refresh requested while one runs is coalesced
// (LAN) or recorded as the latest wanted route (public IP) instead of starting
// a concurrent request. The displayed value is replaced only when a result
// arrives, so routine refreshes never fall back to a loading state.
// UI-thread only; fetch functions move their blocking work to task threads.
struct HomeNetworkModel : std::enable_shared_from_this<HomeNetworkModel> {
    using LanFetch = std::function<huxerui::Task<std::optional<std::string>>()>;
    using PublicFetch = std::function<huxerui::Task<wire::PublicIpLookup>()>;
    static constexpr std::array<int, 3> kPublicRetrySeconds{8, 15, 30};
    // A successful result for the same route is reused when Home remounts.
    static constexpr auto kPublicFreshness = std::chrono::minutes{5};

    huxerui::State<std::optional<std::string>> lanAddress{std::optional<std::string>{}};
    huxerui::State<bool> lanReady{false};
    huxerui::State<wire::PublicIpLookup> publicIp{wire::PublicIpLookup{}};
    // Ready means a success or failure has been published at least once.
    huxerui::State<bool> publicReady{false};
    huxerui::State<bool> publicFailed{false};

    void RefreshLan(huxerui::TaskScope tasks, LanFetch fetch) {
        if (lanRunning_) return;
        lanRunning_ = true;
        auto self = shared_from_this();
        tasks.Launch([self, fetch = std::move(fetch)]() -> huxerui::Task<void> {
            const LanRelease release{self};
            std::optional<std::string> address;
            bool succeeded = true;
            try { address = co_await fetch(); } catch (...) { succeeded = false; }
            // A failed read keeps the last address instead of blanking the card.
            if (!succeeded) co_return;
            self->lanAddress = std::move(address);
            self->lanReady = true;
        });
    }

    void RequestPublic(huxerui::TaskScope tasks, std::string routeKey, PublicFetch fetch) {
        if (publicRunning_ && routeKey == publicWantedKey_) return;
        if (!publicRunning_ && routeKey == publicShownKey_ && publicReady.Get() && !publicFailed.Get() &&
            std::chrono::steady_clock::now() - publicShownAt_ < kPublicFreshness) return;
        publicWantedKey_ = std::move(routeKey);
        publicWantedFetch_ = std::move(fetch);
        ++publicGeneration_;
        // The running loop observes the new generation and restarts with it.
        if (publicRunning_) return;
        publicRunning_ = true;
        auto self = shared_from_this();
        tasks.Launch([self]() -> huxerui::Task<void> {
            const PublicRelease release{self};
            for (;;) {
                const std::uint64_t generation = self->publicGeneration_;
                const std::string key = self->publicWantedKey_;
                const PublicFetch fetch = self->publicWantedFetch_;
                bool superseded = false;
                for (std::size_t attempt = 0;; ++attempt) {
                    wire::PublicIpLookup result;
                    try { result = co_await fetch(); } catch (...) { result = {}; }
                    if (generation != self->publicGeneration_) { superseded = true; break; }
                    if (!result.ip.empty()) {
                        self->publicIp = std::move(result);
                        self->publicFailed = false;
                        self->publicReady = true;
                        self->publicShownKey_ = key;
                        self->publicShownAt_ = std::chrono::steady_clock::now();
                        break;
                    }
                    // An address measured for another route must not stay on
                    // screen as if it were current; a same-route value may.
                    if (self->publicShownKey_ != key || !self->publicReady.Get()) {
                        self->publicIp = wire::PublicIpLookup{};
                        self->publicFailed = true;
                        self->publicReady = true;
                        self->publicShownKey_ = key;
                    }
                    if (attempt >= kPublicRetrySeconds.size()) break;
                    // Step in 250ms Delay slices so a newer route preempts promptly
                    // under both real wall-clock and virtual test clocks.
                    const int steps = kPublicRetrySeconds[attempt] * 4;
                    for (int step = 0; step < steps && generation == self->publicGeneration_; ++step) {
                        co_await huxerui::Delay(std::chrono::milliseconds{250});
                    }
                    if (generation != self->publicGeneration_) { superseded = true; break; }
                }
                if (!superseded) co_return;
            }
        });
    }

private:
    struct LanRelease {
        std::shared_ptr<HomeNetworkModel> model;
        ~LanRelease() { model->lanRunning_ = false; }
    };
    struct PublicRelease {
        std::shared_ptr<HomeNetworkModel> model;
        ~PublicRelease() { model->publicRunning_ = false; }
    };

    bool lanRunning_ = false;
    bool publicRunning_ = false;
    std::uint64_t publicGeneration_ = 0;
    std::string publicWantedKey_;
    PublicFetch publicWantedFetch_;
    std::string publicShownKey_;
    std::chrono::steady_clock::time_point publicShownAt_{};
};

} // namespace clashflux::ui
