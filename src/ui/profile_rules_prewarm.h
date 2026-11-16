#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <utility>

namespace clashflux::ui {
// Only owned source identity and pending IDs, never another artifact cache.
// Access on the application thread; the existing profile pump drives retries.
struct ProfileRulesPrewarm {
    struct Source {
        std::string file, type, name;
        std::int64_t updatedAt = 0;
        std::uint64_t record = 0, generation = 0;
    };
    std::map<std::int64_t, Source> sources;
    std::deque<std::int64_t> pending;
    std::uint64_t serial = 0;
    bool observed = false, startup = false, running = false;

    void Queue(std::int64_t id) {
        if (id && sources.contains(id) && std::ranges::find(pending, id) == pending.end()) pending.push_back(id);
    }
    std::uint64_t Generation(std::int64_t id) const {
        const auto it = sources.find(id);
        return it == sources.end() ? 0 : it->second.generation;
    }
    template<class Profiles, class Revision>
    void Observe(const Profiles& profiles, Revision revision, std::uint64_t global) {
        // DataSession selection assigns one version to every row. It changes
        // runtime selection without changing source bytes or provider content.
        const bool selection = profiles.size() > 1 && std::ranges::all_of(profiles,
            [&](const auto& profile) { return revision(profile.id) == global; });
        std::map<std::int64_t, Source> next;
        for (const auto& profile : profiles) {
            if (profile.id == 0 || profile.type == "pptp" || profile.file.empty()) continue;
            const auto version = revision(profile.id);
            const auto old = sources.find(profile.id);
            const bool changed = old == sources.end() || old->second.file != profile.file ||
                old->second.type != profile.type || old->second.name != profile.name || old->second.updatedAt != profile.updatedAt ||
                (!selection && old->second.record != version);
            const auto generation = changed ? ++serial : old->second.generation;
            next.emplace(profile.id, Source{profile.file, profile.type, profile.name, profile.updatedAt, version, generation});
            // Hydration does not enqueue every historical source. Startup warms
            // one eligible source once its existing format catalog is ready.
            if (observed && changed && std::ranges::find(pending, profile.id) == pending.end()) pending.push_back(profile.id);
        }
        sources = std::move(next);
        std::erase_if(pending, [&](auto id) { return !sources.contains(id); });
        observed = true;
    }
};
} // namespace clashflux::ui
