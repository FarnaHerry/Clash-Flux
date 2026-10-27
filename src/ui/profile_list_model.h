#pragma once

#include <huxerui/huxerui.h>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>
#include <vector>

namespace clashflux::ui {

template<class Profile> struct ProfileListSnapshot {
    std::vector<Profile> profiles;
    std::vector<std::int64_t> clashSources;
    bool ready = false;
};

// UI-thread state shared by action completions and the background reader.
template<class Profile> struct ProfileListModel {
    huxerui::State<std::vector<Profile>> list{std::vector<Profile>{}};
    huxerui::State<bool> loaded{false};
    huxerui::State<std::vector<std::int64_t>> clashSources{std::vector<std::int64_t>{}};
    huxerui::State<bool> selectionPending{false};
    huxerui::State<bool> reorderPending{false};
    huxerui::State<std::uint64_t> syncTick{0};
    std::uint64_t selectionSerial = 0;
    std::stop_source selectionCancel;
    std::optional<std::int64_t> selectionTarget;
    bool selectionRunning = false;
    std::function<huxerui::Task<void>()> queuedSelection;

    void RequestSync() { syncTick = syncTick.Get() + 1; }
    void Publish(std::vector<Profile> next) {
        // Even equal content invalidates an in-flight older snapshot (ABA).
        RequestSync();
        list = std::move(next);
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
