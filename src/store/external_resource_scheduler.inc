// One bounded scheduler pass, on an application worker, independent of profile
// downloads and window lifetime. The lifecycle lock serializes candidate commit.
    ExternalResourceRefreshBatch refreshDueExternalResources(std::stop_token cancel = {}) {
        ExternalResourceRefreshBatch batch;
        std::unique_lock lock(lifecycleMutex_, std::try_to_lock);
        if (cancel.stop_requested() || !lock.owns_lock() || !clashflux::persistence::persistence().ready()) {
            batch.deferred = true;
            return batch;
        }
        ensureOpen();
        struct Pending { std::int64_t id; std::shared_ptr<const std::string> source; singbox::ExternalResource resource; };
        std::map<std::string, Pending> pending;
        for (const auto& profile : db_->listProfiles()) {
            if (cancel.stop_requested()) return batch;
            std::string error;
            const auto source = singbox::ReadRuleProviderText(cfg::profilesDir() / profile.file, 8 * 1024 * 1024, error);
            if (!source || singbox::DetectProfileFormat(*source) != singbox::ProfileFormat::ClashYaml) continue;
            const auto resources = singbox::InspectExternalResources(*source, error, routing::ConnectionId(profile.id));
            if (!error.empty()) continue; // malformed declarations are never executed
            const auto sharedSource = std::make_shared<const std::string>(*source);
            for (const auto& resource : resources)
                if (!resource.cacheKey.empty() && resource.intervalSeconds)
                    pending.emplace(resource.identity, Pending{profile.id, sharedSource, resource});
        }
        std::erase_if(resourceRetries_, [&](const auto& item) { return !pending.contains(item.first); });
        // Rotate the starting identity to avoid starvation by a slow/failing
        // first profile; at most three resources are attempted per 30s pass.
        auto next = pending.upper_bound(resourceCursor_);
        const auto count = pending.size();
        for (std::size_t visited = 0; visited < count && batch.attempted < 3; ++visited) {
            if (cancel.stop_requested()) break;
            if (next == pending.end()) next = pending.begin();
            const auto& [identity, item] = *next++;
            const auto now = std::chrono::steady_clock::now();
            const auto retry = resourceRetries_.find(identity);
            if (retry != resourceRetries_.end() && now < retry->second.next) continue;
            const auto root = cfg::coreWorkDir() / (item.resource.kind == "proxy-provider" ? "proxy-providers" : "rule-providers");
            std::error_code ec;
            const auto stamp = std::filesystem::last_write_time(root / (item.resource.cacheKey + ".cache"), ec);
            const auto fileNow = std::filesystem::file_time_type::clock::now();
            if (!ec && stamp <= fileNow && fileNow - stamp < std::chrono::seconds(item.resource.intervalSeconds)) continue;
            resourceCursor_ = identity;
            ++batch.attempted;
            bool applied = false;
            std::string error;
            if (refreshExternalResource(item.id, *item.source, item.resource, applied, error, cancel)) {
                resourceRetries_.erase(identity);
                ++batch.updated;
                if (applied) ++batch.applied;
            } else {
                auto& state = resourceRetries_[identity];
                state.failures = std::min(state.failures + 1, 5U);
                state.next = std::chrono::steady_clock::now() + std::chrono::seconds(30U << state.failures);
                batch.errors.push_back("source_id=" + std::to_string(item.id) + ": " + error);
                stream::logApplication("warning", "external-resource-update " + batch.errors.back());
            }
        }
        return batch;
    }
