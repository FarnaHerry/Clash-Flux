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
template<class Measure, class Valid>
void LaunchProxyProbeBatch(huxerui::TaskScope tasks, std::shared_ptr<ProxyTestModel> model,
    std::vector<ProxyProbeTarget> targets, int ticket, Measure measure, Valid valid) {
    constexpr std::size_t concurrency = 8;
    const auto owned = std::make_shared<const std::vector<ProxyProbeTarget>>(std::move(targets));
    for (std::size_t worker = 0; worker < std::min(concurrency, owned->size()); ++worker)
        tasks.Launch([model, owned, ticket, worker, measure, valid]() -> huxerui::Task<void> {
            for (std::size_t index = worker; index < owned->size(); index += concurrency) {
                if (!model->Current(ticket)) co_return;
                if (!valid()) { model->End(ticket); co_return; }
                int delay = 0;
                try { delay = co_await measure((*owned)[index].tag); } catch (...) {}
                if (!valid()) { model->End(ticket); co_return; }
                model->Complete((*owned)[index].probe, ticket, delay);
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
