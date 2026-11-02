// external_resources_page.cpp — 单个订阅的外部资源只读清单。
#include <huxerui/huxerui.h>

#include <cstdint>
#include <exception>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app_resources.h"
#include "empty_state.h"
#include "task_bridge.h"
#include "ui.h"

import clashflux.singbox;
import clashflux.store.profiles;

namespace clashflux::ui {
namespace {

huxerui::StringVariant ExternalResourceKindName(std::string_view kind) {
    if (kind == "proxy-provider") return Localized("代理提供者");
    if (kind == "geo") return Localized("Geo 数据");
    return Localized("规则集");
}

} // namespace

// TODO(subscription-parsing): 继续补齐规则集内部引用的外部资源类型、下载身份和状态；
// 当前只识别该订阅的 Clash rule-providers(http)、proxy-providers(http) 和 geox-url。
[[huxerui::composable]] huxerui::View ExternalResourcesPage(
    std::int64_t profileId, std::string profileName,
    std::function<void()> onBack) {
    const auto& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseApplicationTaskScope();
    auto toast = UseFluxToast();
    auto clipboard = huxerui::UseApplication().Clipboard();
    auto resources = huxerui::UseState(
        std::vector<singbox::ExternalResource>{});
    auto loading = huxerui::UseState(true);
    auto error = huxerui::UseState(std::string{});
    auto refresh = huxerui::UseState<std::uint64_t>(0);
    auto generation = huxerui::UseState<std::uint64_t>(0);
    const auto cleanup = [] {};
    huxerui::Lifecycle([=] {
        const auto ticket = generation.Get() + 1;
        generation = ticket;
        loading = true;
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
                        return std::pair{std::vector<singbox::ExternalResource>{},
                                         std::string{exception.what()}};
                    }
                    if (content.empty()) {
                        return std::pair{std::vector<singbox::ExternalResource>{},
                                         std::string{"无法读取订阅配置"}};
                    }
                    std::string scanError;
                    auto parsed = singbox::InspectExternalResources(
                        content, scanError);
                    return std::pair{std::move(parsed), std::move(scanError)};
                });
                found = std::move(outcome.first);
                failure = std::move(outcome.second);
            } catch (const std::exception& exception) {
                failure = exception.what();
            }
            if (ticket != generation.Get()) co_return;
            resources = std::move(found);
            error = std::move(failure);
            loading = false;
        });
        return cleanup;
    }, profileId, refresh);

    std::vector<huxerui::View> rows;
    rows.reserve(resources.Get().size());
    for (const auto& resource : resources.Get()) {
        const std::string url = resource.url;
        rows.push_back(huxerui::Row {
            huxerui::Image(app::images::link)
                .Tint(theme.colors.primary)
                .With(huxerui::Frame{.width = 18.0F, .height = 18.0F}),
            huxerui::Column {
                huxerui::Row {
                    huxerui::Text(resource.name).With(huxerui::Grow(1.0F)),
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
            }.With(huxerui::Grow(1.0F), huxerui::Spacing(2.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
            huxerui::IconButton(app::images::copy, Localized("复制地址"))
                .With(huxerui::Tooltip(Localized("复制地址")))
                .OnClick([clipboard, toast, url] {
                    toast.Show(Localized(clipboard->WriteText(url)
                        ? "已复制到剪贴板" : "复制失败"));
                }),
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
            }.With(huxerui::Grow(1.0F), huxerui::Spacing(8.0F),
                   huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
        }
        content = std::move(empty);
    } else {
        huxerui::View body = huxerui::Column {
            huxerui::Text(profileName).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption),
                theme.colors.on_surface_variant}),
            huxerui::Text(Localized(
                "应用订阅时自动下载所需的远程规则与节点集合，手动更新订阅会刷新它们。本页显示订阅声明，不代表下载状态。")).Style(
                huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    theme.colors.on_surface_variant}),
            Card(huxerui::Column(std::move(rows)).With(
                huxerui::Spacing(0.0F),
                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
                false),
            huxerui::Text(LocalizedFormat("已识别 {} 个外部资源",
                                          resources.Get().size())),
        }.With(huxerui::Spacing(12.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
        if (!error.Get().empty()) {
            body = huxerui::Column {
                std::move(body),
                huxerui::Text(error.Get()).Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    theme.colors.error}),
            }.With(huxerui::Spacing(12.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
        }
        content = huxerui::ScrollView(std::move(body)).With(huxerui::Grow(1.0F));
    }

    return SecondaryPageScaffold(
        huxerui::Text(Localized("外部资源"), huxerui::TextRole::Title),
        huxerui::IconButton(app::images::refresh, Localized("重新识别外部资源"))
            .With(huxerui::Tooltip(Localized("重新识别外部资源")),
                  huxerui::Enabled(!loading.Get()))
            .OnClick([refresh] { refresh = refresh.Get() + 1; }),
        std::move(content), std::move(onBack), false, false, false, true);
}

} // namespace clashflux::ui
