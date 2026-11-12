// Included privately by CoreStore. One bounded, short-lived accepted compilation.
    struct CachedCompilation {
        singbox::CompileOptions request, prepared;
        singbox::CompileResult result;
        std::chrono::steady_clock::time_point created;
    };
    std::mutex compilationMutex_;
    std::optional<CachedCompilation> cachedCompilation_;

    bool reuseCompilation(const singbox::CompileOptions& request,
                          singbox::CompileOptions& prepared, singbox::CompileResult& result) {
        if (!cachedCompilation_ || !request.forceProviderRefreshSources.empty() ||
            !request.refreshProviderKey.empty() || cachedCompilation_->request != request ||
            std::chrono::steady_clock::now() - cachedCompilation_->created > std::chrono::seconds{60}) return false;
        const auto& cache = *cachedCompilation_;
        // Full identities and bytes are re-read; filenames/mtime alone are not trust.
        const auto fresh = [](const auto& resource, const std::filesystem::path& root) {
            if (!resource.intervalSeconds) return true;
            std::error_code ec;
            const auto stamp = std::filesystem::last_write_time(root / (resource.cacheKey + ".cache"), ec);
            return !ec && decltype(stamp)::clock::now() - stamp < std::chrono::seconds{resource.intervalSeconds};
        };
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
    void rememberCompilation(singbox::CompileOptions request,
                             const singbox::CompileOptions& prepared, const singbox::CompileResult& result) {
        // File providers and built-in GEO have independently changing disk inputs.
        // Their existing preparation/check path is retained until fully fingerprinted.
        const auto format = singbox::DetectProfileFormat(request.profileYaml);
        if (result.hasFileProviders || !result.ruleSetResources.empty() ||
            (format != singbox::ProfileFormat::ClashYaml && format != singbox::ProfileFormat::NodeSubscription)) return;
        std::size_t bytes = result.json.size() * 2 + request.profileYaml.size();
        for (const auto& [key, body] : prepared.ruleProviderContents) bytes += key.size() + body.size();
        for (const auto& [key, body] : prepared.proxyProviderContents) bytes += key.size() + body.size();
        if (bytes > 32 * 1024 * 1024) return;
        request.forceProviderRefreshSources.clear(); request.refreshProviderKey.clear();
        auto accepted = prepared;
        accepted.forceProviderRefreshSources.clear(); accepted.refreshProviderKey.clear();
        cachedCompilation_ = CachedCompilation{std::move(request), std::move(accepted), result,
                                               std::chrono::steady_clock::now()};
    }
