#pragma once

#include <huxerui/huxerui.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clashflux::ui {
struct ProbeState {
    int delay = 0;
    bool timeout = false;
    bool testing = false;
    bool operator==(const ProbeState&) const = default;
};
struct GroupTestProgress {
    int generation = 0;
    bool busy = false;
    bool operator==(const GroupTestProgress&) const = default;
};
struct ProxyProbe {
    huxerui::State<ProbeState> value{ProbeState{}};
    huxerui::State<int> generation{0};
    huxerui::State<std::int64_t> baselineTime{0};
};

// UI-thread state owned by ProxiesModel, independent of pages and virtual rows.
struct ProxyTestModel {
    huxerui::State<int> generation{0};
    huxerui::State<std::string> group{std::string{}};
    huxerui::State<GroupTestProgress> progress{GroupTestProgress{}};
    huxerui::State<int> pending{0};
    huxerui::State<int> singlePending{0};
    std::map<std::string, std::shared_ptr<ProxyProbe>> probes;
    std::optional<std::pair<bool, std::uint64_t>> context;

    bool Current(int ticket) const {
        return progress.Get().busy && progress.Get().generation == ticket;
    }
    int Begin() {
        if (progress.Get().busy || singlePending.Get() != 0) return 0;
        const int ticket = generation.Get() + 1;
        pending = 0;
        progress = GroupTestProgress{ticket, true};
        return ticket;
    }
    void Broadcast(int ticket, const std::string& name) {
        if (!Current(ticket)) return;
        group = name;
        generation = ticket;
    }
    void End(int ticket) {
        if (!Current(ticket)) return;
        for (auto& [tag, probe] : probes)
            if (probe->generation.Get() == ticket && probe->value.Get().testing)
                probe->value = ProbeState{0, true, false};
        if (generation.Get() < ticket) generation = ticket;
        pending = 0;
        progress = GroupTestProgress{};
    }
    void ObserveContext(bool running, std::uint64_t revision) {
        const auto next = std::pair{running, revision};
        if (context && *context != next) {
            const int ticket = std::max(generation.Get(), progress.Get().generation) + 1;
            progress = GroupTestProgress{};
            pending = 0;
            singlePending = 0;
            probes.clear();
            generation = ticket;
            group = std::string{};
        }
        context = next;
    }
    std::shared_ptr<ProxyProbe> Get(const std::string& tag, int delay, bool timeout, std::int64_t time) {
        auto& probe = probes[tag];
        if (!probe) {
            probe = std::make_shared<ProxyProbe>();
            probe->value = ProbeState{delay, timeout, false};
            probe->baselineTime = time;
        }
        return probe;
    }
    bool Start(const std::shared_ptr<ProxyProbe>& probe, int ticket, std::int64_t time) {
        if (!Current(ticket) || probe->generation.Get() == ticket || probe->value.Get().testing) return false;
        probe->generation = ticket;
        probe->baselineTime = time;
        probe->value = ProbeState{0, false, true};
        pending += 1;
        return true;
    }
    void Complete(const std::shared_ptr<ProxyProbe>& probe, int ticket, int delay) {
        if (!Current(ticket) || probe->generation.Get() != ticket || !probe->value.Get().testing) return;
        probe->value = ProbeState{delay > 0 ? delay : 0, delay <= 0, false};
        if (pending.Get() > 0) pending -= 1;
    }
    int StartSingle(const std::string& tag, const std::shared_ptr<ProxyProbe>& probe) {
        const auto found = probes.find(tag);
        if (found == probes.end() || found->second != probe ||
            progress.Get().busy || probe->value.Get().testing) return 0;
        const int ticket = generation.Get() + 1;
        generation = ticket;
        probe->generation = ticket;
        probe->value = ProbeState{0, false, true};
        singlePending += 1;
        return ticket;
    }
    void CompleteSingle(const std::string& tag, const std::shared_ptr<ProxyProbe>& probe,
                        int ticket, std::optional<int> delay) {
        const auto found = probes.find(tag);
        if (found == probes.end() || found->second != probe ||
            probe->generation.Get() != ticket || !probe->value.Get().testing) return;
        probe->value = ProbeState{delay && *delay > 0 ? *delay : 0,
                                 delay && *delay <= 0, false};
        if (singlePending.Get() > 0) singlePending -= 1;
    }
};

template<class Measure, class Valid>
bool LaunchSingleProxyProbe(huxerui::TaskScope tasks, std::shared_ptr<ProxyTestModel> model,
    std::string tag, std::shared_ptr<ProxyProbe> probe, Measure measure, Valid valid) {
    const int ticket = model->StartSingle(tag, probe);
    if (!ticket) return false;
    tasks.Launch([model, tag = std::move(tag), probe, ticket, measure, valid]() -> huxerui::Task<void> {
        int delay = 0;
        if (valid()) {
            try { delay = co_await measure(tag); } catch (...) {}
        }
        model->CompleteSingle(tag, probe, ticket, valid() ? std::optional<int>{delay} : std::nullopt);
    });
    return true;
}

struct ProxyProbeTarget {
    std::string tag;
    std::shared_ptr<ProxyProbe> probe;
};

// One kernel-side group test supplies delays for every target. This replaces
// UI-side per-node fan-out so the kernel owns the group URL, concurrency and
// history write order (sing-box /group/{name}/delay semantics).
template<class Measure, class Valid, class Poll>
void LaunchProxyGroupProbeBatch(huxerui::TaskScope tasks, std::shared_ptr<ProxyTestModel> model,
    std::string group, std::vector<ProxyProbeTarget> targets, int ticket,
    Measure measure, Valid valid, Poll poll) {
    struct BatchResult {
        bool done = false;
        std::map<std::string, int> delays;
    };
    auto result = std::make_shared<BatchResult>();
    // The kernel batch response only arrives after every member finishes. Run
    // it beside a snapshot poll so each node can publish its own delay as soon
    // as the kernel history/proxies snapshot reports it.
    tasks.Launch([result, group = std::move(group), measure, valid]() -> huxerui::Task<void> {
        std::map<std::string, int> delays;
        if (valid()) {
            try { delays = co_await measure(group); } catch (...) {}
        }
        result->delays = std::move(delays);
        result->done = true;
    });
    tasks.Launch([model, targets = std::move(targets), ticket, valid, poll, result]() -> huxerui::Task<void> {
        for (int attempt = 0; attempt < 124; ++attempt) {
            if (!valid() || !model->Current(ticket)) co_return;
            if (result->done) break;
            for (const auto& target : targets) {
                const auto& probe = target.probe;
                if (!probe->value.Get().testing) continue;
                const auto measured = poll(target.tag);
                if (measured && measured->first > 0 &&
                    measured->second > probe->baselineTime.Get()) {
                    model->Complete(probe, ticket, measured->first);
                    probe->baselineTime = measured->second;
                }
            }
            co_await huxerui::Delay(std::chrono::duration<double>{0.25});
        }
        if (!valid() || !model->Current(ticket)) co_return;
        for (const auto& target : targets) {
            const auto found = result->delays.find(target.tag);
            model->Complete(target.probe, ticket, found == result->delays.end() ? 0 : found->second);
        }
    });
}

template<class Valid, class Poll>
void LaunchNativeProxyProbeBatch(huxerui::TaskScope tasks, std::shared_ptr<ProxyTestModel> model,
    std::vector<ProxyProbeTarget> targets, int ticket, Valid valid, Poll poll) {
    tasks.Launch([model, targets = std::move(targets), ticket, valid, poll]() -> huxerui::Task<void> {
        for (int attempt = 0; attempt < 120; ++attempt) {
            co_await huxerui::Delay(std::chrono::duration<double>{0.25});
            if (!valid()) { model->End(ticket); co_return; }
            if (!model->Current(ticket) || model->pending.Get() == 0) co_return;
            for (const auto& target : targets) {
                const auto& probe = target.probe;
                if (!probe->value.Get().testing) continue;
                const auto result = poll(target.tag);
                if (result && result->second > 0 && result->second != probe->baselineTime.Get()) {
                    model->Complete(probe, ticket, result->first);
                    probe->baselineTime = result->second;
                }
            }
        }
        model->End(ticket);
    });
}

inline huxerui::Task<void> WaitForProxyTestBatch(std::shared_ptr<ProxyTestModel> model, int ticket,
    bool bounded = true) {
    for (int attempt = 0; !bounded || attempt < 124; ++attempt) {
        co_await huxerui::Delay(std::chrono::duration<double>{0.25});
        if (!model->Current(ticket)) co_return;
        if (model->pending.Get() == 0) break;
    }
    model->End(ticket);
}
} // namespace clashflux::ui
