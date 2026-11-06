// external_resources_page.cpp — 单个订阅的外部资源与逐项刷新。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <tuple>
#include <cstdint>
#include <exception>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app_resources.h"
#include "empty_state.h"
#include "page_layout.h"
#include "task_bridge.h"
#include "ui.h"

import clashflux.singbox;
import clashflux.store.profiles;
import clashflux.store.core;
import clashflux.db;
import clashflux.core;
import clashflux.routing;
#include "profiles_model.h"
#include "core_model.h"

namespace clashflux::ui {
namespace {

huxerui::StringVariant ExternalResourceKindName(std::string_view kind) {
    if (kind == "proxy-provider") return Localized("代理提供者");
    if (kind == "geo") return Localized("Geo 数据");
    return Localized("规则集");
}

} // namespace

// TODO(subscription-parsing): 继续补齐规则集内部引用的外部资源类型、下载身份和状态；
// HTTP providers and supported Geo DAT/MMDB declarations share the staged refresh path.
[[huxerui::composable]] huxerui::View ExternalResourcesPage(
    std::int64_t profileId, std::string profileName,
    std::function<void()> onBack) {
    const auto& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseApplicationTaskScope();
    auto toast = UseFluxToast();
    static_cast<void>(profileName);
    const auto model = huxerui::UseService<ProfilesModel>();
    const auto coreModel = huxerui::UseService<CoreModel>();
    const auto updated = huxerui::UseString(Localized("资源已更新并应用"));
    const auto cached = huxerui::UseString(Localized("资源缓存已更新"));
    const auto source = huxerui::UseState(std::string{});
    const auto alive = huxerui::UseState(true);
    huxerui::Lifecycle([alive] { return [alive] { alive = false; }; }, 0);
    auto resources = huxerui::UseState(
        std::vector<singbox::ExternalResource>{});
    const auto caches = huxerui::UseState(std::vector<singbox::ExternalResourceCache>{});
    auto loading = huxerui::UseState(true);
    auto error = huxerui::UseState(std::string{});
    auto refresh = huxerui::UseState<std::uint64_t>(0);
    auto generation = huxerui::UseState<std::uint64_t>(0);
    const auto cleanup = [] {};
    huxerui::Lifecycle([=] {
        const auto ticket = generation.Get() + 1;
        generation = ticket;
        loading = resources.Get().empty();
        error = std::string{};
        tasks.Launch([=]() -> huxerui::Task<void> {
            std::vector<singbox::ExternalResource> found;
            std::string failure;
            try {
                const auto outcome = co_await RunOnTaskThread([profileId] {
                    std::string content;
                    try {
                        content = store::profilesStore().yamlOf(profileId);
                    } catch (const std::exception& exception) {
                        return std::tuple{std::vector<singbox::ExternalResource>{}, std::string{exception.what()}, std::string{}, std::vector<singbox::ExternalResourceCache>{}};
                    }
                    std::string scanError;
                    auto parsed = singbox::InspectExternalResources(
                        content, scanError, routing::ConnectionId(profileId));
                    // Discovery is optional for every subscription format. Keep
                    // strict diagnostics in compilation/refresh, not this list.
                    scanError.clear();
                    std::vector<singbox::ExternalResourceCache> states;
                    states.reserve(parsed.size());
                    for (const auto& resource : parsed) states.push_back(store::coreStore().externalResourceCache(resource));
                    return std::tuple{std::move(parsed), std::move(scanError), std::move(content), std::move(states)};
                });
                found = std::move(std::get<0>(outcome));
                failure = std::move(std::get<1>(outcome));
                if (alive.Get() && ticket == generation.Get()) {
                    source = std::move(std::get<2>(outcome));
                    caches = std::move(std::get<3>(outcome));
                }
            } catch (const std::exception& exception) {
                failure = exception.what();
            }
            if (!alive.Get() || ticket != generation.Get()) co_return;
            resources = std::move(found);
            error = std::move(failure);
            loading = false;
        });
        return cleanup;
    }, profileId, refresh, model->externalResourcesRevision);

    std::vector<huxerui::View> rows;
    rows.reserve(resources.Get().size());
    std::size_t resourceIndex = 0;
    for (const auto& resource : resources.Get()) {
        const auto cache = resourceIndex < caches.Get().size() ? caches.Get()[resourceIndex] : singbox::ExternalResourceCache{};
        ++resourceIndex;
        const std::string url = resource.url;
        const auto key = resource.kind + ":" + resource.cacheKey;
        const auto& pending = model->externalResourcesRefreshing.Get();
        const bool busy = std::ranges::find(pending, key) != pending.end();
        rows.push_back(huxerui::Row {
            huxerui::Image(app::images::link)
                .Tint(theme.colors.primary)
                .With(huxerui::Frame{.width = 18.0F, .height = 18.0F}),
            huxerui::Column {
                huxerui::Row {
                    FittedSingleLineLabel(resource.name, huxerui::TextStyle{
                        huxerui::Font::System(font_size::kBody), theme.colors.on_surface})
                        .With(huxerui::Grow(1.0F), huxerui::Tooltip(resource.name)),
                    huxerui::Text(ExternalResourceKindName(resource.kind))
                        .Style(huxerui::TextStyle{
                            huxerui::Font::System(font_size::kCaption),
                            theme.colors.primary}),
                }.With(huxerui::Spacing(8.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
                FittedSingleLineLabel(url,
                    huxerui::TextStyle{
                        huxerui::Font::System(font_size::kCaption),
                        theme.colors.on_surface_variant}),
                !cache.supported ? huxerui::View{} : huxerui::View{huxerui::Row {
                    huxerui::Text(cache.valid ? Localized("缓存可用") : cache.present ? Localized("缓存无效") : Localized("尚未缓存"))
                        .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption),
                            cache.present && !cache.valid ? theme.colors.error : theme.colors.on_surface_variant}),
                    cache.valid ? huxerui::View{huxerui::Text(std::to_string((cache.bytes + 1023) / 1024) + " KiB")
                        .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption), theme.colors.on_surface_variant})} : huxerui::View{},
                }.With(huxerui::Spacing(8.0F))},
            }.With(huxerui::Grow(1.0F), huxerui::Spacing(2.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
            busy ? huxerui::View{huxerui::ProgressCircle().With(
                huxerui::Frame{.width = 20.0F, .height = 20.0F}, huxerui::Padding(10.0F))} :
            huxerui::View{huxerui::IconButton(app::images::refresh, Localized("刷新资源"))
                .With(huxerui::Tooltip(resource.cacheKey.empty() ? Localized("此资源暂不支持单项刷新") : Localized("刷新资源")),
                      huxerui::Enabled(!busy && !resource.cacheKey.empty()))
                .OnClick([=] {
                    auto pending = model->externalResourcesRefreshing.Get();
                    if (std::ranges::find(pending, key) != pending.end()) return;
                    pending.push_back(key);
                    model->externalResourcesRefreshing = std::move(pending);
                    const auto content = source.Get();
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        std::pair<bool, std::string> result;
                        try {
                            result = co_await RunOnTaskThread([=] {
                                bool applied = false;
                                std::string failure;
                                store::coreStore().refreshExternalResource(profileId, content, resource, applied, failure);
                                return std::pair{applied, failure};
                            });
                        } catch (const std::exception& exception) { result.second = exception.what(); }
                        auto pending = model->externalResourcesRefreshing.Get();
                        std::erase(pending, key);
                        model->externalResourcesRefreshing = std::move(pending);
                        coreModel->RequestRefresh();
                        toast.Show(result.second.empty() ? (result.first ? updated : cached) : result.second);
                        if (alive.Get()) refresh = refresh.Get() + 1;
                    });
                })},
        }.With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(8.0F, 6.0F)),
               huxerui::Spacing(8.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
    }

    huxerui::View content;
    if (loading.Get()) {
        content = huxerui::Column {
            huxerui::Text(Localized("正在识别外部资源…")),
        }.With(huxerui::Grow(1.0F), huxerui::Padding(24.0F),
               huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    } else if (rows.empty()) {
        huxerui::View empty = EmptyState(
            Localized("未识别到外部资源"), app::images::link);
        if (!error.Get().empty()) {
            empty = huxerui::Column {
                std::move(empty),
                huxerui::Text(error.Get()).Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    theme.colors.error}),
                huxerui::Button(Localized("重试")).OnClick([refresh] {
                    refresh = refresh.Get() + 1;
                }),
            }.With(huxerui::Grow(1.0F), huxerui::Spacing(8.0F),
                   huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
        }
        content = std::move(empty);
    } else {
        huxerui::View body = Card(huxerui::Column(std::move(rows)).With(
            huxerui::Spacing(0.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)), false);
        if (!error.Get().empty()) {
            body = huxerui::Column {
                std::move(body),
                huxerui::Text(error.Get()).Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    theme.colors.error}),
            }.With(huxerui::Spacing(12.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
        }
        content = ScrollablePageContent(body);
    }

    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    if (kPageTitlesInWindow && !compact) content = huxerui::Row {
      huxerui::Spacer(),
      huxerui::Column {content}.With(huxerui::Frame{.max_width = 640.0F},
          huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
      huxerui::Spacer(),
    }.With(huxerui::Grow(1.0F), huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    return SecondaryPageScaffold(
        huxerui::Text(Localized("外部资源"), huxerui::TextRole::Title),
        huxerui::View{},
        std::move(content), std::move(onBack), false, false, false, true);
}

} // namespace clashflux::ui
