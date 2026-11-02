export module clashflux.proxy_provider_cache;
import std;
import clashflux.singbox;

namespace proxy_provider_cache {

export struct Operations {
    unsigned maxConcurrentFetches = 1;
    std::function<bool(const singbox::HttpProxyProviderResource&,
                       const std::filesystem::path&, std::string&)> fetch;
};

export struct Preparation {
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> pending;
    std::vector<std::string> warnings;
    unsigned compileCount = 0;
    std::int64_t compileMicros = 0;
};

export inline bool Prepare(singbox::CompileOptions& candidate, const Operations& operations,
        singbox::CompileResult& result, Preparation& preparation,
        const std::filesystem::path& stage, std::string& error,
        std::stop_token cancel = {}) {
    error.clear();
    preparation = {};
    try {
        std::vector<const singbox::HttpProxyProviderResource*> resources;
        std::map<std::string, std::string> identities;
        for (const auto& resource : result.httpProxyProviders) {
            if (!resource.required) continue;
            const auto [position, inserted] = identities.emplace(resource.cacheKey, resource.identity);
            if (position->second != resource.identity || identities.size() > 64) {
                error = "HTTP 代理集合缓存身份冲突或数量过多";
                return false;
            }
            resources.push_back(&resource);
        }
        if (resources.empty()) return true;
        if (candidate.proxyProviderCacheDir.empty() || !operations.fetch || stage.empty()) {
            error = "HTTP 代理集合任务缺少缓存目录、暂存目录或下载接口";
            return false;
        }
        const std::filesystem::path root(candidate.proxyProviderCacheDir);
        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        if (ec) {
            error = "无法创建 HTTP 代理集合缓存目录";
            return false;
        }
        struct Item {
            std::string content, error, warning;
            bool changed = false;
            bool inputChanged = false;
        };
        std::vector<Item> items(resources.size());
        std::vector<std::size_t> jobs(resources.size());
        for (std::size_t index = 0; index < jobs.size(); ++index) jobs[index] = index;
        const auto prepare = [&](std::size_t index) {
            auto& item = items[index];
            const auto& resource = *resources[index];
            try {
                if (cancel.stop_requested()) {
                    item.error = "订阅准备已取消";
                    return;
                }
                const auto consumed = result.proxyProviderContents.find(resource.cacheKey);
                const bool alreadyCompiled = consumed != result.proxyProviderContents.end();
                std::optional<std::string> content;
                if (alreadyCompiled) content = consumed->second;
                std::string reason;
                if (!content) content = singbox::ReadHttpProxyProviderCache(resource, root, reason);
                bool usable = content && singbox::ValidateHttpProxyProvider(resource, *content, reason);
                bool stale = !usable || candidate.forceProxyProviderRefresh;
                if (usable && !stale && resource.intervalSeconds != 0) {
                    const auto destination = root / (resource.cacheKey + ".cache");
                    std::error_code stampError;
                    const auto stamp = std::filesystem::last_write_time(destination, stampError);
                    stale = stampError || decltype(stamp)::clock::now() - stamp >=
                        std::chrono::seconds(resource.intervalSeconds);
                }
                if (stale) {
                    const auto payload = stage / (resource.cacheKey + ".proxy-download");
                    reason.clear();
                    const bool fetched = operations.fetch(resource, payload, reason);
                    auto downloaded = fetched
                        ? singbox::ReadRuleProviderText(payload, resource.maxBytes, reason)
                        : std::nullopt;
                    if (downloaded && singbox::ValidateHttpProxyProvider(resource, *downloaded, reason)) {
                        item.inputChanged = !alreadyCompiled || *downloaded != *content;
                        content = std::move(downloaded);
                        usable = true;
                        item.changed = true;
                    } else if (usable && !candidate.forceProxyProviderRefresh) {
                        item.warning = "HTTP 代理集合「" + resource.name +
                            "」到期刷新失败，沿用已验证缓存：" + reason;
                    } else {
                        item.error = "HTTP 代理集合「" + resource.name + "」准备失败：" + reason;
                        return;
                    }
                }
                if (!usable) {
                    item.error = "HTTP 代理集合「" + resource.name + "」没有有效内容";
                    return;
                }
                item.content = std::move(*content);
            } catch (const std::exception& exception) {
                item.error = std::string("HTTP 代理集合准备异常：") + exception.what();
            } catch (...) {
                item.error = "HTTP 代理集合准备异常";
            }
        };
        const auto concurrency = std::min<std::size_t>({jobs.size(), 4,
            std::max(1u, operations.maxConcurrentFetches)});
        if (concurrency <= 1) {
            for (const auto index : jobs) prepare(index);
        } else {
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
        if (cancel.stop_requested()) {
            error = "订阅准备已取消";
            return false;
        }
        bool inputsChanged = false;
        for (std::size_t index = 0; index < resources.size(); ++index) {
            const auto& resource = *resources[index];
            auto& item = items[index];
            if (!item.error.empty()) {
                error = std::move(item.error);
                return false;
            }
            if (!item.warning.empty()) preparation.warnings.push_back(std::move(item.warning));
            if (item.changed) {
                const auto image = singbox::HttpProxyProviderCacheImage(resource, item.content);
                if (image.empty()) {
                    error = "HTTP 代理集合缓存身份或内容无效";
                    return false;
                }
                const auto file = stage / (resource.cacheKey + ".proxy-cache");
                std::ofstream output(file, std::ios::binary | std::ios::trunc);
                output << image;
                output.close();
                if (!output) {
                    error = "无法完成 HTTP 代理集合缓存临时文件写入";
                    return false;
                }
                preparation.pending.emplace_back(file, root / (resource.cacheKey + ".cache"));
            }
            candidate.proxyProviderContents[resource.cacheKey] = std::move(item.content);
            inputsChanged = inputsChanged || item.inputChanged;
        }
        if (inputsChanged) {
            const auto began = std::chrono::steady_clock::now();
            result = singbox::compileConfig(candidate, cancel);
            preparation.compileMicros += std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - began).count();
            ++preparation.compileCount;
        }
        if (cancel.stop_requested()) {
            error = "订阅准备已取消";
            return false;
        }
        for (const auto& resource : result.httpProxyProviders) {
            if (resource.required && !candidate.proxyProviderContents.contains(resource.cacheKey)) {
                error = "HTTP 代理集合「" + resource.name + "」缺少已提交快照";
                return false;
            }
        }
        result.proxyProviderContents.clear();
        return true;
    } catch (const std::exception& exception) {
        error = std::string("HTTP 代理集合准备异常：") + exception.what();
        return false;
    } catch (...) {
        error = "HTTP 代理集合准备异常";
        return false;
    }
}

} // namespace proxy_provider_cache
