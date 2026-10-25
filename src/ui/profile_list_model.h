#pragma once

#include <huxerui/huxerui.h>
#include <chrono>
#include <cstdint>
#include <memory>
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

    void RequestSync() { syncTick = syncTick.Get() + 1; }
    void Publish(std::vector<Profile> next) {
        // Even equal content invalidates an in-flight older snapshot (ABA).
        RequestSync();
        list = std::move(next);
    }
};

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
