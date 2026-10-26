// Task-layer HTTP preparation. The compiler never calls fetch/commit; candidate
// snapshots remain immutable and no provider/user file is deleted or repaired.
export module clashflux.rule_provider_cache;
import std;
import clashflux.singbox;

namespace rule_provider_cache {

export struct StagingDirectory {
    std::filesystem::path path;
    StagingDirectory() = default;
    StagingDirectory(const StagingDirectory&) = delete;
    StagingDirectory& operator=(const StagingDirectory&) = delete;
    ~StagingDirectory() {
        if (!path.empty()) { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    }
};

export struct FetchResult {
    bool ok = false, notModified = false;
    long status = 0;
    singbox::HttpCacheValidators validators;
    std::string error;
};

export struct Operations {
    // Callbacks are invoked concurrently only when explicitly opted in. All
    // workers finish before check/commit or staging-directory destruction.
    unsigned maxConcurrentFetches = 1;
    std::function<FetchResult(const singbox::HttpRuleProviderResource&, const std::filesystem::path&,
                             const singbox::HttpCacheValidators&)> revalidate;
    std::function<bool(const singbox::HttpRuleProviderResource&, const std::filesystem::path&,
                       std::string&)> fetch;
    std::function<bool(const singbox::CompileOptions&, const singbox::CompileResult&,
                       std::string&)> check;
    std::function<bool(const std::filesystem::path&, const std::filesystem::path&,
                       std::string&)> commit;
};

export struct PreparationStats {
    unsigned compileCount = 0;
    std::int64_t compileMicros = 0;
    unsigned validatedSnapshotReuses = 0;
    unsigned conversionCacheHits = 0;
};

export inline bool Prepare(singbox::CompileOptions& options, const Operations& operations,
        singbox::CompileResult& result, std::string& error, PreparationStats* stats = nullptr) {
    error.clear();
    if (stats) *stats = {};
    const auto compile = [&](const singbox::CompileOptions& candidate) {
        const auto began = stats ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const auto record = [&] {
            if (stats) stats->compileMicros += std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - began).count();
        };
        if (stats) ++stats->compileCount;
        try {
            auto compiled = singbox::compileConfig(candidate);
            if (stats) stats->conversionCacheHits += compiled.ruleProviderConversionCacheHits;
            record();
            return compiled;
        } catch (...) {
            record();
            throw;
        }
    };
    try {
        result = compile(options);
        if (result.httpRuleProviders.empty()) { error = result.error; return error.empty(); }
        if (options.ruleProviderCacheDir.empty() || (!operations.fetch && !operations.revalidate) ||
                !operations.check || !operations.commit) {
            error = "HTTP 规则集任务缺少缓存目录或操作接口"; return false;
        }
        const std::filesystem::path root(options.ruleProviderCacheDir);
        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        if (ec) { error = "无法创建 HTTP 规则集缓存目录"; return false; }
        StagingDirectory stage;
        std::random_device random;
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            const auto candidate = root / (".stage-" + std::to_string(random()) + "-" + std::to_string(random()));
            if (std::filesystem::create_directory(candidate, ec)) { stage.path = candidate; break; }
        }
        if (stage.path.empty()) { error = "无法创建独占 HTTP 规则集临时目录"; return false; }
        // Caller options are published only after complete conversion/check/commit.
        auto candidate = options;
        std::vector<std::pair<std::filesystem::path, std::filesystem::path>> pending;
        std::vector<std::string> warnings;
        // A classical body can reveal ASN dependencies only after its download.
        // Discover in bounded rounds, holding every replacement until full check.
        std::map<std::string, std::string> identities;
        for (unsigned round = 0; ; ++round) {
            if (round >= 256) { error = "HTTP/ASN 依赖发现超过轮数限制"; return false; }
            const auto resources = result.httpRuleProviders;
            bool inputsChanged = false;
            struct Prepared {
                std::optional<std::string> content;
                std::string error, warning;
                singbox::HttpCacheValidators validators;
                bool changed = false, inputChanged = false, reused = false;
            };
            std::vector<std::size_t> jobs;
            std::set<std::string> scheduled;
            std::vector<Prepared> prepared(resources.size());
            // Validate/deduplicate identities before starting any network IO.
            for (std::size_t i = 0; i < resources.size(); ++i) {
                const auto& resource = resources[i];
                const auto [position, inserted] = identities.emplace(resource.cacheKey, resource.identity);
                if (position->second != resource.identity || identities.size() > 256) {
                    error = "HTTP/ASN 缓存身份冲突或依赖过多"; return false;
                }
                if (!candidate.ruleProviderContents.contains(resource.cacheKey) && scheduled.insert(resource.cacheKey).second)
                    jobs.push_back(i);
            }
            const auto prepare = [&](std::size_t index) {
              auto& item = prepared[index];
              const auto& resource = resources[index];
              try {
                std::string reason;
                const auto destination = root / (resource.cacheKey + ".cache");
                const auto consumed = result.ruleProviderContents.find(resource.cacheKey);
                const bool alreadyCompiled = consumed != result.ruleProviderContents.end();
                singbox::HttpCacheValidators validators;
                auto content = alreadyCompiled ? std::optional<std::string>{std::move(consumed->second)}
                    : singbox::ReadHttpRuleProviderCache(resource, root, reason, &validators);
                if (reason == "HTTP 规则集缓存身份或版本不匹配") { item.error = reason; return; }
                bool usable = alreadyCompiled || (content && singbox::ValidateHttpRuleProvider(resource, *content, reason));
                bool inputChanged = !alreadyCompiled;
                item.reused = alreadyCompiled;
                bool changed = false;
                if (!usable && !resource.seedPath.empty()) {
                    content = singbox::ReadHttpRuleProviderSeed(resource, options.ruleProviderDir, reason);
                    if (reason.find("越过文件根目录") != std::string::npos) { item.error = reason; return; }
                    usable = content && singbox::ValidateHttpRuleProvider(resource, *content, reason);
                    changed = usable;
                }
                bool stale = !usable;
                if (usable && resource.intervalSeconds != 0) {
                    std::error_code stampError;
                    const auto stamp = std::filesystem::last_write_time(destination, stampError);
                    stale = stampError || decltype(stamp)::clock::now() - stamp >= std::chrono::seconds(resource.intervalSeconds);
                }
                if (stale) {
                    const auto payload = stage.path / (resource.cacheKey + ".download");
                    if (operations.revalidate && alreadyCompiled) {
                        // Bind validators to the exact verified body, including
                        // a cache file changed between compilation and refresh.
                        std::string ignored;
                        const auto disk = singbox::ReadHttpRuleProviderCache(resource, root, ignored, &validators);
                        if (!disk || *disk != *content) validators = {};
                    }
                    if (!usable || resource.headers.contains("if-none-match") ||
                        resource.headers.contains("if-modified-since")) validators = {};
                    FetchResult response;
                    if (operations.revalidate) response = operations.revalidate(resource, payload, validators);
                    else { response.ok = operations.fetch(resource, payload, reason); response.error = reason; }
                    reason = response.error;
                    if (!singbox::ValidHttpCacheValidators(response.validators)) response.validators = {};
                    const bool verified304 = response.ok && response.notModified && usable &&
                        (!validators.etag.empty() || !validators.lastModified.empty());
                    if (response.notModified && !verified304 && reason.empty())
                        reason = "HTTP 304 没有对应的已验证缓存";
                    bool fetched = response.ok && !response.notModified;
                    auto downloaded = fetched ? singbox::ReadRuleProviderText(payload, resource.maxBytes, reason)
                                              : std::nullopt;
                    if (verified304) {
                        changed = true;
                        if (!response.validators.etag.empty()) validators.etag = response.validators.etag;
                        if (!response.validators.lastModified.empty()) validators.lastModified = response.validators.lastModified;
                    } else if (downloaded && singbox::ValidateHttpRuleProvider(resource, *downloaded, reason)) {
                        inputChanged = !alreadyCompiled || *downloaded != *content;
                        content = std::move(downloaded); usable = true; changed = true;
                        validators = std::move(response.validators);
                    } else if (usable) {
                        item.warning = "HTTP 规则集「" + resource.name + "」刷新失败，沿用已验证缓存/种子：" + reason;
                    } else {
                        item.error = "HTTP 规则集「" + resource.name + "」准备失败：" + reason;
                        return;
                    }
                }
                item.validators = std::move(validators);
                item.content = std::move(content); item.changed = changed; item.inputChanged = inputChanged;
              } catch (const std::exception& exception) {
                item.error = std::string("HTTP 规则集准备异常：") + exception.what();
              } catch (...) { item.error = "HTTP 规则集准备异常"; }
            };
            const auto concurrency = std::min<std::size_t>({jobs.size(), 4,
                std::max(1u, operations.maxConcurrentFetches)});
            if (concurrency <= 1) { for (const auto index : jobs) prepare(index); }
            else {
                std::atomic_size_t next = 0;
                std::vector<std::jthread> workers;
                for (std::size_t i = 0; i < concurrency; ++i) workers.emplace_back([&] {
                    for (;;) {
                        const auto offset = next.fetch_add(1, std::memory_order_relaxed);
                        if (offset >= jobs.size()) break;
                        prepare(jobs[offset]);
                    }
                });
            }
            // Stable declaration order for diagnostics, snapshots and commits.
            // No structural mutation of candidate/result occurs in workers.
            for (const auto index : jobs) {
                const auto& resource = resources[index];
                auto& item = prepared[index];
                if (!item.error.empty()) { error = std::move(item.error); return false; }
                if (!item.warning.empty()) warnings.push_back(std::move(item.warning));
                if (item.reused && stats) ++stats->validatedSnapshotReuses;
                const auto destination = root / (resource.cacheKey + ".cache");
                if (item.changed) {
                    const auto image = singbox::HttpRuleProviderCacheImage(resource, *item.content, item.validators);
                    if (image.empty()) { error = "HTTP 缓存身份或内容无效"; return false; }
                    const auto file = stage.path / (resource.cacheKey + ".cache");
                    std::ofstream output(file, std::ios::binary | std::ios::trunc);
                    output << image;
                    output.close();
                    if (!output) { error = "无法完成 HTTP 缓存临时文件写入"; return false; }
                    pending.emplace_back(file, destination);
                }
                candidate.ruleProviderContents[resource.cacheKey] = std::move(*item.content);
                inputsChanged = inputsChanged || item.inputChanged;
            }
            // The first compile may already have consumed fresh disk caches.
            // Only new or byte-different snapshots require another compile.
            if (inputsChanged) result = compile(candidate);
            // Newly discovered dependencies must verify full identity even
            // when a colliding filename already has a prepared snapshot.
            for (const auto& resource : result.httpRuleProviders) {
                const auto [position, inserted] = identities.emplace(resource.cacheKey, resource.identity);
                if (position->second != resource.identity || identities.size() > 256) {
                    error = "HTTP/ASN 缓存身份冲突或依赖过多"; return false;
                }
            }
            if (std::ranges::any_of(result.httpRuleProviders, [&](const auto& resource) {
                  return !candidate.ruleProviderContents.contains(resource.cacheKey);
                })) continue;
            break;
        }
        result.ruleProviderContents.clear(); // Options own the published/rollback snapshots.
        if (!result.error.empty() || result.json.empty()) { error = result.error; return false; }
        if (!operations.check(candidate, result, error)) {
            if (error.empty()) error = "HTTP 候选配置检查失败";
            return false;
        }
        // Each replace is atomic. No cross-file crash transaction is claimed.
        for (const auto& [file, destination] : pending)
            if (!operations.commit(file, destination, error)) {
                if (error.empty()) error = "HTTP 缓存提交失败";
                return false;
            }
        result.warnings.insert(result.warnings.end(), warnings.begin(), warnings.end());
        options = std::move(candidate);
        return true;
    } catch (const std::exception& exception) {
        error = std::string("HTTP 规则集准备异常：") + exception.what(); return false;
    } catch (...) {
        error = "HTTP 规则集准备异常"; return false;
    }
}

} // namespace rule_provider_cache
