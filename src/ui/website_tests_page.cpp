#include "app_http_client.h"
#include <huxerui/huxerui.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <optional>
#include <utility>
#include <vector>
#include "app_resources.h"
#include "ui.h"
#include "page_layout.h"
#include "public_ip_lookup.h"
#include "task_bridge.h"
#include "website_test_card.h"
#include "website_test_grid.h"
#include "website_test_editor.h"
#include "empty_state.h"
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
struct WebsiteRuntime { std::string identity; int port = 0; bool tunEnabled = false; };
WebsiteRuntime CurrentWebsiteRuntime() {
    const auto snapshot = store::coreStore().snapshot();
    if (snapshot.state != core::CoreState::Running || !snapshot.proxyListening ||
        snapshot.mixedPort < 1 || snapshot.mixedPort > 65535) return {};
    return {std::format("{}|{}|{}|{}", snapshot.runtimeEpoch, snapshot.nativeStartedAt,
                        snapshot.planRevision, snapshot.mixedPort),
            snapshot.mixedPort, snapshot.tunEnabled};
}

std::optional<WebsiteRouteBaseline> ReadWebsiteRouteBaseline() {
    const auto runtime = CurrentWebsiteRuntime();
    if (runtime.identity.empty()) return std::nullopt;
    const auto response = store::coreStore().api().traceConnections();
    if (!response.ok) return std::nullopt;
    auto ids = wire::DecodeConnectionIds(response.body);
    if (!ids || CurrentWebsiteRuntime().identity != runtime.identity) return std::nullopt;
    return WebsiteRouteBaseline{runtime.identity, runtime.port, runtime.tunEnabled, std::move(ids.value)};
}

std::string ReadWebsiteRouteLeaf(const WebsiteRouteBaseline& baseline, const std::string& host) {
    if (CurrentWebsiteRuntime().identity != baseline.identity) return {};
    const auto response = store::coreStore().api().traceConnections();
    if (!response.ok) return {};
    auto leaf = WebsiteRouteLeaf(response.body, baseline.connectionIds, host);
    return CurrentWebsiteRuntime().identity == baseline.identity ? leaf : std::string{};
}

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
#ifdef _WIN32
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
#else
    // On Linux/macOS we cannot inspect desktop proxy settings without spawning
    // shell tools; leave unconfirmed when our own kernel is not carrying traffic.
    return false;
#endif
}

struct EgressLookupResult {
    std::string country;
    std::vector<std::pair<std::string, std::string>> bonus;
};

std::string FetchDirectEgressCountry(long timeoutSec) {
    // Prefer domestic echo first when probing a direct connection.
    const std::array<std::string_view, 2> urls{kPublicIpLookupUrls[1], kPublicIpLookupUrls[0]};
    for (const std::string_view url : urls) {
        const api::ApiResult result = api::fetchText(std::string(url), {}, timeoutSec);
        if (!result.ok) continue;
        if (auto decoded = wire::DecodePublicIpLookup(result.body)) {
            return std::move(decoded.value.countryCode);
        }
    }
    return {};
}

// Ask an egress echo through the running mixed inbound and accept its country
// only when the kernel routed that echo through the same leaf outbound as the
// website. The curl connection is still open while the route is read.
EgressLookupResult LookupWebsiteEgress(const WebsiteRouteBaseline& baseline,
                                       const std::string& leaf, long timeoutSec) {
    EgressLookupResult out;
    const std::string proxy = "http://127.0.0.1:" + std::to_string(baseline.proxyPort);
    const std::array<std::string_view, 2> urls = leaf == "DIRECT"
        ? std::array<std::string_view, 2>{kPublicIpLookupUrls[1], kPublicIpLookupUrls[0]}
        : std::array<std::string_view, 2>{kPublicIpLookupUrls[0], kPublicIpLookupUrls[1]};
    for (const std::string_view url : urls) {
        if (CurrentWebsiteRuntime().identity != baseline.identity) return {};
        const std::string target(url);
        const std::string host = WebsiteHost(target);
        std::string observed;
        const auto result = api::fetchTextObserved(target, proxy, timeoutSec, [&] {
            const auto response = store::coreStore().api().traceConnections();
            if (response.ok) observed = WebsiteRouteLeaf(response.body, {}, host);
        });
        if (!result.ok || observed.empty()) continue;
        auto decoded = wire::DecodePublicIpLookup(result.body);
        if (!decoded) continue;
        if (observed == leaf) {
            out.country = std::move(decoded.value.countryCode);
            return out;
        }
        out.bonus.emplace_back(observed, std::move(decoded.value.countryCode));
    }
    if (leaf == "DIRECT" && !baseline.tunEnabled && NoExternalTunnel() &&
        CurrentWebsiteRuntime().identity == baseline.identity) {
        out.country = FetchDirectEgressCountry(timeoutSec);
    }
    return out;
}

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

huxerui::Task<WebsiteRouteResolution> LocateWebsiteRoute(
    std::shared_ptr<WebsiteTestModel> model,
    std::optional<WebsiteRouteBaseline> baseline, std::string observedLeaf,
    std::string host, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    const auto secondsLeft = [&] {
        return std::chrono::duration_cast<std::chrono::seconds>(
            deadline - std::chrono::steady_clock::now()).count();
    };
    if (!baseline) {
        const std::string key = "__pure_direct__";
        if (auto cached = model->CachedEgress(key)) {
            co_return WebsiteRouteResolution{std::move(*cached), WebsiteLocationSource::DirectEgress};
        }
        while (model->IsEgressInFlight(key) &&
               deadline - std::chrono::steady_clock::now() > std::chrono::milliseconds{250}) {
            co_await huxerui::Delay(std::chrono::milliseconds{25});
            if (auto cached = model->CachedEgress(key)) {
                co_return WebsiteRouteResolution{std::move(*cached), WebsiteLocationSource::DirectEgress};
            }
        }
        if (auto cached = model->CachedEgress(key)) {
            co_return WebsiteRouteResolution{std::move(*cached), WebsiteLocationSource::DirectEgress};
        }
        const long left = secondsLeft();
        if (model->IsEgressInFlight(key) || left < 1) co_return WebsiteRouteResolution{};
        const auto flight = model->AcquireEgressFlight(key);
        const long timeoutSec = static_cast<long>(std::min<long long>(left, 4));
        std::string country = co_await RunOnTaskThread([timeoutSec] {
            if (!IsConfirmedPureDirectDesktop()) return std::string{};
            return FetchDirectEgressCountry(timeoutSec);
        });
        if (!country.empty()) {
            model->StoreEgress(key, country);
            co_return WebsiteRouteResolution{std::move(country), WebsiteLocationSource::DirectEgress};
        }
        co_return WebsiteRouteResolution{};
    }

    std::string leaf = std::move(observedLeaf);
    if (leaf.empty()) {
        leaf = co_await RunOnTaskThread([baseline = *baseline, host] {
            return ReadWebsiteRouteLeaf(baseline, host);
        });
    }
    if (leaf.empty()) co_return WebsiteRouteResolution{};
    const std::string key = baseline->identity + '\n' + leaf;
    if (auto cached = model->CachedEgress(key)) {
        co_return WebsiteRouteResolution{std::move(*cached), WebsiteLocationSource::CoreOutbound};
    }
    while (model->IsEgressInFlight(key) &&
           deadline - std::chrono::steady_clock::now() > std::chrono::milliseconds{250}) {
        co_await huxerui::Delay(std::chrono::milliseconds{25});
        if (auto cached = model->CachedEgress(key)) {
            co_return WebsiteRouteResolution{std::move(*cached), WebsiteLocationSource::CoreOutbound};
        }
    }
    if (auto cached = model->CachedEgress(key)) {
        co_return WebsiteRouteResolution{std::move(*cached), WebsiteLocationSource::CoreOutbound};
    }
    const long left = secondsLeft();
    if (model->IsEgressInFlight(key) || left < 1) co_return WebsiteRouteResolution{};
    const auto flight = model->AcquireEgressFlight(key);
    const long timeoutSec = static_cast<long>(std::min<long long>(left, 4));
    EgressLookupResult lookup = co_await RunOnTaskThread([baseline = *baseline, leaf, timeoutSec] {
        return LookupWebsiteEgress(baseline, leaf, timeoutSec);
    });
    for (auto& [bonusLeaf, bonusCountry] : lookup.bonus) {
        if (!bonusLeaf.empty() && !bonusCountry.empty()) {
            model->StoreEgress(baseline->identity + '\n' + bonusLeaf, std::move(bonusCountry));
        }
    }
    if (!lookup.country.empty()) {
        model->StoreEgress(key, lookup.country);
        co_return WebsiteRouteResolution{std::move(lookup.country), WebsiteLocationSource::CoreOutbound};
    }
    co_return WebsiteRouteResolution{};
}

WebsiteRouteLocator MakeWebsiteRouteLocator(std::shared_ptr<WebsiteTestModel> model) {
    return {
        [] { return BeginWebsiteRoute(); },
        [](WebsiteRouteBaseline baseline, std::string finalUrl) {
            return ObserveWebsiteRoute(std::move(baseline), std::move(finalUrl));
        },
        [model](std::optional<WebsiteRouteBaseline> baseline, std::string observedLeaf,
                std::string host, std::chrono::milliseconds budget) {
            return LocateWebsiteRoute(model, std::move(baseline), std::move(observedLeaf),
                                      std::move(host), budget);
        },
    };
}
#else
// Mobile builds expose no Clash connection table to the UI process; the app's
// own requests also bypass its VPN. Keep the same-origin trace only.
WebsiteRouteLocator MakeWebsiteRouteLocator(std::shared_ptr<WebsiteTestModel>) { return {}; }
#endif

} // namespace website_route_detail

namespace {

using website_route_detail::MakeWebsiteRouteLocator;

huxerui::Task<std::optional<WebsiteRouteBaseline>> BeginSharedBaseline(
    std::function<huxerui::Task<std::optional<WebsiteRouteBaseline>>()> begin,
    std::shared_ptr<std::optional<WebsiteRouteBaseline>> sharedBaseline) {
    *sharedBaseline = co_await begin();
    co_return *sharedBaseline;
}

huxerui::Task<std::string> ObserveSharedStreamRoute(
    std::shared_ptr<std::optional<WebsiteRouteBaseline>> sharedBaseline,
    std::function<huxerui::Task<std::string>(WebsiteRouteBaseline, std::string)> observe,
    std::string finalUrl) {
    if (!*sharedBaseline || !observe) co_return std::string{};
    co_return co_await observe(**sharedBaseline, std::move(finalUrl));
}

WebsiteTestModel::Probe MakeWebsiteProbe(std::shared_ptr<AppHttpClient> http,
                                         std::shared_ptr<WebsiteTestModel> model) {
    const auto locator = MakeWebsiteRouteLocator(std::move(model));
    return [http = std::move(http), locator](std::string url) {
        auto sharedBaseline = std::make_shared<std::optional<WebsiteRouteBaseline>>();
        WebsiteRouteLocator bound = locator;
        if (locator.begin) {
            bound.begin = [begin = locator.begin, sharedBaseline] {
                return BeginSharedBaseline(begin, sharedBaseline);
            };
        }
        const WebsiteHttpExchange exchange =
            [http, sharedBaseline, observe = locator.observe](huxerui::HttpRequest request) {
                WebsiteStreamObserver streamObserver;
                if (request.method == huxerui::HttpMethod::Head && observe) {
                    streamObserver = [sharedBaseline, observe](std::string finalUrl) {
                        return ObserveSharedStreamRoute(sharedBaseline, observe, std::move(finalUrl));
                    };
                }
                return SendWebsiteRequest(http, std::move(request), std::move(streamObserver));
            };
        return ProbeWebsiteNetwork(exchange, std::move(url), std::move(bound));
    };
}

// Add and edit share fields and validation across presentation sizes.
[[huxerui::composable]] huxerui::View WebsiteTestEditorPage(std::shared_ptr<WebsiteTestModel> model,
    bool editing, std::function<bool()> submit) {
    const auto navigation = huxerui::UseNavigation();
    const auto back = [navigation] { static_cast<void>(navigation.Pop()); };
    const auto close = UseWebsiteEditorClose(model, back);
    const auto title = editing ? Localized("编辑") : Localized("添加网站");
    const auto action = editing ? Localized("保存") : Localized("添加网站");
    const auto icon = editing ? app::images::check : app::images::add;
    const auto commit = [submit, back] { if (submit()) back(); };
    huxerui::View content = WebsiteTestPageForm(model);
    return SecondaryPageScaffold(huxerui::Text(title),
        FormHeaderAction(icon, action, commit),
        content, close);
}

} // namespace

[[huxerui::composable]] huxerui::View WebsiteTestsPage(std::function<void()> onBack, bool active) {
    const float pageInset = ResolveIslandTheme(huxerui::UseTheme()).nested_radius;
    const auto model = huxerui::UseService<WebsiteTestModel>();
    const auto tasks = huxerui::UseApplicationTaskScope();
    const auto http = huxerui::UseService<AppHttpClient>();
    const auto menuTasks = huxerui::UseTaskScope();
    const auto toast = UseFluxToast();
    const auto dialog = huxerui::UseDialog();
    const auto navigation = huxerui::UseNavigation();
    const auto offset = huxerui::UseState(std::make_shared<PageScrollOffset>()).Get();
    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    const auto rows = model->sites.Get();
    const bool anyActive = model->AnyActive();
    const auto invalidUrl = huxerui::UseString(Localized("请输入不重复的 HTTP(S) 网站地址，最多添加 24 项。"));
    const auto probe = MakeWebsiteProbe(http, model);
    if (!active) return huxerui::View{};

    const auto openEditor = [model, compact, navigation, dialog, toast, invalidUrl](std::optional<std::size_t> id) {
        if (id) {
            const auto& current = model->sites.Get();
            const auto row = std::find_if(current.begin(), current.end(), [id](const auto& site) { return site.id == *id; });
            if (row == current.end()) return;
            model->customUrl = huxerui::TextEditingValue::FromText(row->url);
            model->customName = huxerui::TextEditingValue::FromText(row->name);
        } else {
            model->customUrl = huxerui::TextEditingValue{};
            model->customName = huxerui::TextEditingValue{};
        }
        const auto submit = [model, toast, invalidUrl, id] {
            const auto url = model->customUrl.Get().text;
            const auto name = model->customName.Get().text;
            if (id ? model->Update(*id, url, name) : model->Add(url, name)) return true;
            toast.Show(invalidUrl);
            return false;
        };
        if (compact) {
            navigation.Push([model, submit, id] { return WebsiteTestEditorPage(model, id.has_value(), submit); });
        } else {
            ShowEditorDialog(dialog, [model, submit, id](huxerui::DialogContext context) {
                return WebsiteTestEditorDialog(context, model, id.has_value(), submit);
            });
        }
    };
    const auto testLabel = anyActive ? Localized("测试中…") : Localized("全部测试");
    const auto actions = huxerui::Row {
      huxerui::IconButton(app::images::add, Localized("添加网站"))
          .With(huxerui::Tooltip(Localized("添加网站")), huxerui::Enabled(rows.size() < 24))
          .OnClick([openEditor] { openEditor(std::nullopt); }),
      WebsiteTestAction(anyActive, testLabel, [model, tasks, probe] {
              if (model->AnyActive()) return;
              std::vector<std::size_t> ids;
              for (const auto& row : model->sites.Get()) ids.push_back(row.id);
              model->Start(tasks, probe, std::move(ids));
          }).With(huxerui::Enabled(!rows.empty())),
    }.With(huxerui::Spacing(8.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    std::vector<huxerui::View> cards;
    for (const auto& row : rows)
        cards.push_back(WebsiteTestCard(row, [model, tasks, probe, id = row.id] {
            model->Start(tasks, probe, {id});
        }, [openEditor, id = row.id] { openEditor(id); },
           [model, id = row.id] { model->Remove(id); }, menuTasks).Key("website-card-" + std::to_string(row.id)));
    const auto content = cards.empty()
        ? EmptyState(Localized("还没有网站，点击右上角添加。"), app::images::globe)
        : WebsiteTestGrid(cards, compact, offset);
    if (onBack) return SecondaryPageScaffold(huxerui::Text(Localized("网站测试")), actions, content,
        onBack, false, false, false, true, pageInset);
    return PageScaffold(Localized("网站测试"), actions, content, false, false, true,
        pageInset, {}, {}, true, {}, true);
}

} // namespace clashflux::ui
