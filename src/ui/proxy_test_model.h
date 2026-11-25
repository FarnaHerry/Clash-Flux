#pragma once

#include "../wire_codec.h"

#include <huxerui/huxerui.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

namespace clashflux::ui {
struct ProbeState {
    int delay = 0;
    bool timeout = false;
    bool testing = false;
    bool operator==(const ProbeState&) const = default;
};
struct ProxyProbe {
    huxerui::State<ProbeState> value{ProbeState{}};
    huxerui::State<std::int64_t> baselineTime{0};
    ProbeState beforeTest;
    int owner = 0;
};

struct ProxyGroupTest {
    huxerui::State<int> ticket{0};
    bool bound = false;
    int pending = 0;
    std::vector<std::shared_ptr<ProxyProbe>> claimed;

    bool Current(int id) const { return id != 0 && ticket.Get() == id; }

    bool Start(const std::shared_ptr<ProxyProbe>& probe, int id, std::int64_t time) {
        if (!Current(id) || probe->owner != 0) return false;
        claimed.push_back(probe);
        probe->owner = id;
        probe->beforeTest = probe->value.Get();
        probe->baselineTime = std::max(probe->baselineTime.Get(), time);
        probe->value = ProbeState{0, false, true};
        ++pending;
        return true;
    }

    void Complete(const std::shared_ptr<ProxyProbe>& probe, int id, int delay) {
        if (!Current(id) || probe->owner != id || !probe->value.Get().testing) return;
        probe->value = ProbeState{delay > 0 ? delay : 0, delay <= 0, false};
        --pending;
    }

    void End(int id, bool timedOut = true) {
        if (!Current(id)) return;
        for (const auto& probe : claimed) {
            if (probe->value.Get().testing) {
                probe->value = timedOut ? ProbeState{0, true, false} : probe->beforeTest;
            }
            probe->owner = 0;
        }
        claimed.clear();
        pending = 0;
        bound = false;
        ticket = 0;
    }
};

// UI-thread state owned by ProxiesModel, independent of pages and virtual rows.
struct ProxyTestModel {
    using GroupRequest = std::pair<std::shared_ptr<ProxyGroupTest>, int>;

    huxerui::State<int> generation{0};
    std::map<std::string, std::shared_ptr<ProxyProbe>> probes;
    std::map<std::string, std::shared_ptr<ProxyGroupTest>, std::less<>> groups;
    huxerui::State<std::vector<GroupRequest>> allGroupRequests{std::vector<GroupRequest>{}};
    std::optional<std::tuple<bool, std::uint64_t, std::uint64_t>> context;
    std::shared_ptr<std::optional<bool>> readiness;
    std::set<std::string> singlePreparing;
    int nextTicket = 0;

    std::shared_ptr<ProxyGroupTest> GetGroup(std::string_view tag) {
        auto [entry, inserted] = groups.try_emplace(std::string(tag));
        if (inserted) entry->second = std::make_shared<ProxyGroupTest>();
        return entry->second;
    }

    int Begin(ProxyGroupTest& group) {
        if (group.ticket.Get() != 0) return 0;
        group.ticket = ++nextTicket;
        return nextTicket;
    }

    bool AllGroupsTesting() const {
        return std::ranges::any_of(allGroupRequests.Get(), [](const GroupRequest& request) {
            return request.first->Current(request.second);
        });
    }

    void StartAllGroups(const std::vector<std::string>& tags,
        const std::function<void(const std::string&)>& start) {
        if (AllGroupsTesting()) return;
        std::vector<GroupRequest> requests;
        requests.reserve(tags.size());
        for (const auto& tag : tags) {
            auto group = GetGroup(tag);
            if (group->ticket.Get() == 0) start(tag);
            const int ticket = group->ticket.Get();
            if (ticket != 0) requests.emplace_back(std::move(group), ticket);
        }
        allGroupRequests = std::move(requests);
    }

    void ObserveContext(bool running, std::uint64_t revision, std::uint64_t epoch = 0) {
        const auto next = std::tuple{running, revision, epoch};
        if (context && *context != next) {
            // Requests waiting for standby bind to the runtime after it starts.
            for (const auto& [tag, group] : groups) {
                if (group->bound) group->End(group->ticket.Get(), false);
            }
            probes.clear();
            generation += 1;
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
    int StartSingle(const std::string& tag, const std::shared_ptr<ProxyProbe>& probe) {
        const auto found = probes.find(tag);
        if (found == probes.end() || found->second != probe || probe->owner != 0) return 0;
        const int ticket = ++nextTicket;
        probe->owner = ticket;
        probe->beforeTest = probe->value.Get();
        probe->value = ProbeState{0, false, true};
        return ticket;
    }
    void CompleteSingle(const std::string& tag, const std::shared_ptr<ProxyProbe>& probe,
                        int ticket, std::optional<int> delay) {
        const auto found = probes.find(tag);
        if (found == probes.end() || found->second != probe || probe->owner != ticket) return;
        probe->value = delay ? ProbeState{std::max(*delay, 0), *delay <= 0, false}
                             : probe->beforeTest;
        probe->owner = 0;
    }
};

template<class Measure, class Valid>
bool LaunchSingleProxyProbe(huxerui::TaskScope tasks, std::shared_ptr<ProxyTestModel> model,
    std::string tag, std::shared_ptr<ProxyProbe> probe, Measure measure, Valid valid) {
    const int ticket = model->StartSingle(tag, probe);
    if (!ticket) return false;
    tasks.Launch([model, tag = std::move(tag), probe, ticket, measure, valid]() -> huxerui::Task<void> {
        std::optional<int> delay{0};
        if (valid()) {
            try { delay = co_await measure(tag); } catch (...) {}
        }
        model->CompleteSingle(tag, probe, ticket, valid() ? delay : std::nullopt);
    });
    return true;
}

struct NativeProxyHistory {
    std::int64_t baselineTime = 0;
    bool hadHistory = false;

    static NativeProxyHistory Capture(const ProxyProbe& probe,
        const std::optional<std::pair<int, std::int64_t>>& previous) {
        const auto time = previous ? previous->second : 0;
        const auto baseline = std::max(probe.baselineTime.Get(), time);
        return {baseline, time > 0 && time == baseline};
    }

    std::optional<int> Resolve(ProxyProbe& probe,
        const std::optional<std::pair<int, std::int64_t>>& result) const {
        if (!result) return std::nullopt;
        if (result->second > baselineTime) {
            probe.baselineTime = result->second;
            return result->first;
        }
        // libbox removes an existing history record when the probe fails.
        if (hadHistory && result->second == 0) return 0;
        return std::nullopt;
    }
};

inline huxerui::Task<void> WaitForNativeProxyTimestamp(std::int64_t baselineTime) {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto second = floor<seconds>(now);
    // libbox timestamps have one-second resolution. A quick retry needs a
    // different timestamp even if it measures the same latency.
    if (baselineTime == second.time_since_epoch().count())
        co_await huxerui::Delay(second + seconds{1} - now);
}

inline constexpr auto kNativeProxyPollInterval = std::chrono::milliseconds{250};
inline constexpr int kNativeProxyPollAttempts = 120;

template<class Start, class Poll, class Valid>
huxerui::Task<std::optional<int>> MeasureNativeProxyDelay(std::string tag, std::shared_ptr<ProxyProbe> probe,
    Start start, Poll poll, Valid valid) {
    const auto history = NativeProxyHistory::Capture(*probe, poll(tag));
    co_await WaitForNativeProxyTimestamp(history.baselineTime);
    if (!valid() || !(co_await start(tag))) co_return std::nullopt;
    for (int attempt = 0; attempt < kNativeProxyPollAttempts; ++attempt) {
        co_await huxerui::Delay(kNativeProxyPollInterval);
        if (!valid()) co_return std::nullopt;
        if (const auto result = history.Resolve(*probe, poll(tag))) co_return *result;
    }
    co_return 0;
}

struct ProxyProbeTarget {
    std::string tag;
    std::shared_ptr<ProxyProbe> probe;
    NativeProxyHistory nativeHistory{};
};

inline std::vector<ProxyProbeTarget> CollectProxyGroupTargets(
    ProxyTestModel& model, const wire::Proxies& proxies, const std::string& group) {
    const auto root = proxies.entries.find(group);
    if (root == proxies.entries.end() || !root->second.members) return {};

    // sing-box tests the leaves of nested groups. A leaf can be reachable
    // through several paths but has only one history record.
    std::unordered_set<std::string_view> visited{root->first};
    std::vector<const std::vector<std::string>*> pending{&*root->second.members};
    std::vector<ProxyProbeTarget> targets;
    while (!pending.empty()) {
        const auto& members = *pending.back();
        pending.pop_back();
        for (const auto& tag : members) {
            if (!visited.insert(tag).second) continue;
            const auto entry = proxies.entries.find(tag);
            if (entry == proxies.entries.end()) continue;
            const auto& proxy = entry->second;
            if (proxy.members) {
                pending.push_back(&*proxy.members);
            } else {
                targets.push_back({tag, model.Get(tag, proxy.delay, false, proxy.testTime)});
            }
        }
    }
    return targets;
}

template<class Valid, class Poll>
huxerui::Task<bool> ClaimProxyGroupTargets(ProxyGroupTest& batch, int ticket,
    std::vector<ProxyProbeTarget>& targets, bool native, Valid valid, Poll poll) {
    while (batch.Current(ticket) && valid()) {
        const auto occupied = std::ranges::find_if(targets, [](const auto& target) {
            return target.probe->owner != 0;
        });
        if (occupied == targets.end()) break;
        // One occupied node is enough to hold this batch. Check the whole set
        // again only when that node is released, immediately before claiming.
        do {
            co_await huxerui::Delay(std::chrono::milliseconds{100});
        } while (occupied->probe->owner != 0 && batch.Current(ticket) && valid());
    }
    if (!batch.Current(ticket) || !valid()) co_return false;

    // Claim the entire set before yielding so an overlapping batch or single
    // test cannot start between the availability check and the claim.
    batch.claimed.reserve(targets.size());
    for (auto& target : targets) {
        const auto previous = poll(target.tag);
        if (native) target.nativeHistory = NativeProxyHistory::Capture(*target.probe, previous);
        const auto baseline = native ? target.nativeHistory.baselineTime : (previous ? previous->second : 0);
        batch.Start(target.probe, ticket, baseline);
    }
    co_return true;
}

// One kernel-side group test supplies delays for every target. This replaces
// UI-side per-node fan-out so the kernel owns the group URL, concurrency and
// history write order (sing-box /group/{name}/delay semantics).
template<class Measure, class Valid, class Poll>
void LaunchProxyGroupProbeBatch(huxerui::TaskScope tasks, std::shared_ptr<ProxyGroupTest> batch,
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
    tasks.Launch([batch, targets = std::move(targets), ticket, valid, poll, result]() -> huxerui::Task<void> {
        for (int attempt = 0; attempt < 124; ++attempt) {
            if (!batch->Current(ticket)) co_return;
            if (!valid()) { batch->End(ticket, false); co_return; }
            if (result->done) break;
            for (const auto& target : targets) {
                const auto& probe = target.probe;
                if (!probe->value.Get().testing) continue;
                const auto measured = poll(target.tag);
                if (measured && measured->first > 0 &&
                    measured->second > probe->baselineTime.Get()) {
                    batch->Complete(probe, ticket, measured->first);
                    probe->baselineTime = measured->second;
                }
            }
            co_await huxerui::Delay(std::chrono::duration<double>{0.25});
        }
        if (!batch->Current(ticket)) co_return;
        if (!valid()) { batch->End(ticket, false); co_return; }
        for (const auto& target : targets) {
            const auto found = result->delays.find(target.tag);
            batch->Complete(target.probe, ticket, found == result->delays.end() ? 0 : found->second);
        }
        batch->End(ticket);
    });
}

template<class Valid, class Poll>
void LaunchNativeProxyProbeBatch(huxerui::TaskScope tasks, std::shared_ptr<ProxyGroupTest> batch,
    std::vector<ProxyProbeTarget> targets, int ticket, Valid valid, Poll poll) {
    tasks.Launch([batch, targets = std::move(targets), ticket, valid, poll]() -> huxerui::Task<void> {
        for (int attempt = 0; attempt < kNativeProxyPollAttempts; ++attempt) {
            co_await huxerui::Delay(kNativeProxyPollInterval);
            if (!batch->Current(ticket)) co_return;
            if (!valid()) { batch->End(ticket, false); co_return; }
            for (const auto& target : targets) {
                const auto& probe = target.probe;
                if (!probe->value.Get().testing) continue;
                if (const auto result = target.nativeHistory.Resolve(*probe, poll(target.tag)))
                    batch->Complete(probe, ticket, *result);
            }
            if (batch->pending == 0) break;
        }
        batch->End(ticket);
    });
}
} // namespace clashflux::ui
