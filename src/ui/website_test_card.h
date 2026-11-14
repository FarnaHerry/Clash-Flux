#pragma once
#include "website_test_model.h"
#include "ui.h"
#include "app_resources.h"
#include "action_menu.h"
#include "refresh_motion.h"
#include <ctime>

namespace clashflux::ui {

inline huxerui::View WebsiteTestAction(bool active, huxerui::StringVariant label, std::function<void()> start) {
    return huxerui::Scope([active, label, start] {
        const auto& theme = huxerui::UseTheme();
        const auto animationReady = huxerui::UseState(false);
        huxerui::Lifecycle([animationReady] { animationReady = true; });
        return huxerui::IconButton(app::images::refresh, label)
            .With(huxerui::Tooltip(label),
                huxerui::Rotation(RefreshIconRotation(theme, active && animationReady.Get())),
                huxerui::Enabled(!active),
                huxerui::Semantics{.role = huxerui::SemanticRole::Button, .label = label, .busy = active})
            .OnClick([active, start] { if (!active) start(); });
    });
}

inline std::string WebsiteTestTime(std::int64_t value, bool full = false) {
    if (value <= 0) return "—";
    const auto time = static_cast<std::time_t>(value);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &time) != 0) return "—";
#else
    if (!localtime_r(&time, &local)) return "—";
#endif
    char text[32]{};
    std::strftime(text, sizeof(text), full ? "%Y-%m-%d %H:%M:%S" : "%H:%M:%S", &local);
    return text;
}

inline huxerui::View WebsiteCountry(std::string code) {
    return huxerui::Scope([code = std::move(code)] {
        const auto image = huxerui::UseImage(app::images::country_flags);
        const auto codes = huxerui::UseRawResource(app::raw::country_codes_txt).ReadString(true);
        auto lower = code;
        for (auto& c : lower) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        const auto position = lower.size() == 2 ? codes.find(lower) : std::string::npos;
        std::vector<huxerui::View> views;
        if (position != std::string::npos && position % 3 == 0) {
            const auto index = position / 3;
            views.push_back(huxerui::Canvas([image, index](huxerui::PaintContext& paint, huxerui::Size size) {
                paint.DrawImageRect(image, {float(index % 16) * 48.0F, float(index / 16) * 36.0F, 48.0F, 36.0F},
                    {0, 0, size.width, size.height});
            }).With(huxerui::Frame{.width = 20, .height = 15}));
        }
        views.push_back(huxerui::Text(code));
        return huxerui::Row(std::move(views)).With(huxerui::Spacing(4.0F),
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    });
}

inline huxerui::View WebsiteTestCard(
    WebsiteTestSite site, std::function<void()> start,
    std::function<void()> edit, std::function<void()> remove, huxerui::TaskScope menuTasks) {
    return huxerui::Scope([site, start, edit, remove, menuTasks] {
        const auto& theme = huxerui::UseTheme();
        const auto menu = UseActionMenu();
        const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
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
        const auto testLabel = result.phase == WebsiteTestPhase::Queued ? Localized("等待测试") :
            ownActive ? Localized("测试中…") : Localized("测试");
        auto status = Localized("未测试");
        auto color = theme.colors.on_surface_variant;
        if (result.phase == WebsiteTestPhase::Queued) status = Localized("等待测试");
        if (result.phase == WebsiteTestPhase::Running) { status = Localized("测试中…"); color = theme.colors.primary; }
        if (result.phase == WebsiteTestPhase::Cancelled) status = Localized("已取消");
        const bool success = result.phase == WebsiteTestPhase::Responded && result.status >= 200 && result.status < 400;
        if (result.phase == WebsiteTestPhase::Failed || (result.phase == WebsiteTestPhase::Responded && !success)) {
            status = Localized("失败"); color = theme.colors.error;
        }
        if (success) { status = Localized("位置未知"); color = theme.colors.on_surface_variant; }
        const auto hint = !result.error.empty() ? huxerui::StringVariant{result.error} :
            success && !result.country.empty() ? huxerui::StringVariant{result.country} : status;
        const auto showMenu = [edit, confirmDelete] {
            return std::vector<ActionMenuEntry>{
                ActionMenuItem(app::images::edit, Localized("编辑"), edit),
                ActionMenuItem(app::images::trash, Localized("删除"), confirmDelete).Danger(),
            };
        };
        auto content = huxerui::Column {
          huxerui::Row {
            huxerui::Image(app::images::globe).Tint(theme.colors.primary).With(huxerui::Frame{.width = 20, .height = 20}),
            huxerui::Text(site.name, huxerui::TextRole::Title).LineLimit(1).Overflow(huxerui::TextOverflow::Ellipsis).With(huxerui::Grow(1.0F)),
            WebsiteTestAction(ownActive, testLabel, start)
                .Key("website-test-" + std::to_string(site.id)),
            compact ? huxerui::View{huxerui::IconButton(app::images::more_vertical, Localized("更多操作"))
                .With(menu.Target(), huxerui::Tooltip(Localized("更多操作")))
                .OnClick([menu, showMenu] { menu.Show(showMenu()); })
                .Key("website-menu-" + std::to_string(site.id))} : huxerui::View{},
          }.With(huxerui::Spacing(8.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
          huxerui::Row {
            (success && !result.country.empty() ? WebsiteCountry(result.country) : huxerui::View{huxerui::Text(status)})
                .With(huxerui::Foreground(color), huxerui::Grow(1.0F), huxerui::Tooltip(hint))
                .Key("website-result-" + std::to_string(site.id)),
            huxerui::Text(WebsiteTestTime(result.completedAt))
                .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption), theme.colors.on_surface_variant})
                .With(huxerui::Tooltip(WebsiteTestTime(result.completedAt, true)))
                .Key("website-time-" + std::to_string(site.id)),
          }.With(huxerui::Spacing(8.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        }.With(huxerui::Spacing(8.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
        auto card = Card(std::move(content), false);
        if (compact) return card;
        return std::move(card).With(menu.Target(), huxerui::Focusable())
            .On<huxerui::ViewEvents::ContextMenuRequested>(
            [menu, menuTasks, showMenu](huxerui::Point position) {
                menuTasks.Launch([menu, position, entries = showMenu()]() mutable -> huxerui::Task<void> {
                    co_await huxerui::Delay(std::chrono::duration<double>{0});
                    menu.ShowAt(position, std::move(entries));
                });
            });
    });
}

} // namespace clashflux::ui
