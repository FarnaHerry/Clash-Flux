#include <huxerui/huxerui.h>

#include <functional>
#include <string>
#include <vector>

#include "app_resources.h"
#include "ui.h"
#include "empty_state.h"
#include "theme_colors.h"
#include "task_bridge.h"

import clashflux.singbox;
import clashflux.store.core;

namespace clashflux::ui {

[[huxerui::composable]] huxerui::View FidelityPage(
    std::int64_t profileId, std::function<void()> onBack, bool windowTitle) {
    const auto& theme = huxerui::UseTheme();
    const auto tasks = huxerui::UseTaskScope();
    auto report = huxerui::UseState(store::ProfileFidelityReport{});
    auto loading = huxerui::UseState(true);
    huxerui::Lifecycle([tasks, report, loading, profileId] {
        loading = true;
        report = store::ProfileFidelityReport{};
        tasks.Launch([report, loading, profileId]() -> huxerui::Task<void> {
            report = co_await RunOnTaskThread([profileId] {
                try {
                    return store::coreStore().fidelityReportForProfile(profileId);
                } catch (const std::exception& exception) {
                    store::ProfileFidelityReport result;
                    result.error = exception.what();
                    return result;
                } catch (...) {
                    store::ProfileFidelityReport result;
                    result.error = "配置保真度检查失败";
                    return result;
                }
            });
            loading = false;
        });
        return [] {};
    }, profileId);
    const auto result = report.Get();
    const auto notes = result.notes;
    const auto title = result.profileName.empty() ? Localized("配置保真度")
        : LocalizedFormat("配置保真度 · {}", result.profileName);
    const auto islands = ResolveIslandTheme(theme);
    huxerui::View content = EmptyState(Localized("暂无保真度记录"), app::images::shield_check);
    if (loading.Get()) content = EmptyState(Localized("正在检查配置保真度…"), app::images::shield_check);
    else if (!result.supported) content = EmptyState(Localized("此配置类型暂无保真度报告"), app::images::shield_check);
    if (!loading.Get() && !notes.empty()) {
        content = huxerui::VirtualList(notes.size(), [notes, theme, islands](std::size_t index) {
            const auto& note = notes[index];
            const bool unsupported = note.level == singbox::Fidelity::Unsupported;
            const auto levelColor = unsupported ? theme.colors.error : SemanticWarningColor(theme);
            std::vector<huxerui::View> rows;
            rows.push_back(huxerui::Row {
                huxerui::Text(Localized(unsupported ? "不支持" : "已近似"))
                    .With(huxerui::Foreground(levelColor)),
                huxerui::Text(note.subject).With(huxerui::Grow(1.0F)),
            }.With(huxerui::Spacing(theme.spacing.small),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
            if (!note.sourceId.empty()) rows.push_back(
                huxerui::Text(LocalizedFormat("来源：{}", note.sourceId))
                    .With(huxerui::Foreground(theme.colors.on_surface_variant)));
            rows.push_back(huxerui::Text(note.detail));
            if (!note.action.empty()) rows.push_back(
                huxerui::Text(note.action).With(huxerui::Foreground(theme.colors.on_surface_variant)));
            huxerui::View card = huxerui::Column(std::move(rows)).With(
                huxerui::Padding(theme.spacing.medium),
                huxerui::Spacing(theme.spacing.small),
                huxerui::Background(islands.raised),
                huxerui::CornerRadius(theme.shapes.medium),
                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
                .Key("fidelity-record-" + std::to_string(index));
            return huxerui::Column{card}.With(huxerui::Padding(huxerui::EdgeInsets{
                .bottom = index + 1 < notes.size() ? kSectionCardSpacing : 0.0F}));
        }).EstimatedItemExtent(120.0F)
            .With(huxerui::Grow(1.0F), huxerui::ScrollBar());
    }
    if (!result.error.empty()) {
        if (notes.empty()) content = EmptyState(
            LocalizedFormat("保真度检查失败：{}", result.error), app::images::error);
        else content = huxerui::Column {
            huxerui::Text(LocalizedFormat("保真度检查失败：{}", result.error))
                .With(huxerui::Foreground(theme.colors.error)),
            content,
        }.With(huxerui::Grow(1.0F), huxerui::Spacing(theme.spacing.medium),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }
    if (windowTitle) {
        auto back = huxerui::IconButton(app::images::arrow_back, Localized("返回上一页"))
            .With(huxerui::Tooltip(Localized("返回上一页")))
            .OnClick(onBack);
        return PageScaffold(title, {}, content,
                            true, false, true, std::nullopt, back)
            .On<huxerui::ViewEvents::BackRequested>(onBack);
    }
    return SecondaryPageScaffold(
        huxerui::Text(title, huxerui::TextRole::Title), {}, content, onBack);
}

} // namespace clashflux::ui
