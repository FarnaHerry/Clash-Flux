#pragma once
#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace clashflux::target_source {
// Template arguments keep this ordinary header independent of named modules.
// The application instantiates it only for CompileOptions/TargetSourceModel.
// Weak artifacts: the shared page snapshot owns the payload, this index never
// adds a second retained artifact cache. Expensive loads run on task threads.
template<class Options, class Model> class Cache {
public:
    struct Result { std::shared_ptr<const Model> model; std::string error; bool reused = false; };
private:
    struct Entry { Options input; std::uint64_t resources = 0; std::weak_ptr<const Model> model; std::size_t bytes = 0; };
    std::deque<Entry> entries_;
    mutable std::mutex mutex_;
    std::size_t budget_;
public:
    explicit Cache(std::size_t budget = 8*1024*1024) : budget_(budget) {}
    template<class ResultType> static bool Reusable(const Options& options, const ResultType& result) {
        if (result.hasFileProviders || !result.ruleSetResources.empty()) return false;
        for (const auto& resource : result.httpRuleProviders)
            if (!options.ruleProviderContents.contains(resource.cacheKey)) return false;
        for (const auto& resource : result.httpProxyProviders)
            if (resource.required && !options.proxyProviderContents.contains(resource.cacheKey)) return false;
        // Only source-scoped requests have a bounded key representation here.
        return options.auxiliarySources.empty() && options.globalRules.empty() && options.nativeConnections.empty() &&
            options.profileRuleEdits.rules.empty() && options.ruleSetLibrary.resources.empty() &&
            options.forceProviderRefreshSources.empty() && options.refreshProviderKey.empty() && options.tunExcludeAddresses.empty() &&
            options.network == decltype(options.network){};
    }
    template<class Load> Result Get(const Options& options, std::uint64_t resources, Load load) {
        std::lock_guard lock(mutex_); // One capture at a time, including different large sources.
        std::erase_if(entries_, [](const auto& entry) { return entry.model.expired(); });
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->resources != resources || it->input != options) continue;
            if (auto model = it->model.lock()) {
                auto entry = std::move(*it); entries_.erase(it); entries_.push_front(std::move(entry));
                return {std::move(model), {}, true};
            }
        }
        auto result = load();
        if (!result.error.empty() || !result.targetSource)
            return {{}, result.error.empty() ? "来源尚未生成目标规则" : result.error, false};
        std::size_t bytes = sizeof(Entry) + options.profileYaml.capacity() + options.mainConnectionId.capacity() +
            options.mainSourceName.capacity() + options.ruleSetDir.capacity() + options.ruleProviderDir.capacity() +
            options.ruleProviderCacheDir.capacity() + options.proxyProviderCacheDir.capacity() + options.secret.capacity() +
            options.controller.capacity() + options.mode.capacity() + options.logLevel.capacity();
        for (const auto* bodies : {&options.ruleProviderContents, &options.proxyProviderContents})
            for (const auto& [key, body] : *bodies) bytes += 128 + key.capacity() + body.capacity();
        if (bytes <= budget_ && Reusable(options, result)) {
            entries_.push_front({options, resources, result.targetSource, bytes});
            std::size_t total = 0; for (const auto& entry : entries_) total += entry.bytes;
            while (entries_.size() > 4 || total > budget_) { total -= entries_.back().bytes; entries_.pop_back(); }
        }
        return {std::move(result.targetSource), {}, false};
    }
    std::size_t retainedKeyBytes() const {
        std::lock_guard lock(mutex_); std::size_t bytes = 0;
        for (const auto& entry : entries_) bytes += entry.bytes;
        return bytes;
    }
};
} // namespace clashflux::target_source
