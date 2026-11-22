#include "website_test_model.h"
#include "app_http_client.h"
#if __has_include("wire_codec.h")
#include "wire_codec.h"
#else
#include "../wire_codec.h"
#endif

namespace clashflux::ui {
huxerui::Task<WebsiteHttpReply> SendWebsiteRequest(
    std::shared_ptr<AppHttpClient> http, huxerui::HttpRequest request,
    WebsiteStreamObserver observeRoute) {
    const std::string requestUrl = request.url;
    const auto started = std::chrono::steady_clock::now();
    auto received = co_await http->SendStreamAsync(std::move(request));
    const auto latency = std::chrono::ceil<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    if (!received.Succeeded()) co_return WebsiteHttpReply{.error = received.Error().code == huxerui::HttpErrorCode::Timeout
        ? clashflux::website::kTimeoutError : received.Error().message};
    auto stream = std::move(received).Value();
    WebsiteHttpReply reply{.status = stream.StatusCode(), .url = stream.Url(),
        .latencyMs = static_cast<int>(latency.count())};
    if (reply.url.empty()) reply.url = requestUrl;
    if (observeRoute) {
        // Observe the kernel's connection table while `stream` is still open
        // on this frame so pooled/short-lived connections haven't torn down yet.
        reply.outbound = co_await observeRoute(reply.url);
    }
    // Finish the empty HEAD body so the platform can retain a pooled connection.
    for (int reads = 0; reads < 2; ++reads) {
        auto chunk = co_await stream.Body().ReadAsync(1);
        if (!chunk.Succeeded() || std::move(chunk).Value().empty()) break;
    }
    co_return reply;
}

std::string WebsiteRouteLeaf(std::string_view connectionsBody,
                             const std::vector<std::string>& baseline, std::string_view host) {
    if (host.empty()) return {};
    const auto decoded = wire::DecodeConnections(connectionsBody);
    if (!decoded) return {};
    std::vector<std::string> fresh, all;
    for (const auto& entry : decoded.value.entries) {
        std::string name = entry.host;
        // Android-style host:port fallbacks keep the port in metadata.host.
        if (!entry.destinationPort.empty() && name.ends_with(":" + entry.destinationPort))
            name.resize(name.size() - entry.destinationPort.size() - 1);
        while (!name.empty() && name.back() == '.') name.pop_back();
        std::transform(name.begin(), name.end(), name.begin(),
            [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 'a' - 'A') : c; });
        if (name != host) continue;
        // sing-box reports chains leaf first; without one the route is unknown.
        const std::string leaf = !entry.chains.empty() ? entry.chains.front() : entry.chain;
        if (leaf.empty()) return {};
        all.push_back(leaf);
        if (std::find(baseline.begin(), baseline.end(), entry.id) == baseline.end()) fresh.push_back(leaf);
    }
    const auto& pool = fresh.empty() ? all : fresh;
    if (pool.empty()) return {};
    for (const auto& leaf : pool)
        if (leaf != pool.front()) return {};
    return pool.front();
}

huxerui::Task<WebsiteTestResult> ProbeWebsiteNetwork(WebsiteHttpExchange exchange, std::string url,
                                                     WebsiteRouteLocator locator) {
    const auto deadline = std::chrono::steady_clock::now() + clashflux::website::kTimeout;
    const auto remaining = [deadline] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    };
    std::optional<WebsiteRouteBaseline> baseline;
    if (locator.begin && locator.observe) {
        try { baseline = co_await locator.begin(); } catch (...) { baseline.reset(); }
    }
    constexpr auto routeBudget = std::chrono::milliseconds{1200};
    WebsiteStreamObserver observer;
    if (baseline) {
        observer = [base = *baseline, observe = locator.observe, remaining, routeBudget](std::string finalUrl)
            -> huxerui::Task<std::string> {
            if (remaining() < routeBudget) co_return std::string{};
            try { co_return co_await observe(base, std::move(finalUrl)); }
            catch (...) { co_return std::string{}; }
        };
    }
    huxerui::HttpRequest request;
    request.url = url;
    request.method = huxerui::HttpMethod::Head;
    request.timeout = remaining();
    if (*request.timeout <= std::chrono::milliseconds::zero())
        co_return WebsiteTestResult{WebsiteTestPhase::Failed, 0, clashflux::website::kTimeoutError};
    request.headers = {{"Cache-Control", "no-cache"}};
    auto site = co_await exchange(std::move(request), observer);
    if (!site.error.empty()) co_return WebsiteTestResult{WebsiteTestPhase::Failed, site.status, site.error};
    if (site.outbound.empty() && observer) {
        site.outbound = co_await observer(site.url.empty() ? std::move(url) : std::move(site.url));
    }
    if (site.outbound.empty() && !baseline && locator.isDirect && remaining() >= routeBudget) {
        try {
            if (co_await locator.isDirect()) site.outbound = "DIRECT";
        } catch (...) { /* No route evidence. */ }
    }
    if (site.status < 200 || site.status >= 400)
        co_return WebsiteTestResult{WebsiteTestPhase::Failed, site.status, "HTTP " + std::to_string(site.status),
            std::move(site.outbound)};
    co_return WebsiteTestResult{WebsiteTestPhase::Responded, site.status, {},
        std::move(site.outbound), site.latencyMs};
}
} // namespace clashflux::ui
