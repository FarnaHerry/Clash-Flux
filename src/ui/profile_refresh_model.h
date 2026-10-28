#pragma once

#include <huxerui/huxerui.h>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <utility>

namespace clashflux::ui {

// Application-owned, UI-thread state. Completed versions remain available so
// a rules snapshot can distinguish its own refresh from another profile's.
struct ProfileRefreshModel {
    struct Refresh {
        std::uint64_t version = 0;
        bool busy = false;
        bool operator==(const Refresh&) const = default;
    };
    huxerui::State<std::map<std::int64_t, Refresh>> refreshes{
        std::map<std::int64_t, Refresh>{}};
    std::uint64_t nextRefreshVersion = 0;

    bool Refreshing(std::int64_t id) const {
        const auto& current = refreshes.Get();
        const auto found = current.find(id);
        return found != current.end() && found->second.busy;
    }
    std::uint64_t RefreshVersion(std::int64_t id) const {
        const auto& current = refreshes.Get();
        const auto found = current.find(id);
        return found == current.end() ? 0 : found->second.version;
    }
    bool RulesLoading(std::int64_t id, std::optional<std::uint64_t> snapshotVersion) const {
        return Refreshing(id) || !snapshotVersion || *snapshotVersion != RefreshVersion(id);
    }
    std::optional<std::uint64_t> BeginRefresh(std::int64_t id) {
        if (Refreshing(id)) return std::nullopt;
        const auto version = ++nextRefreshVersion;
        refreshes.Update([&](auto& current) { current[id] = {version, true}; });
        return version;
    }
    bool FinishRefresh(std::int64_t id, std::uint64_t version) {
        if (!Refreshing(id) || RefreshVersion(id) != version) return false;
        refreshes.Update([&](auto& current) { current.at(id).busy = false; });
        return true;
    }
};

// The application scope owns the task after its card/page retires. Every exit
// clears only this ID's matching operation before publishing its result.
template<class Result, class Model, class Work, class Completed>
bool LaunchProfileRefresh(huxerui::TaskScope tasks, std::shared_ptr<Model> model,
                          std::int64_t id, Work work, Completed completed) {
    const auto version = model->BeginRefresh(id);
    if (!version) return false;
    try {
        tasks.Launch([model, id, version = *version, work = std::move(work), completed]() mutable
                     -> huxerui::Task<void> {
            std::optional<Result> result;
            std::exception_ptr error;
            try { result.emplace(co_await work()); }
            catch (...) { error = std::current_exception(); }
            if (model->FinishRefresh(id, version)) completed(std::move(result), error);
        });
    } catch (...) {
        if (model->FinishRefresh(id, *version)) completed(std::optional<Result>{}, std::current_exception());
    }
    return true;
}

} // namespace clashflux::ui
