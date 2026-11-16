#pragma once

#include <huxerui/huxerui.h>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clashflux::ui {

// Versioned profile files and persisted rule intent are the cache identity.
// Selection, traffic counters and unrelated settings do not invalidate it.
struct ProfileRulesInput {
    std::string file, type, name, edits, library;
    std::uint64_t resources = 0;
    std::uint64_t source = 0;
    bool operator==(const ProfileRulesInput&) const = default;
};

template<class Snapshot> struct ProfileRulesModel {
    struct Entry {
        ProfileRulesInput input;
        std::uint64_t generation = 0, published = 0, reading = 0, readingTargets = 0;
        std::shared_ptr<const Snapshot> value;
        std::shared_ptr<const std::vector<std::string>> targets;
        std::string targetError;
        bool loading = false, targetsLoading = false;
        std::size_t bytes = 0;
        std::uint64_t touched = 0;
    };
    // Only the application thread accesses entries; workers receive owned inputs.
    std::map<std::int64_t, Entry> entries;
    huxerui::State<std::uint64_t> revision{0};
    std::uint64_t serial = 0, clock = 0;
    std::uint64_t readsStarted = 0;
    std::size_t budgetBytes = 32*1024*1024;
    std::map<std::int64_t, unsigned> pins;

    static std::size_t Measure(const std::shared_ptr<const Snapshot>& value) {
        if (!value) return 0;
        if constexpr (requires { value->retainedBytes(); }) return value->retainedBytes();
        else if constexpr (requires { value->capacity(); }) return sizeof(Snapshot) + value->capacity();
        else return sizeof(Snapshot);
    }
    std::size_t RetainedBytes() const {
        std::size_t bytes = 0;
        for (const auto& [id, entry] : entries) {
            bytes += sizeof(Entry) + entry.bytes + entry.input.file.capacity() + entry.input.type.capacity() +
                entry.input.name.capacity() + entry.input.edits.capacity() + entry.input.library.capacity() + entry.targetError.capacity();
            if (entry.targets) {
                bytes += sizeof(*entry.targets) + entry.targets->capacity()*sizeof(std::string);
                for (const auto& target : *entry.targets) bytes += target.capacity();
            }
        }
        return bytes;
    }
    void Trim() {
        while (RetainedBytes() > budgetBytes) {
            auto victim = entries.end();
            for (auto it = entries.begin(); it != entries.end(); ++it) {
                if (it->second.loading || it->second.targetsLoading || pins.contains(it->first)) continue;
                if (victim == entries.end() || it->second.touched < victim->second.touched) victim = it;
            }
            if (victim == entries.end()) break; // Active views and in-flight values cannot be revoked.
            entries.erase(victim); // Drop owned inputs too; global serial prevents ABA after recreation.
            ++revision;
        }
    }
    void Pin(std::int64_t id) { if (id) ++pins[id]; }
    void Unpin(std::int64_t id) {
        const auto found = pins.find(id);
        if (found != pins.end() && !--found->second) pins.erase(found);
        Trim();
    }

    void SetInput(std::int64_t id, ProfileRulesInput input) {
        auto [it, inserted] = entries.try_emplace(id);
        auto& entry = it->second;
        if (!inserted && entry.input == input) return;
        entry.input = std::move(input);
        entry.generation = ++serial;
        entry.targets.reset();
        entry.targetError.clear();
        ++revision;
    }
    void Forget(std::int64_t id) {
        if (entries.erase(id)) ++revision;
    }
    void Retry(std::int64_t id) {
        const auto it = entries.find(id);
        if (it == entries.end()) return;
        it->second.generation = ++serial;
        it->second.targets.reset();
        it->second.targetError.clear();
        ++revision;
    }
    bool Ready(std::int64_t id) const {
        const auto it = entries.find(id);
        return it != entries.end() && it->second.value && it->second.published == it->second.generation;
    }
    std::shared_ptr<const Snapshot> SnapshotOf(std::int64_t id) {
        const auto it = entries.find(id);
        if (it != entries.end()) it->second.touched = ++clock;
        return it == entries.end() ? nullptr : it->second.value;
    }
    bool EditorReady(std::int64_t id) const {
        const auto it = entries.find(id);
        return Ready(id) && it->second.targets && it->second.targetError.empty();
    }
    std::optional<std::uint64_t> Begin(std::int64_t id, bool targets = false) {
        const auto it = entries.find(id);
        if (it == entries.end()) return std::nullopt;
        auto& entry = it->second;
        if (targets) {
            if (!Ready(id) || entry.targets || entry.targetsLoading) return std::nullopt;
            entry.targetsLoading = true;
            entry.readingTargets = entry.generation;
        } else {
            if (Ready(id) || entry.loading) return std::nullopt;
            entry.loading = true;
            entry.reading = entry.generation;
            ++readsStarted;
        }
        return entry.generation;
    }
    bool Publish(std::int64_t id, std::uint64_t ticket, std::shared_ptr<const Snapshot> value) {
        const auto it = entries.find(id);
        if (it == entries.end() || it->second.reading != ticket) return false;
        auto& entry = it->second;
        entry.loading = false;
        entry.reading = 0;
        if (entry.generation != ticket) { ++revision; return false; }
        entry.bytes = Measure(value);
        entry.value = std::move(value);
        entry.touched = ++clock;
        entry.published = ticket;
        ++revision;
        Trim();
        return true;
    }
    bool PublishTargets(std::int64_t id, std::uint64_t ticket,
                        std::vector<std::string> targets, std::string error = {}, std::shared_ptr<const Snapshot> upgraded = {}) {
        const auto it = entries.find(id);
        if (it == entries.end() || it->second.readingTargets != ticket) return false;
        auto& entry = it->second;
        entry.targetsLoading = false;
        entry.readingTargets = 0;
        if (entry.generation != ticket) { ++revision; return false; }
        if (upgraded) { entry.bytes = Measure(upgraded); entry.value = std::move(upgraded); }
        entry.touched = ++clock;
        entry.targets = std::make_shared<const std::vector<std::string>>(std::move(targets));
        entry.targetError = std::move(error);
        ++revision;
        Trim();
        return true;
    }
};

// Application-owned reads outlive their initiating panel. Revalidate before
// publication even if the shared data pump has not observed a write yet.
template<class Snapshot, class Read, class Validate, class Failed>
void RequestProfileRulesSnapshot(huxerui::TaskScope tasks,
    std::shared_ptr<ProfileRulesModel<Snapshot>> model, std::int64_t id,
    Read read, Validate validate, Failed failed, std::function<void()> finished = {}) {
    const auto ticket = model->Begin(id);
    if (!ticket) { if (finished) finished(); return; }
    try {
        tasks.Launch([=]() -> huxerui::Task<void> {
            std::shared_ptr<const Snapshot> value;
            try { value = co_await read(); }
            catch (...) { value = failed(std::current_exception()); }
            validate();
            model->Publish(id, *ticket, std::move(value));
            if (finished) finished();
        });
    } catch (...) {
        auto value = failed(std::current_exception());
        validate();
        model->Publish(id, *ticket, std::move(value));
        if (finished) finished();
    }
}

} // namespace clashflux::ui
