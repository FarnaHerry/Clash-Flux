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
    const bool head = request.method == huxerui::HttpMethod::Head;
    const std::string requestUrl = request.url;
    auto received = co_await http->SendStreamAsync(std::move(request));
    if (!received.Succeeded()) co_return WebsiteHttpReply{.error = received.Error().code == huxerui::HttpErrorCode::Timeout
        ? clashflux::website::kTimeoutError : received.Error().message};
    auto stream = std::move(received).Value();
    WebsiteHttpReply reply{.status = stream.StatusCode(), .url = stream.Url()};
    if (head && reply.status >= 200 && reply.status < 400 && observeRoute) {
        // Observe the kernel's connection table while `stream` is still open
        // on this frame so pooled/short-lived connections haven't torn down yet.
        const std::string effectiveUrl = reply.url.empty() ? requestUrl : reply.url;
        try { reply.routeLeaf = co_await observeRoute(effectiveUrl); }
        catch (...) { reply.routeLeaf.clear(); }
    }
    if (head) {
        // Finish the empty HEAD body instead of destroying an unfinished
        // stream: completion lets the platform keep its connection pooled, so
        // the kernel's live connection table can also retain the route.
        for (int reads = 0; reads < 2; ++reads) {
            auto chunk = co_await stream.Body().ReadAsync(1);
            if (!chunk.Succeeded() || std::move(chunk).Value().empty()) break;
        }
        co_return reply;
    }
    if (reply.status != 200) co_return reply;
    for (;;) {
        auto chunk = co_await stream.Body().ReadAsync(4097 - reply.body.size());
        if (!chunk.Succeeded()) { reply.error = "位置响应读取失败"; co_return reply; }
        auto bytes = std::move(chunk).Value();
        if (bytes.empty()) break;
        if (bytes.size() > 4096 - reply.body.size()) { reply.error = "位置响应过大"; co_return reply; }
        reply.body.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
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

namespace {
bool IsCountryCode(const std::string& code) {
    return code.size() == 2 && code[0] >= 'A' && code[0] <= 'Z' && code[1] >= 'A' && code[1] <= 'Z' && code != "XX";
}
} // namespace

huxerui::Task<WebsiteTestResult> ProbeWebsiteNetwork(WebsiteHttpExchange exchange, std::string url,
                                                     WebsiteRouteLocator locator) {
    const auto deadline = std::chrono::steady_clock::now() + clashflux::website::kTimeout;
    const auto remaining = [&] { return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()); };
    // Correlation is optional evidence: any failure here leaves the website
    // test on the plain system-network path.
    std::optional<WebsiteRouteBaseline> baseline;
    if (locator.begin && locator.locate) {
        try { baseline = co_await locator.begin(); } catch (...) { baseline.reset(); }
    }
    huxerui::HttpRequest request;
    request.url = std::move(url); request.method = huxerui::HttpMethod::Head;
    request.timeout = remaining(); request.headers = {{"Cache-Control", "no-cache"}};
    auto site = co_await exchange(std::move(request));
    if (!site.error.empty()) co_return WebsiteTestResult{WebsiteTestPhase::Failed, site.status, site.error};
    if (site.status < 200 || site.status >= 400) co_return WebsiteTestResult{WebsiteTestPhase::Failed, site.status, "HTTP " + std::to_string(site.status)};
    if (site.routeLeaf.empty() && baseline && locator.observe) {
        try { site.routeLeaf = co_await locator.observe(*baseline, site.url); }
        catch (...) { site.routeLeaf.clear(); }
    }
    WebsiteTestResult result{WebsiteTestPhase::Responded, site.status};
    const auto origin = WebsiteOrigin(site.url);
    if (origin && remaining() >= std::chrono::milliseconds{100}) {
        // The tested origin's own trace observes this request's exit even
        // behind another VPN or system proxy.
        request = {}; request.url = *origin + "/cdn-cgi/trace"; request.timeout = remaining();
        request.headers = {{"Cache-Control", "no-cache"}};
        try {
            const auto location = co_await exchange(std::move(request));
            if (location.error.empty() && location.status == 200 && WebsiteOrigin(location.url) == origin &&
                location.body.size() <= 4096) result.country = clashflux::website::TraceCountry(location.body);
        } catch (...) { /* A location failure must not turn a reachable website into a failure. */ }
        if (!result.country.empty()) {
            result.locationSource = WebsiteLocationSource::SiteTrace;
            co_return result;
        }
    }
    // Otherwise attribute only what this application's kernel observed for
    // the final host, or a verified pure-direct desktop connection. Another
    // client's or PAC-direct traffic stays unconfirmed.
    if (result.country.empty() && locator.locate && remaining() >= std::chrono::milliseconds{300}) {
        const auto host = WebsiteHost(site.url);
        WebsiteRouteResolution resolved;
        try {
            if (!host.empty()) {
                resolved = co_await locator.locate(std::move(baseline), std::move(site.routeLeaf), host, remaining());
            }
        } catch (...) { resolved = {}; /* Correlation is optional evidence. */ }
        if (IsCountryCode(resolved.country)) {
            result.country = std::move(resolved.country);
            result.locationSource = resolved.source == WebsiteLocationSource::None
                ? WebsiteLocationSource::CoreOutbound : resolved.source;
        }
    }
    co_return result;
}
} // namespace clashflux::ui
