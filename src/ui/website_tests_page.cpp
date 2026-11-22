#include "app_http_client.h"
#include <huxerui/huxerui.h>
#include <algorithm>
#include <format>
#include <optional>
#include <utility>
#include <vector>
#include "app_resources.h"
#include "ui.h"
#include "page_layout.h"
#include "proxy_display_name.h"
#include "task_bridge.h"
#include "website_test_card.h"
#include "website_test_grid.h"
#include "wire_codec.h"

#if !defined(__ANDROID__) && !defined(CLASHFLUX_IOS)
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wininet.h>
#endif
#include <cstdlib>
#include "network_conflict.h"
#endif

import clashflux.api;
import clashflux.core;
import clashflux.store.core;

namespace clashflux::ui {

namespace website_route_detail {

#if !defined(__ANDROID__) && !defined(CLASHFLUX_IOS)
// Desktop correlation with this application's own kernel or verified pure-
// direct egress. When our kernel is accepting traffic, identity pins epoch,
// native instance, plan revision and port so results from a restarted or
// reconfigured kernel are never mixed. Blocking calls run on task threads.
std::string CurrentWebsiteRuntime() {
    const auto snapshot = store::coreStore().snapshot();
    if (snapshot.state != core::CoreState::Running || !snapshot.proxyListening ||
        snapshot.mixedPort < 1 || snapshot.mixedPort > 65535) return {};
    return std::format("{}|{}|{}|{}", snapshot.runtimeEpoch, snapshot.nativeStartedAt,
                       snapshot.planRevision, snapshot.mixedPort);
}

std::optional<WebsiteRouteBaseline> ReadWebsiteRouteBaseline() {
    const auto runtime = CurrentWebsiteRuntime();
    if (runtime.empty()) return std::nullopt;
    const auto response = store::coreStore().api().traceConnections();
    if (!response.ok) return std::nullopt;
    auto ids = wire::DecodeConnectionIds(response.body);
    if (!ids || CurrentWebsiteRuntime() != runtime) return std::nullopt;
    return WebsiteRouteBaseline{runtime, std::move(ids.value)};
}

std::string ReadWebsiteRouteLeaf(const WebsiteRouteBaseline& baseline, const std::string& host) {
    if (CurrentWebsiteRuntime() != baseline.identity) return {};
    const auto response = store::coreStore().api().traceConnections();
    if (!response.ok) return {};
    auto leaf = WebsiteRouteLeaf(response.body, baseline.connectionIds, host);
    if (leaf.empty() || CurrentWebsiteRuntime() != baseline.identity) return {};
    if (leaf != "DIRECT") {
        if (const auto catalog = store::coreStore().proxySourceObjects(false)) {
            const auto match = std::ranges::find_if(*catalog, [&leaf](const auto& object) { return object.tag == leaf; });
            if (match != catalog->end()) {
                const bool qualify = match->objectId == "DIRECT" || ShouldQualifyProxySources(*catalog);
                auto label = ProxyObjectDisplayName(match->objectId, match->sourceName, qualify);
                if (!label.empty() && label != "DIRECT") leaf = std::move(label);
            }
        }
    }
    return CurrentWebsiteRuntime() == baseline.identity ? leaf : std::string{};
}

#ifdef _WIN32
bool NoExternalTunnel() {
    const auto snap = clashflux::network_conflict::Inspect();
    if (!snap.interfacesKnown || !snap.routesKnown) return false;
    for (const auto& iface : snap.interfaces) {
        if (iface.up && iface.tunnel && (iface.capturesIpv4 || iface.capturesIpv6)) return false;
    }
    return true;
}

bool HasProxyEnvironmentVariable() {
    for (const char* name : {"HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY",
                             "http_proxy", "https_proxy", "all_proxy"}) {
        if (const char* value = std::getenv(name); value && *value != '\0') return true;
    }
    return false;
}

bool IsConfirmedPureDirectDesktop() {
    if (HasProxyEnvironmentVariable() || !NoExternalTunnel()) return false;
    INTERNET_PER_CONN_OPTIONW option{};
    option.dwOption = INTERNET_PER_CONN_FLAGS;
    INTERNET_PER_CONN_OPTION_LISTW list{};
    list.dwSize = sizeof(list);
    list.dwOptionCount = 1;
    list.pOptions = &option;
    DWORD size = sizeof(list);
    if (!InternetQueryOptionW(nullptr, INTERNET_OPTION_PER_CONNECTION_OPTION, &list, &size)) {
        return false;
    }
    const DWORD flags = option.Value.dwValue;
    return (flags & (PROXY_TYPE_PROXY | PROXY_TYPE_AUTO_PROXY_URL | PROXY_TYPE_AUTO_DETECT)) == 0;
}
#endif

huxerui::Task<std::optional<WebsiteRouteBaseline>> BeginWebsiteRoute() {
    co_return co_await RunOnTaskThread([] { return ReadWebsiteRouteBaseline(); });
}

huxerui::Task<std::string> ObserveWebsiteRoute(WebsiteRouteBaseline baseline, std::string finalUrl) {
    const std::string host = WebsiteHost(finalUrl);
    if (host.empty()) co_return std::string{};
    co_return co_await RunOnTaskThread([baseline = std::move(baseline), host] {
        return ReadWebsiteRouteLeaf(baseline, host);
    });
}

WebsiteRouteLocator MakeWebsiteRouteLocator() {
    return {
        .begin = BeginWebsiteRoute,
        .observe = ObserveWebsiteRoute,
#ifdef _WIN32
        .isDirect = []() -> huxerui::Task<bool> {
            co_return co_await RunOnTaskThread([] {
                return CurrentWebsiteRuntime().empty() && IsConfirmedPureDirectDesktop() &&
                       CurrentWebsiteRuntime().empty();
            });
        },
#endif
    };
}
#else
// The mobile UI does not observe the background engine's connections.
WebsiteRouteLocator MakeWebsiteRouteLocator() { return {}; }
#endif

} // namespace website_route_detail

namespace {

using website_route_detail::MakeWebsiteRouteLocator;

WebsiteTestModel::Probe MakeWebsiteProbe(std::shared_ptr<AppHttpClient> http) {
    const auto locator = MakeWebsiteRouteLocator();
    return [http = std::move(http), locator](std::string url) {
        const WebsiteHttpExchange exchange = [http](huxerui::HttpRequest request, WebsiteStreamObserver observe) {
            return SendWebsiteRequest(http, std::move(request), std::move(observe));
        };
        return ProbeWebsiteNetwork(exchange, std::move(url), locator);
    };
}

} // namespace

[[huxerui::composable]] huxerui::View WebsiteTestsPage(std::function<void()> onBack, bool active) {
    const float pageInset = ResolveIslandTheme(huxerui::UseTheme()).nested_radius;
    const auto model = huxerui::UseService<WebsiteTestModel>();
    const auto tasks = huxerui::UseApplicationTaskScope();
    const auto http = huxerui::UseService<AppHttpClient>();
    const auto offset = huxerui::UseState(std::make_shared<PageScrollOffset>()).Get();
    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    const auto rows = model->sites.Get();
    const bool anyActive = model->AnyActive();
    const auto probe = MakeWebsiteProbe(http);
    if (!active) return huxerui::View{};

    const auto testLabel = anyActive ? Localized("测试中…") : Localized("全部测试");
    const auto actions = WebsiteTestBatchAction(anyActive,
        testLabel, [model, tasks, probe] {
            if (model->AnyActive()) return;
            std::vector<std::size_t> ids;
            for (const auto& row : model->sites.Get()) ids.push_back(row.id);
            model->Start(tasks, probe, std::move(ids));
        });
    std::vector<huxerui::View> cards;
    for (const auto& row : rows)
        cards.push_back(WebsiteTestCard(row, [model, tasks, probe, id = row.id] {
            model->Start(tasks, probe, {id});
        }).Key("website-card-" + std::to_string(row.id)));
    const auto content = WebsiteTestGrid(cards, compact, offset);
    if (onBack) return SecondaryPageScaffold(huxerui::Text(Localized("网站测试")), actions, content,
        onBack, false, false, false, true, pageInset);
    return PageScaffold(Localized("网站测试"), actions, content, false, false, true,
        pageInset, {}, {}, true, {}, true);
}

} // namespace clashflux::ui
