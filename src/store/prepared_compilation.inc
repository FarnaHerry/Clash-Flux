// Immutable accepted compilations; cache locks never span file IO or downloads.
    struct CachedCompilation {
        singbox::CompileOptions request, prepared;
        singbox::CompileResult result;
        std::chrono::steady_clock::time_point created;
        std::size_t bytes = 0;
    };
    std::mutex compilationCacheMutex_, resourceCommitMutex_, builtinGeoMutex_;
    std::deque<std::shared_ptr<const CachedCompilation>> cachedCompilations_;

    bool reuseCompilation(const singbox::CompileOptions& request,
                          singbox::CompileOptions& prepared, singbox::CompileResult& result) {
        if (!request.forceProviderRefreshSources.empty() || !request.refreshProviderKey.empty()) return false;
        std::vector<std::shared_ptr<const CachedCompilation>> snapshots;
        {
            std::lock_guard lock(compilationCacheMutex_);
            snapshots.assign(cachedCompilations_.begin(), cachedCompilations_.end());
        }
        for (const auto& snapshot : snapshots) {
            if (snapshot->request != request ||
                std::chrono::steady_clock::now() - snapshot->created > std::chrono::seconds{60}) continue;
            const auto& cache = *snapshot;
            const auto fresh = [](const auto& resource, const std::filesystem::path& root) {
                if (!resource.intervalSeconds) return true;
                std::error_code ec;
                const auto stamp = std::filesystem::last_write_time(root / (resource.cacheKey + ".cache"), ec);
                return !ec && decltype(stamp)::clock::now() - stamp < std::chrono::seconds(resource.intervalSeconds);
            };
            // Full identities and bytes remain authoritative, including external edits.
            for (const auto& resource : cache.result.httpRuleProviders) {
                std::string error;
                const auto body = singbox::ReadHttpRuleProviderCache(resource, request.ruleProviderCacheDir, error);
                const auto expected = cache.prepared.ruleProviderContents.find(resource.cacheKey);
                if (!fresh(resource, request.ruleProviderCacheDir) || !body ||
                    expected == cache.prepared.ruleProviderContents.end() || *body != expected->second) return false;
            }
            for (const auto& resource : cache.result.httpProxyProviders) {
                if (!resource.required) continue;
                std::string error;
                const auto body = singbox::ReadHttpProxyProviderCache(resource, request.proxyProviderCacheDir, error);
                const auto expected = cache.prepared.proxyProviderContents.find(resource.cacheKey);
                if (!fresh(resource, request.proxyProviderCacheDir) || !body ||
                    expected == cache.prepared.proxyProviderContents.end() || *body != expected->second) return false;
            }
            prepared = cache.prepared;
            result = cache.result;
            return true;
        }
        return false;
    }

    void rememberCompilation(singbox::CompileOptions request,
                             const singbox::CompileOptions& prepared, const singbox::CompileResult& result) {
        const auto format = singbox::DetectProfileFormat(request.profileYaml);
        if (result.hasFileProviders || !result.ruleSetResources.empty() ||
            (format != singbox::ProfileFormat::ClashYaml && format != singbox::ProfileFormat::NodeSubscription)) return;
        const auto optionBytes = [](const auto& input) {
            std::size_t bytes = sizeof(input) + input.profileYaml.size();
            for (const auto& source : input.auxiliarySources) bytes += sizeof(source) + source.content.size();
            for (const auto* contents : {&input.ruleProviderContents, &input.proxyProviderContents})
                for (const auto& [key, body] : *contents) bytes += 128 + key.size() + body.size();
            return bytes;
        };
        // Account for both retained options, raw snapshots and result projection.
        std::size_t bytes = sizeof(CachedCompilation) + optionBytes(request) + optionBytes(prepared) + result.json.size() * 4;
        for (const auto* contents : {&result.ruleProviderContents, &result.proxyProviderContents})
            for (const auto& [key, body] : *contents) bytes += 128 + key.size() + body.size();
        if (bytes > 32 * 1024 * 1024) return;
        request.forceProviderRefreshSources.clear(); request.refreshProviderKey.clear();
        auto accepted = prepared;
        accepted.forceProviderRefreshSources.clear(); accepted.refreshProviderKey.clear();
        auto snapshot = std::make_shared<const CachedCompilation>(CachedCompilation{
            std::move(request), std::move(accepted), result, std::chrono::steady_clock::now(), bytes});
        std::lock_guard lock(compilationCacheMutex_);
        std::erase_if(cachedCompilations_, [&](const auto& entry) {
            return entry->request == snapshot->request ||
                std::chrono::steady_clock::now() - entry->created > std::chrono::seconds{60};
        });
        cachedCompilations_.push_front(std::move(snapshot));
        std::size_t total = 0;
        for (const auto& entry : cachedCompilations_) total += entry->bytes;
        while (cachedCompilations_.size() > 4 || total > 32 * 1024 * 1024) {
            total -= cachedCompilations_.back()->bytes;
            cachedCompilations_.pop_back();
        }
    }
