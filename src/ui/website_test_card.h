#pragma once
#include "website_test_model.h"
#include "ui.h"
#include "app_resources.h"
#include "action_menu.h"

namespace clashflux::ui {

inline huxerui::View WebsiteTestCardContent(
    WebsiteTestSite site, std::function<void()> toggle,
    std::function<void()> edit, std::function<void()> remove) {
    return huxerui::Scope([site, toggle, edit, remove] {
        const auto& theme = huxerui::UseTheme();
        const auto menu = UseActionMenu();
        const auto dialog = huxerui::UseDialog();
        const auto confirmDelete = [dialog, name = site.name, remove] {
            dialog.Show([name, remove](huxerui::DialogContext context) {
                return DialogScaffold(DialogHeader(Localized("删除网站")),
                    huxerui::Text(LocalizedFormat("确定删除“{}”吗？此操作无法撤销。", name)),
                    DialogActions({
                        DialogAction(Localized("取消"), [context] { context.Dismiss(); }, false),
                        DialogAction(Localized("删除"), [context, remove] { context.Dismiss(); remove(); },
                            true, true, true).Key("website-delete-confirm"),
                    }), DialogSize::Small);
            });
        };
        const auto& result = site.result;
        const bool ownActive = result.phase == WebsiteTestPhase::Queued || result.phase == WebsiteTestPhase::Running;
        const auto testLabel = ownActive ? Localized("取消") : Localized("测试");
        auto status = Localized("未测试");
        auto color = theme.colors.on_surface_variant;
        if (result.phase == WebsiteTestPhase::Queued) status = Localized("等待测试");
        if (result.phase == WebsiteTestPhase::Running) { status = Localized("测试中…"); color = theme.colors.primary; }
        if (result.phase == WebsiteTestPhase::Cancelled) status = Localized("已取消");
        if (result.phase == WebsiteTestPhase::Failed) { status = Localized("连接失败"); color = theme.colors.error; }
        if (result.phase == WebsiteTestPhase::Responded) {
            status = result.status >= 200 && result.status < 400 ? Localized("访问成功") : Localized("网站已响应");
            color = result.status >= 200 && result.status < 400 ? SemanticSuccessColor(theme) : SemanticWarningColor(theme);
        }
        return huxerui::Column {
          huxerui::Row {
            huxerui::Image(app::images::globe).Tint(theme.colors.primary).With(huxerui::Frame{.width = 20, .height = 20}),
            huxerui::Text(site.name, huxerui::TextRole::Title).With(huxerui::Grow(1.0F)),
            huxerui::IconButton(ownActive ? app::images::close : app::images::play, testLabel)
                .With(huxerui::Tooltip(testLabel)).OnClick(toggle)
                .Key("website-test-" + std::to_string(site.id)),
            huxerui::IconButton(app::images::more_vertical, Localized("更多操作"))
                .With(menu.Anchor(), huxerui::Tooltip(Localized("更多操作")))
                .OnClick([menu, edit, confirmDelete] {
                    menu.Show({
                        ActionMenuItem(app::images::edit, Localized("编辑网站"), edit),
                        ActionMenuItem(app::images::trash, Localized("删除"), confirmDelete).Danger(),
                    });
                }).Key("website-menu-" + std::to_string(site.id)),
          }.With(huxerui::Spacing(8.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
          huxerui::Text(site.url).Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption),
              theme.colors.on_surface_variant}),
          huxerui::Row {
            huxerui::Text(status).With(huxerui::Foreground(color), huxerui::Grow(1.0F)),
            result.phase == WebsiteTestPhase::Responded
                ? huxerui::View{huxerui::Text("HTTP " + std::to_string(result.status))} : huxerui::View{},
            (result.phase == WebsiteTestPhase::Responded || result.phase == WebsiteTestPhase::Failed)
                ? huxerui::View{huxerui::Text(LocalizedFormat("{} ms", result.elapsedMs))} : huxerui::View{},
          }.With(huxerui::Spacing(8.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
          result.error.empty() ? huxerui::View{} : huxerui::View{
            huxerui::Text(result.error)
                .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption), theme.colors.on_surface_variant})},
        }.With(huxerui::Spacing(8.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    });
}

} // namespace clashflux::ui
