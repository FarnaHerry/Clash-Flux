#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <huxerui/huxerui.h>
#include "app_http_client.h"

namespace clashflux::ui {

enum class WebsiteTestPhase { Idle, Queued, Running, Responded, Failed, Cancelled };
struct WebsiteTestResult {
    WebsiteTestPhase phase = WebsiteTestPhase::Idle;
    int status = 0;
    std::int64_t elapsedMs = 0;
    std::string error;
    bool operator==(const WebsiteTestResult&) const = default;
};
struct WebsiteTestSite {
    std::string name, url;
    WebsiteTestResult result;
    bool operator==(const WebsiteTestSite&) const = default;
};

inline std::vector<WebsiteTestSite> DefaultWebsiteTests() {
    return {{"Baidu", "https://www.baidu.com/"}, {"Bilibili", "https://www.bilibili.com/"},
        {"QQ", "https://www.qq.com/"}, {"JD", "https://www.jd.com/"},
        {"Google", "https://www.google.com/generate_204"}, {"YouTube", "https://www.youtube.com/"},
        {"GitHub", "https://github.com/"}, {"Cloudflare", "https://www.cloudflare.com/"}};
}

inline huxerui::Task<WebsiteTestResult> ProbeWebsite(
    std::shared_ptr<AppHttpClient> http, std::string url) {
    WebsiteTestResult result;
    const auto began = std::chrono::steady_clock::now();
    try {
        huxerui::HttpRequest request;
        request.url = std::move(url);
        request.method = huxerui::HttpMethod::Head;
        request.timeout = std::chrono::seconds{10};
        request.headers = {{"Cache-Control", "no-cache"}};
        auto response = co_await http->SendAsync(std::move(request));
        if (response.Succeeded()) {
            result.phase = WebsiteTestPhase::Responded;
            result.status = response.Value().status_code;
        } else {
            result.phase = WebsiteTestPhase::Failed;
            result.error = response.Error().message;
        }
    } catch (const std::exception& error) {
        result.phase = WebsiteTestPhase::Failed;
        result.error = error.what();
    } catch (...) {
        result.phase = WebsiteTestPhase::Failed;
    }
    result.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - began).count();
    co_return result;
}

// UI-thread application service: one batch, at most three asynchronous HTTP
// requests, no page lifetime or timer. Every publication checks its generation.
struct WebsiteTestModel : std::enable_shared_from_this<WebsiteTestModel> {
    using Probe = std::function<huxerui::Task<WebsiteTestResult>(std::string)>;
    huxerui::State<std::vector<WebsiteTestSite>> sites{DefaultWebsiteTests()};
    huxerui::State<bool> busy{false};
    huxerui::State<huxerui::TextEditingValue> customUrl{huxerui::TextEditingValue{}};
    std::uint64_t generation = 0;
    std::vector<huxerui::TaskHandle> handles;

    bool Add(std::string url) {
        if (busy.Get() || sites.Get().size() >= 24) return false;
        const auto begin = url.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos) return false;
        url = url.substr(begin, url.find_last_not_of(" \t\r\n") - begin + 1);
        const auto parsed = huxerui::Uri::Parse(url);
        if (!parsed || (parsed->Scheme() != "http" && parsed->Scheme() != "https") ||
            !parsed->Authority() || parsed->Authority()->empty() || parsed->Authority()->find('@') != std::string::npos ||
            parsed->Fragment() || url.size() > 2048) return false;
        auto rows = sites.Get();
        if (std::any_of(rows.begin(), rows.end(), [&](const auto& row) { return row.url == url; })) return false;
        rows.push_back({std::string(*parsed->Authority()), std::move(url)});
        sites = std::move(rows);
        customUrl = huxerui::TextEditingValue{};
        return true;
    }

    void Cancel() {
        ++generation;
        for (const auto& handle : handles) handle.Cancel();
        handles.clear();
        auto rows = sites.Get();
        for (auto& row : rows)
            if (row.result.phase == WebsiteTestPhase::Queued || row.result.phase == WebsiteTestPhase::Running)
                row.result = {WebsiteTestPhase::Cancelled};
        sites = std::move(rows);
        busy = false;
    }

    void Start(huxerui::TaskScope tasks, Probe probe, std::vector<std::size_t> indices) {
        if (busy.Get() || indices.empty()) return;
        auto rows = sites.Get();
        std::erase_if(indices, [&](auto index) { return index >= rows.size(); });
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        if (indices.empty()) return;
        for (auto index : indices) rows[index].result = {WebsiteTestPhase::Queued};
        sites = std::move(rows);
        busy = true;
        handles.clear();
        const auto ticket = ++generation;
        struct Batch { std::vector<std::size_t> indices; std::size_t next = 0, remaining = 0; };
        auto batch = std::make_shared<Batch>(Batch{indices, 0, indices.size()});
        auto self = shared_from_this();
        for (std::size_t runner = 0; runner < std::min<std::size_t>(3, indices.size()); ++runner) {
            handles.push_back(tasks.Launch([self, batch, probe, ticket]() -> huxerui::Task<void> {
                while (self->generation == ticket && batch->next < batch->indices.size()) {
                    const auto index = batch->indices[batch->next++];
                    auto current = self->sites.Get();
                    const auto url = current[index].url;
                    current[index].result = {WebsiteTestPhase::Running};
                    self->sites = std::move(current);
                    WebsiteTestResult result;
                    try { result = co_await probe(url); }
                    catch (const std::exception& error) { result = {WebsiteTestPhase::Failed, 0, 0, error.what()}; }
                    catch (...) { result = {WebsiteTestPhase::Failed}; }
                    if (self->generation != ticket) co_return;
                    current = self->sites.Get();
                    current[index].result = std::move(result);
                    self->sites = std::move(current);
                    if (--batch->remaining == 0) self->busy = false;
                }
            }));
        }
    }
};

} // namespace clashflux::ui
