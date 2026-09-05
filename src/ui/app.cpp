// app.cpp — 应用壳：自定义标题栏（Logo + 应用名 + 内核状态胶囊 + 框架窗口按钮）+
// 左侧 NavigationPane（订阅/代理/规则/连接/日志/设置）+ IndexedPages 保活页面。
//
// 主题：Material 底 + Clash 蓝主色（对齐 Clash Verge 视觉）；AppRoot 在主题
// provider 之上，UseTheme 只能拿到默认浅色 spec——根节点自身配色（整窗背景等）
// 按 dark 自选 spec，子树在 provider 之下 UseTheme 正常。
//
// 内核：首个组合即经 RunOnTaskThread 自动启动 mihomo（内核缺失时安静降级，
// 状态胶囊显示「未安装」）；托盘：显示主窗口 / 退出。
#include <huxerui/huxerui.h>

#include <chrono>
#include <string>
#include <vector>

#include "ui.h"
#include "app.h"
#include "app_resources.h"
#include "task_bridge.h"

import clashflux.config;
import clashflux.store.core;
import clashflux.store.profiles;

namespace clashflux::ui {

namespace pages {

enum PageIndex : std::size_t {
    kProfiles = 0,
    kProxies = 1,
    kRules = 2,
    kConnections = 3,
    kLogs = 4,
    kSettings = 5,
};

} // namespace pages

namespace {

// Clash 蓝主色（对齐 Clash Verge 视觉锚点）。
huxerui::ThemeSpec FluxDarkThemeSpec() {
    huxerui::ThemeSpec spec = huxerui::MaterialDarkThemeSpec();
    spec.typography = huxerui::TypographyScheme{
        .body_large = 16.0F,
        .body_medium = font_size::kBody,
        .body_small = font_size::kChip,
        .label_large = font_size::kBody,
        .title_large = font_size::kTitle,
        .headline_small = 24.0F,
    };
    spec.colors.primary = huxerui::Color::Rgb(96, 147, 250);       // #3B82F6 亮档
    spec.colors.on_primary = huxerui::Color::Rgb(250, 250, 251);
    return spec;
}

huxerui::ThemeSpec FluxLightThemeSpec() {
    huxerui::ThemeSpec spec = huxerui::MaterialLightThemeSpec();
    spec.typography = huxerui::TypographyScheme{
        .body_large = 16.0F,
        .body_medium = font_size::kBody,
        .body_small = font_size::kChip,
        .label_large = font_size::kBody,
        .title_large = font_size::kTitle,
        .headline_small = 24.0F,
    };
    spec.colors.primary = huxerui::Color::Rgb(59, 130, 246);       // #3B82F6
    spec.colors.on_primary = huxerui::Color::Rgb(250, 250, 251);
    return spec;
}

// 主题边界：MaterialThemeDefinition(spec) 之上用 typed style 覆盖组件样式——
// 按钮圆角统一 8px（M3 默认全圆胶囊）。
huxerui::View FluxThemed(bool dark, huxerui::View content) {
    const huxerui::ThemeSpec spec = dark ? FluxDarkThemeSpec() : FluxLightThemeSpec();
    huxerui::ThemeDefinition definition = huxerui::MaterialThemeDefinition(spec);

    huxerui::ButtonStyle buttons; // Default()：corner_radius=8、padding Symmetric(14,8)
    buttons.background = spec.colors.primary;
    buttons.label_style = huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                             spec.colors.on_primary};
    definition.Set(buttons);

    huxerui::SegmentedButtonStyle segments; // Default()：corner_radius=8
    segments.background = spec.colors.surface;
    segments.selected_background = spec.colors.primary;
    segments.label_style = huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                              spec.colors.on_surface};
    segments.selected_label = spec.colors.on_primary;
    segments.border = spec.colors.outline;
    segments.selected_border = spec.colors.primary;
    definition.Set(segments);

    return huxerui::Theme(std::move(definition), content);
}

} // namespace

[[huxerui::composable]] huxerui::View AppRoot() {
    const huxerui::ApplicationHandle application = huxerui::UseApplication();
    const huxerui::WindowHandle window = huxerui::UseWindow();
    const huxerui::SystemTrayHandle tray = application.SystemTray();
    const bool trayAvailable = tray.IsAvailable();
    auto tasks = huxerui::UseTaskScope();

    // 初始值在 UseState 之前算好（组合体内不写 State）：
    // 主题模式 0=跟随系统 1=深色 2=浅色（存 settings 表 ui.theme_mode）。
    int initialThemeMode = 0;
    {
        const std::string saved = store::coreStore().setting("ui.theme_mode", "0");
        if (saved == "1" || saved == "2") initialThemeMode = std::stoi(saved);
    }
    auto themeMode = huxerui::UseState<int>(std::move(initialThemeMode));
    auto navPage = huxerui::UseState<std::size_t>(pages::kProfiles);

    // 内核自启 + 崩溃检测泵：启动是阻塞活，整段在任务线程；泵每 500ms 检查
    // 进程存活（异常退出 → Failed，快照由各页面/状态胶囊自行轮询）。
    huxerui::Lifecycle(
        [tasks] {
            tasks.Launch([]() -> huxerui::Task<void> {
                co_await RunOnTaskThread([] {
                    auto& core = store::coreStore();
                    core.init();
                    if (!cfg::mihomoBinary().empty()) {
                        core.startCore(store::profilesStore().selectedYaml());
                    }
                });
                co_await PollWhile(std::chrono::duration<double>{0.5}, [] {
                    store::coreStore().checkAlive();
                    return true;
                });
            });
            return [] {
                // 卸载（退出）时停内核：阻塞调用走任务线程池，不等结果。
            };
        },
        0);

    // 托盘：图标 + 菜单；点击托盘图标激活主窗口。仅在可用时注册。
    if (trayAvailable) {
        tray.OnActivate([window] { window.Activate(); });
        huxerui::Lifecycle(
            [tray, window, application] {
                std::vector<huxerui::MenuEntry> menuEntries;
                menuEntries.push_back(
                    huxerui::MenuItem("显示主窗口", [window] { window.Activate(); }));
                menuEntries.push_back(huxerui::MenuSection{});
                menuEntries.push_back(
                    huxerui::MenuItem("退出", [application] { application.Quit(); }));
                tray.Show(app::images::tray,
                          huxerui::SystemTrayOptions{
                              .tooltip = "Clash-Flux",
                              .menu = std::move(menuEntries)});
                return [tray] { tray.Hide(); };
            },
            0);
    }

    const bool dark =
        themeMode.Get() == 1 || (themeMode.Get() == 0 && cfg::systemPrefersDark());
    const huxerui::ThemeSpec rootSpec = dark ? FluxDarkThemeSpec() : FluxLightThemeSpec();

    // 导航项（图标为 apitab 借用的占位形，后续按语义重绘）：
    // 订阅/代理/规则/连接/日志/设置。
    const std::vector<huxerui::NavigationItem> navItems{
        huxerui::NavigationItem(app::images::request, "订阅")
            .SelectedIcon(app::images::request_selected),
        huxerui::NavigationItem(app::images::websocket, "代理")
            .SelectedIcon(app::images::websocket_selected),
        huxerui::NavigationItem(app::images::loadtest, "规则")
            .SelectedIcon(app::images::loadtest_selected),
        huxerui::NavigationItem(app::images::tcp, "连接")
            .SelectedIcon(app::images::tcp_selected),
        huxerui::NavigationItem(app::images::history, "日志")
            .SelectedIcon(app::images::history_selected),
        huxerui::NavigationItem(app::images::project_settings, "设置")
            .SelectedIcon(app::images::project_settings_selected),
    };

    std::vector<huxerui::View> pages;
    pages.push_back(ProfilesPage().Key("profiles").With(huxerui::Grow(1.0F)));
    pages.push_back(ProxiesPage().Key("proxies").With(huxerui::Grow(1.0F)));
    pages.push_back(RulesPage().Key("rules").With(huxerui::Grow(1.0F)));
    pages.push_back(ConnectionsPage().Key("connections").With(huxerui::Grow(1.0F)));
    pages.push_back(LogsPage().Key("logs").With(huxerui::Grow(1.0F)));
    pages.push_back(SettingsPage(themeMode).Key("settings").With(huxerui::Grow(1.0F)));

    huxerui::View content = huxerui::Column {
        huxerui::WindowTitleBar {
            huxerui::Text("Clash-Flux")
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kChip)
                        .WithWeight(huxerui::FontWeight::Bold),
                    rootSpec.colors.on_surface})
                .With(huxerui::WindowDragRegion{}),
            huxerui::Spacer{}.With(huxerui::Grow(1.0F), huxerui::WindowDragRegion{}),
            CoreStatusPill(),
        }
            .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(
                      rootSpec.spacing.small, 0.0F)),
                  huxerui::Spacing(rootSpec.spacing.small)),
        huxerui::Row {
            huxerui::NavigationPane(navItems, navPage.Get(), /*expanded=*/true)
                .OnChanged([tasks, navPage](std::size_t index) {
                    // 切页经推迟任务出指针事件路径（约定：事件处理器内禁止同步
                    // 写会卸载点击节点的 State）。
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        co_await huxerui::Delay(std::chrono::duration<double>{0});
                        navPage = index;
                    });
                }),
            huxerui::IndexedPages(std::move(pages), navPage.Get())
                .With(huxerui::Grow(1.0F)),
        }
            .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                  huxerui::Grow(1.0F)),
    }
        .With(huxerui::Spacing(rootSpec.spacing.extra_small),
              huxerui::Background(rootSpec.colors.background),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    return FluxThemed(dark, std::move(content));
}

} // namespace clashflux::ui
