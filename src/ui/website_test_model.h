#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
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
    std::size_t id = 0;
    bool operator==(const WebsiteTestSite&) const = default;
};

inline std::vector<WebsiteTestSite> DefaultWebsiteTests() {
    std::vector<WebsiteTestSite> rows{{"Baidu", "https://www.baidu.com/"}, {"Bilibili", "https://www.bilibili.com/"},
        {"QQ", "https://www.qq.com/"}, {"JD", "https://www.jd.com/"},
        {"Google", "https://www.google.com/generate_204"}, {"YouTube", "https://www.youtube.com/"},
        {"GitHub", "https://github.com/"}, {"Cloudflare", "https://www.cloudflare.com/"}};
    for (std::size_t i = 0; i < rows.size(); ++i) rows[i].id = i;
    return rows;
}

inline void TrimWebsiteInput(std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) { text.clear(); return; }
    text = text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}

// Normalize once on submit, preserving path/query spelling and explicit HTTP.
// The same representation is used for presets, additions, edits and duplicates.
inline std::optional<std::string> NormalizeWebsiteUrl(std::string url) {
    TrimWebsiteInput(url);
    if (url.empty() || url.size() > 2048) return std::nullopt;
    if (url.starts_with("//")) url = "https:" + url;
    else {
        const auto colon = url.find(':');
        const bool explicitScheme = colon != std::string::npos && colon < url.find_first_of("/?#") &&
            url.compare(colon, 3, "://") == 0;
        if (!explicitScheme) url = "https://" + url;
    }
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 'a' - 'A') : c; };
    const auto schemeEnd = url.find(':');
    std::transform(url.begin(), url.begin() + schemeEnd, url.begin(), lower);
    const auto parsed = huxerui::Uri::Parse(url);
    if (!parsed || (parsed->Scheme() != "http" && parsed->Scheme() != "https") ||
        !parsed->Authority() || parsed->Authority()->empty() ||
        parsed->Authority()->find('@') != std::string::npos || parsed->Fragment()) return std::nullopt;
    const auto authority = *parsed->Authority();
    const auto hostEnd = authority.starts_with('[') ? authority.find(']') + 1 : authority.find(':');
    if (hostEnd == 0) return std::nullopt;
    if (hostEnd != std::string::npos && hostEnd < authority.size()) {
        const auto port = authority.substr(hostEnd + 1);
        unsigned number = 0;
        if (port.empty()) return std::nullopt;
        for (const char digit : port) {
            if (digit < '0' || digit > '9' || number > 65535) return std::nullopt;
            number = number * 10 + static_cast<unsigned>(digit - '0');
        }
        if (number == 0 || number > 65535) return std::nullopt;
    }
    const auto authorityBegin = schemeEnd + 3;
    std::transform(url.begin() + authorityBegin, url.begin() + authorityBegin + authority.size(),
        url.begin() + authorityBegin, lower);
    if (parsed->Path().empty()) url.insert(authorityBegin + authority.size(), "/");
    if (url.size() > 2048) return std::nullopt;
    return url;
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

// UI-thread application service: each site owns its own request and result, at
// most three asynchronous HTTP requests run at once, no page lifetime or timer.
// Stable IDs and request generations keep removals/edits from retargeting results.
struct WebsiteTestModel : std::enable_shared_from_this<WebsiteTestModel> {
    using Probe = std::function<huxerui::Task<WebsiteTestResult>(std::string)>;
    static constexpr std::size_t kMaxConcurrentProbes = 3;
    huxerui::State<std::vector<WebsiteTestSite>> sites{DefaultWebsiteTests()};
    huxerui::State<huxerui::TextEditingValue> customUrl{huxerui::TextEditingValue{}};
    huxerui::State<huxerui::TextEditingValue> customName{huxerui::TextEditingValue{}};
    huxerui::TaskScope tasks;
    Probe probe;
    std::deque<std::size_t> pending;
    struct Request { std::uint64_t generation; huxerui::TaskHandle task; };
    std::unordered_map<std::size_t, Request> running;

    // `name` is the optional remark; an empty remark falls back to the URL host
    // so a card always has a readable title.
    bool Add(std::string url, std::string name = {}) {
        if (sites.Get().size() >= 24) return false;
        const auto normalized = NormalizeWebsiteUrl(std::move(url));
        if (!normalized) return false;
        auto rows = sites.Get();
        if (Duplicate(rows, *normalized, std::nullopt)) return false;
        TrimWebsiteInput(name);
        if (name.empty()) name = std::string(*huxerui::Uri::Parse(*normalized)->Authority());
        rows.push_back({std::move(name), *normalized, {}, nextId++});
        sites = std::move(rows);
        customUrl = huxerui::TextEditingValue{};
        customName = huxerui::TextEditingValue{};
        return true;
    }

    bool Update(std::size_t id, std::string url, std::string name = {}) {
        const auto normalized = NormalizeWebsiteUrl(std::move(url));
        if (!normalized) return false;
        auto rows = sites.Get();
        const auto row = Find(rows, id);
        if (row == rows.end() || Duplicate(rows, *normalized, id)) return false;
        TrimWebsiteInput(name);
        if (name.empty()) name = std::string(*huxerui::Uri::Parse(*normalized)->Authority());
        if (row->url != *normalized) {
            Stop(id);
            row->url = *normalized;
            row->result = {};
        }
        row->name = std::move(name);
        sites = std::move(rows);
        customUrl = huxerui::TextEditingValue{};
        customName = huxerui::TextEditingValue{};
        Pump();
        return true;
    }

    bool Remove(std::size_t id) {
        auto rows = sites.Get();
        const auto row = Find(rows, id);
        if (row == rows.end()) return false;
        Stop(id);
        rows.erase(row);
        sites = std::move(rows);
        Pump();
        return true;
    }

    void Toggle(huxerui::TaskScope taskScope, Probe onProbe, std::size_t id) {
        const auto& rows = sites.Get();
        const auto row = std::find_if(rows.begin(), rows.end(), [id](const auto& site) { return site.id == id; });
        if (row == rows.end()) return;
        if (row->result.phase == WebsiteTestPhase::Queued || row->result.phase == WebsiteTestPhase::Running) Cancel(id);
        else Start(std::move(taskScope), std::move(onProbe), {id});
    }

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
        for (auto& entry : running) entry.second.task.Cancel();
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
    std::size_t nextId = sites.Get().size();
    std::uint64_t nextGeneration = 0;
    static std::vector<WebsiteTestSite>::iterator Find(std::vector<WebsiteTestSite>& rows, std::size_t id) {
        return std::find_if(rows.begin(), rows.end(), [id](const auto& row) { return row.id == id; });
    }
    static bool Duplicate(const std::vector<WebsiteTestSite>& rows, const std::string& url,
        std::optional<std::size_t> except) {
        return std::any_of(rows.begin(), rows.end(), [&](const auto& row) {
            return row.id != except && NormalizeWebsiteUrl(row.url) == url;
        });
    }
    void Stop(std::size_t id) {
        std::erase(pending, id);
        if (const auto entry = running.find(id); entry != running.end()) {
            entry->second.task.Cancel();
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
                catch (const std::exception& error) { result = {WebsiteTestPhase::Failed, 0, 0, error.what()}; }
                catch (...) { result = {WebsiteTestPhase::Failed}; }
                const auto entry = self->running.find(id);
                if (entry == self->running.end() || entry->second.generation != generation) co_return;
                auto current = self->sites.Get();
                const auto target = Find(current, id);
                if (target != current.end() && target->url == url) {
                    target->result = std::move(result);
                    self->sites = std::move(current);
                }
                self->running.erase(entry);
                self->Pump();
            })};
        }
    }
};

} // namespace clashflux::ui
