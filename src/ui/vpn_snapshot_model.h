#pragma once

#include <huxerui/huxerui.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace clashflux::ui {

// UI-thread publication shared by action completions and the background pump.
// Snapshot is an owning DTO; worker threads never access this model's State.
template <class Snapshot> struct VpnSnapshotModel {
    huxerui::State<Snapshot> view{Snapshot{}};
    huxerui::State<std::uint64_t> syncTick{0};

    void RequestSync() { syncTick = syncTick.Get() + 1; }

    void Publish(Snapshot next) {
        // Invalidate polls started before this action returned, even when the
        // new snapshot compares equal (connect/disconnect/connect can be ABA).
        RequestSync();
        view = std::move(next);
    }

    bool PublishPolled(std::uint64_t before, Snapshot next) {
        if (syncTick.Get() != before) return false;
        view = std::move(next);
        return true;
    }
};

// Startup pages mount before SQLite hydration. Do not call the store until
// loaded, and never let a worker initialization error escape the UI task.
template<class Model, class Load, class Failed>
void LaunchHydratedVpnSnapshot(huxerui::TaskScope tasks, bool loaded,
                               std::shared_ptr<Model> model, Load load, Failed failed) {
    if (!loaded) return;
    const auto before = model->syncTick.Get();
    tasks.Launch([model, before, load = std::move(load), failed = std::move(failed)]() mutable
                     -> huxerui::Task<void> {
        try {
            auto next = co_await load();
            model->PublishPolled(before, std::move(next));
        } catch (const std::exception& error) {
            failed(std::string{error.what()});
        } catch (...) {
            failed(std::string{"VPN initialization failed"});
        }
    });
}

} // namespace clashflux::ui
