#include "website_test_model.h"
#include "app_http_client.h"

namespace clashflux::ui {
huxerui::Task<WebsiteHttpReply> SendWebsiteRequest(std::shared_ptr<AppHttpClient> http, huxerui::HttpRequest request) {
    const bool head = request.method == huxerui::HttpMethod::Head;
    auto received = co_await http->SendStreamAsync(std::move(request));
    if (!received.Succeeded()) co_return WebsiteHttpReply{.error = received.Error().code == huxerui::HttpErrorCode::Timeout
        ? clashflux::website::kTimeoutError : received.Error().message};
    auto stream = std::move(received).Value();
    WebsiteHttpReply reply{.status = stream.StatusCode(), .url = stream.Url()};
    if (head || reply.status != 200) co_return reply;
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

huxerui::Task<WebsiteTestResult> ProbeWebsiteNetwork(WebsiteHttpExchange exchange, std::string url) {
    const auto deadline = std::chrono::steady_clock::now() + clashflux::website::kTimeout;
    const auto remaining = [&] { return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()); };
    huxerui::HttpRequest request;
    request.url = std::move(url); request.method = huxerui::HttpMethod::Head;
    request.timeout = remaining(); request.headers = {{"Cache-Control", "no-cache"}};
    const auto site = co_await exchange(std::move(request));
    if (!site.error.empty()) co_return WebsiteTestResult{WebsiteTestPhase::Failed, site.status, site.error};
    if (site.status < 200 || site.status >= 400) co_return WebsiteTestResult{WebsiteTestPhase::Failed, site.status, "HTTP " + std::to_string(site.status)};
    WebsiteTestResult result{WebsiteTestPhase::Responded, site.status};
    const auto origin = WebsiteOrigin(site.url);
    if (!origin || remaining() < std::chrono::milliseconds{100}) co_return result;
    // Query only the tested origin. A different service's exit cannot prove
    // how this website was routed by another VPN or a system proxy's rules.
    request = {}; request.url = *origin + "/cdn-cgi/trace"; request.timeout = remaining();
    request.headers = {{"Cache-Control", "no-cache"}};
    try {
        const auto location = co_await exchange(std::move(request));
        if (location.error.empty() && location.status == 200 && WebsiteOrigin(location.url) == origin &&
            location.body.size() <= 4096) result.country = clashflux::website::TraceCountry(location.body);
    } catch (...) { /* A location failure must not turn a reachable website into a failure. */ }
    co_return result;
}
} // namespace clashflux::ui
