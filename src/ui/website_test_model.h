#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include <huxerui/huxerui.h>
#include "../website_probe.h"
#include "app_resources.h"

namespace clashflux::ui {

enum class WebsiteTestPhase { Idle, Queued, Running, Responded, Failed, Cancelled };
struct WebsiteTestResult {
    WebsiteTestPhase phase = WebsiteTestPhase::Idle;
    int status = 0;
    std::string error;
    std::string outbound;
    std::optional<int> latencyMs;
    bool operator==(const WebsiteTestResult&) const = default;
};
struct WebsiteTestSite {
    std::string name, url;
    huxerui::ImageResource icon;
    WebsiteTestResult result;
    std::size_t id = 0;
    bool operator==(const WebsiteTestSite&) const = default;
};

inline std::vector<WebsiteTestSite> FixedWebsiteTests() {
    std::vector<WebsiteTestSite> rows{
        {"Google", "https://www.google.com/generate_204", app::images::website_google},
        {"YouTube", "https://www.youtube.com/", app::images::website_youtube},
        {"TikTok", "https://www.tiktok.com/", app::images::website_tiktok},
        {"Bilibili 港澳台", "https://www.bilibili.com/bangumi/play/ss44467", app::images::website_bilibili},
        {"GitHub", "https://github.com/", app::images::github},
        {"Cloudflare", "https://www.cloudflare.com/", app::images::website_cloudflare},
        {"ChatGPT", "https://chatgpt.com/", app::images::website_chatgpt},
        {"Claude", "https://claude.ai/", app::images::website_claude},
        {"Gemini", "https://gemini.google.com/app", app::images::website_gemini},
        {"Muse AI", "https://muse.ai/", app::images::website_muse},
    };
    for (std::size_t i = 0; i < rows.size(); ++i) rows[i].id = i;
    return rows;
}

// Lowercase host of an absolute HTTP(S) URL, without port, brackets or a trailing dot.
inline std::string WebsiteHost(const std::string& url) {
    const auto parsed = huxerui::Uri::Parse(url);
    if (!parsed || !parsed->Authority()) return {};
    std::string host(*parsed->Authority());
    if (const auto at = host.rfind('@'); at != std::string::npos) host.erase(0, at + 1);
    if (host.starts_with('[')) {
        const auto end = host.find(']');
        host = end == std::string::npos ? std::string{} : host.substr(1, end - 1);
    } else if (const auto port = host.find(':'); port != std::string::npos) {
        host.resize(port);
    }
    while (!host.empty() && host.back() == '.') host.pop_back();
    std::transform(host.begin(), host.end(), host.begin(),
        [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 'a' - 'A') : c; });
    return host;
}

struct WebsiteHttpReply {
    int status = 0;
    std::string url, error, outbound;
    std::optional<int> latencyMs;
};
class AppHttpClient;
using WebsiteStreamObserver = std::function<huxerui::Task<std::string>(std::string finalUrl)>;
huxerui::Task<WebsiteHttpReply> SendWebsiteRequest(
    std::shared_ptr<AppHttpClient> http, huxerui::HttpRequest request,
    WebsiteStreamObserver observeRoute = {});
using WebsiteHttpExchange =
    std::function<huxerui::Task<WebsiteHttpReply>(huxerui::HttpRequest, WebsiteStreamObserver)>;

// Connections already known to this application's running kernel before the
// website request. identity pins the exact runtime instance/plan/listener.
struct WebsiteRouteBaseline {
    std::string identity;
    std::vector<std::string> connectionIds;
};
// The observer reads live connections while the HEAD stream is still open.
// Desktop direct attribution requires independent evidence of no interception.
struct WebsiteRouteLocator {
    std::function<huxerui::Task<std::optional<WebsiteRouteBaseline>>()> begin;
    std::function<huxerui::Task<std::string>(WebsiteRouteBaseline, std::string finalUrl)> observe;
    std::function<huxerui::Task<bool>()> isDirect;
};
huxerui::Task<WebsiteTestResult> ProbeWebsiteNetwork(WebsiteHttpExchange exchange, std::string url,
                                                     WebsiteRouteLocator locator = {});
// Pure attribution over a Clash /connections body: the single leaf outbound
// shared by connections to host. New connections (not in baseline) take
// precedence; disagreement, missing chains or no match yield an empty tag.
std::string WebsiteRouteLeaf(std::string_view connectionsBody,
                             const std::vector<std::string>& baseline, std::string_view host);

// UI-thread application service: each site owns its own request and result, at
// most three asynchronous HTTP requests run at once, no page lifetime or timer.
// Stable site IDs and request generations prevent stale results after retry.
struct WebsiteTestModel : std::enable_shared_from_this<WebsiteTestModel> {
    using Probe = std::function<huxerui::Task<WebsiteTestResult>(std::string)>;
    static constexpr std::size_t kMaxConcurrentProbes = 3;
    huxerui::State<std::vector<WebsiteTestSite>> sites{FixedWebsiteTests()};
    huxerui::TaskScope tasks;
    Probe probe;
    std::deque<std::size_t> pending;
    struct Request { std::uint64_t generation; huxerui::TaskHandle task, timeout; };
    std::unordered_map<std::size_t, Request> running;

    // True while any site is queued or running. Callers use it for aggregate
    // actions only; individual sites decide their own availability.
    [[nodiscard]] bool AnyActive() const {
        const auto& rows = sites.Get();
        return std::any_of(rows.begin(), rows.end(), [](const auto& row) {
            return row.result.phase == WebsiteTestPhase::Queued ||
                   row.result.phase == WebsiteTestPhase::Running;
        });
    }

    void CancelAll() {
        for (auto& entry : running) { entry.second.task.Cancel(); entry.second.timeout.Cancel(); }
        running.clear();
        pending.clear();
        auto rows = sites.Get();
        for (auto& row : rows)
            if (row.result.phase == WebsiteTestPhase::Queued || row.result.phase == WebsiteTestPhase::Running)
                row.result = {WebsiteTestPhase::Cancelled};
        sites = std::move(rows);
    }

    void Cancel(std::size_t id) {
        auto rows = sites.Get();
        const auto row = Find(rows, id);
        if (row == rows.end()) return;
        const auto phase = row->result.phase;
        if (phase != WebsiteTestPhase::Queued && phase != WebsiteTestPhase::Running) return;
        Stop(id);
        row->result = {WebsiteTestPhase::Cancelled};
        sites = std::move(rows);
        Pump();
    }

    // Queues the requested sites without touching any other site that is already
    // queued or running. Slots are filled in order up to the concurrency bound.
    void Start(huxerui::TaskScope taskScope, Probe onProbe, std::vector<std::size_t> ids) {
        auto rows = sites.Get();
        std::erase_if(ids, [&](auto id) {
            const auto row = Find(rows, id);
            return row == rows.end() || row->result.phase == WebsiteTestPhase::Queued ||
                   row->result.phase == WebsiteTestPhase::Running;
        });
        if (ids.empty()) return;
        tasks = std::move(taskScope);
        probe = std::move(onProbe);
        for (auto id : ids) {
            const auto row = Find(rows, id);
            if (row->result.phase == WebsiteTestPhase::Queued) continue;
            row->result = {WebsiteTestPhase::Queued};
            pending.push_back(id);
        }
        sites = std::move(rows);
        Pump();
    }

private:
    std::uint64_t nextGeneration = 0;
    static std::vector<WebsiteTestSite>::iterator Find(std::vector<WebsiteTestSite>& rows, std::size_t id) {
        return std::find_if(rows.begin(), rows.end(), [id](const auto& row) { return row.id == id; });
    }
    void Stop(std::size_t id) {
        std::erase(pending, id);
        if (const auto entry = running.find(id); entry != running.end()) {
            entry->second.task.Cancel();
            entry->second.timeout.Cancel();
            running.erase(entry);
        }
    }
    void Pump() {
        while (running.size() < kMaxConcurrentProbes && !pending.empty()) {
            const auto id = pending.front();
            pending.pop_front();
            auto rows = sites.Get();
            const auto row = Find(rows, id);
            if (row == rows.end()) continue;
            const auto url = row->url;
            row->result = {WebsiteTestPhase::Running};
            sites = std::move(rows);
            auto self = shared_from_this();
            auto probeForSite = probe;
            const auto generation = ++nextGeneration;
            running[id] = {generation, tasks.Launch([self, id, generation, url, probeForSite]() -> huxerui::Task<void> {
                WebsiteTestResult result;
                try { result = co_await probeForSite(url); }
                catch (const std::exception& error) { result = {WebsiteTestPhase::Failed, 0, error.what()}; }
                catch (...) { result = {WebsiteTestPhase::Failed}; }
                self->Complete(id, generation, std::move(result), false);
            })};
            running.at(id).timeout = tasks.Launch([self, id, generation]() -> huxerui::Task<void> {
                co_await huxerui::Delay(clashflux::website::kTimeout);
                self->Complete(id, generation,
                    {WebsiteTestPhase::Failed, 0, clashflux::website::kTimeoutError}, true);
            });
        }
    }
    void Complete(std::size_t id, std::uint64_t generation, WebsiteTestResult result, bool timedOut) {
        const auto entry = running.find(id);
        if (entry == running.end() || entry->second.generation != generation) return;
        const auto request = entry->second;
        running.erase(entry);
        auto current = sites.Get();
        const auto target = Find(current, id);
        if (target != current.end()) {
            target->result = std::move(result);
            sites = std::move(current);
        }
        // Cancel only the sibling: never destroy the coroutine completing now.
        if (timedOut) request.task.Cancel(); else request.timeout.Cancel();
        Pump();
    }
};

} // namespace clashflux::ui
