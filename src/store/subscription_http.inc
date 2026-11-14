// Optional HTTP representation cache. The accepted source file and row remain
// authoritative; a cache miss, damage or write failure only costs a full GET.
    static std::string remoteIdentity(const model::Profile& profile) {
        return std::to_string(profile.id) + ":" + std::to_string(profile.url.size()) + ":" + profile.url + profile.file;
    }

    static std::filesystem::path remoteCachePath(std::int64_t id) {
        return cfg::dataDir() / "subscription-http" / (std::to_string(id) + ".cache");
    }

    static std::optional<clashflux::http_cache::Representation> readRemoteCache(
            const model::Profile& profile, const std::string& accepted) {
        if (profile.file.empty() || accepted.empty() || accepted.size() > clashflux::download::kSubscriptionMaxBytes)
            return {};
        const auto path = remoteCachePath(profile.id);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path, ec)) || ec) return {};
        std::string ignored;
        const auto image = singbox::ReadRuleProviderText(path, clashflux::download::kSubscriptionMaxBytes + 36 * 1024 + 80, ignored);
        if (!image) return {};
        auto cached = clashflux::http_cache::Decode(*image, "clash-flux-subscription", remoteIdentity(profile),
                                                   clashflux::download::kSubscriptionMaxBytes);
        // Full bytes bind the validator to the accepted version, including an
        // external edit that leaves the immutable filename unchanged.
        if (!cached || cached->body != accepted) return {};
        return cached;
    }

    template<class Headers>
    static void rememberRemoteCache(const model::Profile& profile, const std::string& body,
            const Headers& headers, const clashflux::http_cache::Representation* previous = nullptr) noexcept {
        try {
            std::string etag = previous ? previous->etag : "", modified = previous ? previous->modified : "";
            if (const auto entry = headers.find("etag"); entry != headers.end()) etag = entry->second;
            if (const auto entry = headers.find("last-modified"); entry != headers.end()) modified = entry->second;
            const auto image = clashflux::http_cache::Encode("clash-flux-subscription", remoteIdentity(profile), body, etag, modified);
            if (image.empty() || body.size() > clashflux::download::kSubscriptionMaxBytes) return;
            const auto path = remoteCachePath(profile.id);
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            if (ec) return;
            if (std::filesystem::exists(std::filesystem::symlink_status(path, ec))) {
                if (ec || !std::filesystem::is_regular_file(std::filesystem::symlink_status(path, ec))) return;
                std::string ignored;
                const auto old = singbox::ReadRuleProviderText(path, clashflux::download::kSubscriptionMaxBytes + 36 * 1024 + 80, ignored);
                if (!old) return;
                const auto first = old->find('\n'), end = first == std::string::npos ? first : old->find('\n', first + 1);
                if (end == std::string::npos) return;
                const auto identity = old->substr(first + 1, end - first - 1);
                if (!identity.starts_with(std::to_string(profile.id) + ":") ||
                    !clashflux::http_cache::Decode(*old, "clash-flux-subscription", identity,
                                                  clashflux::download::kSubscriptionMaxBytes)) return;
            }
            std::string ignored;
            api::WriteFileAtomically(path, image, ignored);
        } catch (...) { /* Accepted subscriptions never depend on an optional cache write. */ }
    }
