// Included in CoreStore's private definition. Persistent cache ownership and
// final candidate commits remain in the provider preparation transaction.
    struct ProviderDownloadStats {
        std::mutex mutex;
        std::int64_t queueMicros = 0, sharedWaitMicros = 0, fetchMicros = 0, networkMicros = 0;
        std::uint64_t bytes = 0;
        unsigned requests = 0, shared = 0, notModified = 0, measuredRequests = 0;
        long newConnections = 0;
        void record(const provider_resources::Outcome& outcome) {
            std::lock_guard lock(mutex);
            queueMicros += outcome.queueMicros;
            sharedWaitMicros += outcome.sharedWaitMicros;
            fetchMicros += outcome.fetchMicros;
            if (outcome.joined) { ++shared; return; }
            if (!outcome.response) return;
            const auto& response = *outcome.response;
            requests += response.attempts;
            networkMicros += response.networkMicros;
            bytes += response.bytes;
            if (response.status == 304) ++notModified;
            if (response.connectionsKnown) {
                measuredRequests += response.attempts;
                newConnections += response.newConnections;
            }
        }
    };

    rule_provider_cache::FetchResult downloadProvider(
            const provider_resources::Request& request, const std::filesystem::path& file,
            ProviderDownloadStats& stats, std::stop_token cancel) {
        const auto outcome = providerResources_.fetch(request, [this, file](const auto& pending) {
            provider_resources::Response result;
            api::ClashApi::DownloadOptions options;
            options.timeoutSecs = pending.timeoutSecs;
            options.attempts = pending.attempts;
            options.maxBytes = pending.maxBytes;
            options.headers = pending.headers;
            options.allowProxyEnv = pending.allowProxyEnv;
            options.proxyUrl = pending.proxyUrl;
            options.acceptNotModified = pending.acceptNotModified;
            // Runtime-owned transport retains safe connections between resources
            // and refreshes, independently of controller credentials.
            const auto response = providerDownloader_.downloadToFile(pending.url, file, options);
            result.ok = response.ok;
            result.networkMicros = response.download.networkMicros;
            result.bytes = response.download.bytes;
            result.connectionsKnown = response.download.transportKnown;
            result.newConnections = response.download.newConnections;
            result.attempts = response.download.attempts;
            if (result.ok && response.status != 304) {
                auto body = singbox::ReadRuleProviderText(file, pending.maxBytes, result.error);
                result.ok = body.has_value();
                if (body) result.body = std::move(*body);
            }

            result.status = response.status;
            result.failure = response.failurePhase;
            if (!response.error.empty()) result.error = response.error;
            if (!result.ok && result.error.empty()) result.error = "HTTP " + std::to_string(result.status);
            if (const auto entry = response.headers.find("etag"); entry != response.headers.end())
                result.etag = entry->second;
            if (const auto entry = response.headers.find("last-modified"); entry != response.headers.end())
                result.lastModified = entry->second;
            return result;
        }, cancel);
        stats.record(outcome);
        // No URL, provider name, headers or raw transport errors enter diagnostics.
        const auto* diagnostic = outcome.response.get();
        stream::logApplication(diagnostic && diagnostic->ok ? "info" : "warning", std::format(
            "provider-download key={:016x} ready={} cancelled={} joined={} status={} attempts={} queue_us={} shared_wait_us={} fetch_us={} network_us={} bytes={} failure={}",
            clashflux::download::DiagnosticId(request.identity), diagnostic && diagnostic->ok, outcome.cancelled,
            outcome.joined, diagnostic ? diagnostic->status : 0, diagnostic ? diagnostic->attempts : 0,
            outcome.queueMicros, outcome.sharedWaitMicros, outcome.fetchMicros,
            diagnostic ? diagnostic->networkMicros : 0, diagnostic ? diagnostic->bytes : 0,
            outcome.cancelled ? "cancelled" : !diagnostic ? "unavailable" : diagnostic->ok ? "none" :
                diagnostic->failure.empty() ? "transport" : diagnostic->failure));
        rule_provider_cache::FetchResult fetched;
        if (outcome.cancelled) { fetched.error = "订阅准备已取消"; return fetched; }
        const auto& response = *outcome.response;
        fetched.ok = response.ok;
        fetched.status = response.status;
        fetched.notModified = response.status == 304;
        fetched.error = response.error;
        fetched.validators = {response.etag, response.lastModified};
        const bool needsCopy = outcome.joined;
        if (fetched.ok && !fetched.notModified && needsCopy) {
            // Never expose the owner's staging path to another candidate.
            std::ofstream output(file, std::ios::binary | std::ios::trunc);
            output << response.body;
            output.close();
            if (!output) { fetched.ok = false; fetched.error = "无法完成 provider 临时文件写入"; }
        }
        return fetched;
    }

    template<class Resource>
    rule_provider_cache::FetchResult downloadConditionalResource(const char* kind, const Resource& resource,
            const std::filesystem::path& file, const singbox::HttpCacheValidators& validators,
            ProviderDownloadStats& stats, std::stop_token cancel) {
        const auto request = [&](const std::map<std::string, std::string>& headers) {
            provider_resources::Request pending;
            pending.identity = std::string(kind) + resource.identity;
            pending.url = resource.url; pending.headers = headers; pending.maxBytes = resource.maxBytes;
            pending.timeoutSecs = clashflux::download::ResourceTimeout(resource.maxBytes);
            pending.acceptNotModified = clashflux::download::HasCondition(headers);
#if defined(__ANDROID__)
            pending.attempts = clashflux::download::ResourceAttempts(resource.maxBytes, true);
#endif
            return downloadProvider(pending, file, stats, cancel);
        };
#if defined(CLASHFLUX_IOS)
        const auto headers = resource.headers; // Preserve the deferred platform's explicit header boundary.
#else
        const auto headers = clashflux::download::ConditionalHeaders(resource.headers, validators.etag, validators.lastModified);
#endif
        auto fetched = request(headers);
        if (clashflux::download::HasCondition(headers) && fetched.status >= 300 && fetched.status < 400 && fetched.status != 304)
            fetched = request(resource.headers);
        return fetched;
    }
