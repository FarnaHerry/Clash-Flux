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

export struct Operations {
    std::function<bool(const singbox::HttpRuleProviderResource&, const std::filesystem::path&,
                       std::string&)> fetch;
    std::function<bool(const singbox::CompileOptions&, const singbox::CompileResult&,
                       std::string&)> check;
    std::function<bool(const std::filesystem::path&, const std::filesystem::path&,
                       std::string&)> commit;
};

export inline bool Prepare(singbox::CompileOptions& options, const Operations& operations,
        singbox::CompileResult& result, std::string& error) {
    error.clear();
    try {
        result = singbox::compileConfig(options);
        if (result.httpRuleProviders.empty()) { error = result.error; return error.empty(); }
        if (options.ruleProviderCacheDir.empty() || !operations.fetch || !operations.check || !operations.commit) {
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
            bool snapshotsAdded = false;
            for (const auto& resource : resources) {
                const auto [position, inserted] = identities.emplace(resource.cacheKey, resource.identity);
                if (position->second != resource.identity || identities.size() > 256) {
                    error = "HTTP/ASN 缓存身份冲突或依赖过多"; return false;
                }
                if (candidate.ruleProviderContents.contains(resource.cacheKey)) continue; // pinned rollback/start snapshot
                std::string reason;
                const auto destination = root / (resource.cacheKey + ".cache");
                auto content = singbox::ReadHttpRuleProviderCache(resource, root, reason);
                if (reason == "HTTP 规则集缓存身份或版本不匹配") { error = reason; return false; }
                bool usable = content && singbox::ValidateHttpRuleProvider(resource, *content, reason);
                bool changed = false;
                if (!usable && !resource.seedPath.empty()) {
                    content = singbox::ReadHttpRuleProviderSeed(resource, options.ruleProviderDir, reason);
                    if (reason.find("越过文件根目录") != std::string::npos) { error = reason; return false; }
                    usable = content && singbox::ValidateHttpRuleProvider(resource, *content, reason);
                    changed = usable;
                }
                bool stale = !usable;
                if (usable && resource.intervalSeconds != 0) {
                    const auto stamp = std::filesystem::last_write_time(destination, ec);
                    stale = ec || decltype(stamp)::clock::now() - stamp >= std::chrono::seconds(resource.intervalSeconds);
                }
                if (stale) {
                    const auto payload = stage.path / (resource.cacheKey + ".download");
                    bool fetched = operations.fetch(resource, payload, reason);
                    auto downloaded = fetched ? singbox::ReadRuleProviderText(payload, resource.maxBytes, reason)
                                              : std::nullopt;
                    if (downloaded && singbox::ValidateHttpRuleProvider(resource, *downloaded, reason)) {
                        content = std::move(downloaded); usable = true; changed = true;
                    } else if (usable) {
                        warnings.push_back("HTTP 规则集「" + resource.name + "」刷新失败，沿用已验证缓存/种子：" + reason);
                    } else {
                        error = "HTTP 规则集「" + resource.name + "」准备失败：" + reason;
                        return false;
                    }
                }
                candidate.ruleProviderContents[resource.cacheKey] = *content;
                snapshotsAdded = true;
                if (changed) {
                    const auto image = singbox::HttpRuleProviderCacheImage(resource, *content);
                    if (image.empty()) { error = "HTTP 缓存身份或内容无效"; return false; }
                    const auto file = stage.path / (resource.cacheKey + ".cache");
                    std::ofstream output(file, std::ios::binary | std::ios::trunc);
                    output << image;
                    output.close();
                    if (!output) { error = "无法完成 HTTP 缓存临时文件写入"; return false; }
                    pending.emplace_back(file, destination);
                }
            }
            // The first compile already consumed every pinned snapshot. A
            // restart/rollback must not decode the identical inputs twice.
            if (snapshotsAdded) result = singbox::compileConfig(candidate);
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
