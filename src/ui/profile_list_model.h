#pragma once

#include <huxerui/huxerui.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>
#include <vector>
#include "profile_refresh_model.h"

namespace clashflux::ui {

template<class Profile> struct ProfileListSnapshot {
    std::vector<Profile> profiles;
    std::vector<std::int64_t> clashSources;
    std::vector<std::int64_t> ruleSources;
    bool ready = false;
};

// UI-thread state shared by action completions and the background reader.
template<class Profile> struct ProfileListModel : ProfileRefreshModel {
    huxerui::State<std::vector<Profile>> list{std::vector<Profile>{}};
    huxerui::State<bool> loaded{false};
    huxerui::State<std::vector<std::int64_t>> clashSources{std::vector<std::int64_t>{}};
    huxerui::State<std::vector<std::int64_t>> ruleSources{std::vector<std::int64_t>{}};
    huxerui::State<bool> ruleSourcesReady{false};
    huxerui::State<bool> selectionPending{false};
    huxerui::State<bool> reorderPending{false};
    huxerui::State<std::uint64_t> syncTick{0};
    std::uint64_t selectionSerial = 0;
    std::stop_source selectionCancel;
    std::optional<std::int64_t> selectionTarget;
    bool selectionRunning = false;
    std::function<huxerui::Task<void>()> queuedSelection;

    bool IsRuleSource(std::int64_t id) const {
        if (!ruleSourcesReady.Get()) return false;
        const auto& eligible = ruleSources.Get();
        if (std::ranges::find(eligible, id) == eligible.end()) return false;
        const auto& profiles = list.Get();
        const auto found = std::ranges::find(profiles, id, &Profile::id);
        return found != profiles.end() && found->type != "pptp";
    }
    std::vector<Profile> RuleSources() const {
        std::vector<Profile> sources;
        if (!ruleSourcesReady.Get()) return sources;
        const auto& eligible = ruleSources.Get();
        for (const auto& profile : list.Get())
            if (profile.type != "pptp" && std::ranges::find(eligible, profile.id) != eligible.end())
                sources.push_back(profile);
        return sources;
    }
    std::int64_t ResolveRuleSource(std::int64_t current) const {
        if (!ruleSourcesReady.Get()) return 0;
        const auto& eligible = ruleSources.Get();
        std::int64_t first = 0, main = 0;
        for (const auto& profile : list.Get()) {
            if (profile.type == "pptp" || std::ranges::find(eligible, profile.id) == eligible.end()) continue;
            if (profile.id == current) return current;
            if (!first) first = profile.id;
            if (profile.selected) main = profile.id;
        }
        return main ? main : first;
    }

    void RequestSync() { syncTick = syncTick.Get() + 1; }
    void Publish(std::vector<Profile> next) {
        // Even equal content invalidates an in-flight older snapshot (ABA).
        RequestSync();
        list = std::move(next);
    }
    // 拖放完成：把新顺序写透权威模型。解除 reorderPending 时镜像立即同步
    // 新顺序，不被旧列表覆盖回弹；Publish 同时使在途旧快照失效（ABA）。
    // 顺序与当前列表不完整对应时拒绝发布，交给后续重读兜底。
    bool PublishOrder(const std::vector<std::int64_t>& ids) {
        auto current = list.Get();
        std::vector<Profile> ordered;
        ordered.reserve(current.size());
        for (const std::int64_t id : ids) {
            const auto it = std::ranges::find(current, id, &Profile::id);
            if (it == current.end()) return false;
            ordered.push_back(std::move(*it));
        }
        if (ordered.size() != current.size()) return false;
        Publish(std::move(ordered));
        return true;
    }
};

// A newer selection cancels preparation and owns publication, including ABA.
// Work stays application-owned when its originating card/page disappears.
template<class Result, class Model, class Work, class Completed>
std::uint64_t LaunchProfileSelection(huxerui::TaskScope tasks, std::shared_ptr<Model> model,
                                    std::int64_t target, Work work, Completed completed) {
    if (model->selectionPending.Get() && model->selectionTarget == target)
        return model->selectionSerial;
    model->selectionCancel.request_stop();
    model->selectionCancel = std::stop_source{};
    const auto token = model->selectionCancel.get_token();
    const auto serial = ++model->selectionSerial;
    model->selectionTarget = target;
    model->selectionPending = true;
    model->RequestSync();
    // One active job and one replaceable pending target; never fill the worker
    // pool with jobs waiting on the same lifecycle lock or serialize in parallel.
    model->queuedSelection = [model, serial, token, work = std::move(work), completed = std::move(completed)]() mutable
                     -> huxerui::Task<void> {
        std::optional<Result> result;
        std::exception_ptr error;
        try { result.emplace(co_await work(token)); }
        catch (...) { error = std::current_exception(); }
        if (model->selectionSerial != serial) co_return;
        model->selectionPending = false;
        completed(std::move(result), error, serial);
    };
    if (!model->selectionRunning) {
        model->selectionRunning = true;
        tasks.Launch([model]() -> huxerui::Task<void> {
            struct Release {
                std::shared_ptr<Model> owner;
                ~Release() {
                    owner->queuedSelection = {};
                    owner->selectionRunning = false;
                    owner->selectionPending = false;
                }
            } release{model};
            while (model->queuedSelection) {
                auto next = std::move(model->queuedSelection);
                model->queuedSelection = {};
                co_await next();
            }
        });
    }
    return serial;
}

template<class Model, class Revision, class Read>
void DriveProfileList(huxerui::TaskScope tasks, std::shared_ptr<Model> model,
                      Revision revision, Read read) {
    tasks.Launch([model, revision, read]() -> huxerui::Task<void> {
        std::uint64_t seenRevision = 0, seenSync = 0;
        bool first = true;
        for (;;) {
            const auto before = model->syncTick.Get();
            const auto version = revision();
            if (first || version != seenRevision || before != seenSync) {
                auto next = co_await read();
                if (!model->selectionPending.Get() && !model->reorderPending.Get() &&
                    model->syncTick.Get() == before && revision() == version) {
                    model->clashSources = std::move(next.clashSources);
                    model->ruleSources = std::move(next.ruleSources);
                    model->ruleSourcesReady = next.ready;
                    model->list = std::move(next.profiles);
                    if (next.ready) model->loaded = true;
                    // A rejected/deferred read is still owed to the UI. Never
                    // acknowledge it before publication actually succeeds.
                    first = false;
                    seenRevision = version;
                    seenSync = before;
                }
            }
            if (model->syncTick.Get() != before || revision() != version) continue;
            co_await huxerui::Delay(std::chrono::seconds{1});
        }
    });
}

} // namespace clashflux::ui
