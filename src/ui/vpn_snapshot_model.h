#pragma once

#include <huxerui/huxerui.h>

#include <cstdint>
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

} // namespace clashflux::ui
