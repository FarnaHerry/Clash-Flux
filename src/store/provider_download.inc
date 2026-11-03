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
        const auto outcome = providerResources_.fetch(request, [file](const auto& pending) {
            provider_resources::Response result;
#if defined(__ANDROID__)
            clashflux::android::ProfileHttpResponse response;
            for (unsigned attempt = 0; attempt < pending.attempts; ++attempt) {
                const auto began = std::chrono::steady_clock::now();
                response = clashflux::android::DownloadProfile(pending.url,
                    static_cast<int>(pending.timeoutSecs), pending.allowInvalidCert,
                    pending.maxBytes, true, pending.headers);
                result.networkMicros += std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - began).count();
                result.bytes += response.body.size();
                ++result.attempts;
                const bool complete = response.error.empty() &&
                    ((response.status >= 200 && response.status < 300) ||
                     (pending.acceptNotModified && response.status == 304));
                if (complete || !response.retryable || attempt + 1 == pending.attempts) break;
                std::this_thread::sleep_for(std::chrono::milliseconds{250});
            }
            result.ok = response.error.empty() &&
                ((response.status >= 200 && response.status < 300) ||
                 (pending.acceptNotModified && response.status == 304));
            if (result.ok && response.status != 304) result.body = std::move(response.body);
#else
            api::ClashApi::DownloadOptions options;
            options.timeoutSecs = pending.timeoutSecs;
            options.maxBytes = pending.maxBytes;
            options.headers = pending.headers;
            options.allowInvalidCert = pending.allowInvalidCert;
            options.allowProxyEnv = pending.allowProxyEnv;
            options.proxyUrl = pending.proxyUrl;
            options.acceptNotModified = pending.acceptNotModified;
            // Independent of the replaceable controller API/secret. Decode and
            // validate separately in each candidate, after sharing raw IO only.
            api::ClashApi downloader{"", ""};
            const auto response = downloader.downloadToFile(pending.url, file, options);
            result.ok = response.ok;
            result.networkMicros = response.download.networkMicros;
            result.bytes = response.download.bytes;
            result.connectionsKnown = response.download.transportKnown;
            result.newConnections = response.download.newConnections;
            result.attempts = 1;
            if (result.ok && response.status != 304) {
                auto body = singbox::ReadRuleProviderText(file, pending.maxBytes, result.error);
                result.ok = body.has_value();
                if (body) result.body = std::move(*body);
            }
#endif
            result.status = response.status;
            if (!response.error.empty()) result.error = response.error;
            if (!result.ok && result.error.empty()) result.error = "HTTP " + std::to_string(result.status);
            if (const auto entry = response.headers.find("etag"); entry != response.headers.end())
                result.etag = entry->second;
            if (const auto entry = response.headers.find("last-modified"); entry != response.headers.end())
                result.lastModified = entry->second;
            return result;
        }, cancel);
        stats.record(outcome);
        rule_provider_cache::FetchResult fetched;
        if (outcome.cancelled) { fetched.error = "订阅准备已取消"; return fetched; }
        const auto& response = *outcome.response;
        fetched.ok = response.ok;
        fetched.status = response.status;
        fetched.notModified = response.status == 304;
        fetched.error = response.error;
        fetched.validators = {response.etag, response.lastModified};
        bool needsCopy = outcome.joined;
#if defined(__ANDROID__)
        needsCopy = true; // The platform response has not been written to file.
#endif
        if (fetched.ok && !fetched.notModified && needsCopy) {
            // Never expose the owner's staging path to another candidate.
            std::ofstream output(file, std::ios::binary | std::ios::trunc);
            output << response.body;
            output.close();
            if (!output) { fetched.ok = false; fetched.error = "无法完成 provider 临时文件写入"; }
        }
        return fetched;
    }
