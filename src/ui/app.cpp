// app.cpp — 应用壳（岛屿架构 + 自定义标题栏 + 托盘，对齐 apitab 岛屿风）：
//   标题栏：应用名 + 内核状态胶囊 + 框架窗口按钮；收窄为 24px 高、去背景直接
//     融入窗口底色。主题为极简 AI 黑白风（MinimalDark/MinimalLightThemeSpec，
//     对齐 apitab/tinynext 配色：深色近纯黑 + 纯白主色；浅色海面白 + 近黑主色）。
//   下方：左侧图标侧边栏（无岛屿包裹，直接落在窗口背景上）｜内容区（页面自己的
//   一级岛屿划分区域——PageScaffold，外壳不再套岛）。根节点刷整窗海面底色
//   （rootSpec.colors.background——AppRoot 在主题 provider 之上，UseTheme 只能
//   拿到默认浅色 spec，须按 dark 自选；子树在 provider 之下 UseTheme 正常）。
//
// 内核：首个组合即经 RunOnTaskThread 自动启动 mihomo（内核缺失时安静降级，
// 状态胶囊显示「未安装」）；托盘：显示主窗口 / 退出。
#include <huxerui/huxerui.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include "ui.h"
#include "app.h"
#include "app_resources.h"
#include "task_bridge.h"

import clashflux.config;
import clashflux.core;
import clashflux.store.core;
import clashflux.store.profiles;

namespace clashflux::ui {

namespace pages {

enum PageIndex : std::size_t {
    kHome = 0,
    kProfiles = 1,
    kProxies = 2,
    kRules = 3,
    kConnections = 4,
    kLogs = 5,
    kSettings = 6,
};

} // namespace pages

namespace {

// 极简 AI 黑白风主题（对齐 apitab/tinynext 的 heibu geekBlack/geekWhite 配色）：
// 深色 = 近纯黑底 + 纯白主色（主色控件白底黑字）；浅色 = 近白海面 + 近黑主色。
// 文本/描边只用地道中灰，状态色仅 error 保留柔和红。
huxerui::ThemeSpec MinimalDarkThemeSpec() {
    huxerui::ThemeSpec spec = huxerui::MaterialDarkThemeSpec();
    spec.typography = huxerui::TypographyScheme{
        .body_large = 16.0F,
        .body_medium = font_size::kBody,
        .body_small = font_size::kChip,
        .label_large = font_size::kBody,
        .title_large = font_size::kTitle,
        .headline_small = 24.0F,
    };
    spec.colors.primary = huxerui::Color::Rgb(255, 255, 255);      // 纯白主色
    spec.colors.on_primary = huxerui::Color::Rgb(10, 10, 12);      // 白底上翻黑
    spec.colors.secondary = huxerui::Color::Rgb(214, 214, 217);
    spec.colors.on_secondary = huxerui::Color::Rgb(10, 10, 12);
    spec.colors.secondary_container = huxerui::Color::Rgb(30, 30, 35);
    spec.colors.on_secondary_container = huxerui::Color::Rgb(242, 242, 242);
    spec.colors.background = huxerui::Color::Rgb(10, 10, 12);      // #0A0A0C 近纯黑（海面）
    spec.colors.surface = huxerui::Color::Rgb(14, 14, 17);
    spec.colors.surface_container_low = huxerui::Color::Rgb(19, 19, 22);
    spec.colors.surface_container = huxerui::Color::Rgb(24, 24, 28);
    spec.colors.surface_container_high = huxerui::Color::Rgb(30, 30, 35);
    spec.colors.surface_container_highest = huxerui::Color::Rgb(37, 37, 43);
    spec.colors.on_surface = huxerui::Color::Rgb(242, 242, 242);   // 0.95 白
    spec.colors.on_surface_variant = huxerui::Color::Rgb(148, 148, 153); // 0.58 灰
    spec.colors.outline = huxerui::Color::Rgb(46, 46, 52);
    spec.colors.inverse_surface = huxerui::Color::Rgb(242, 242, 242);
    spec.colors.inverse_on_surface = huxerui::Color::Rgb(10, 10, 12);
    spec.colors.error = huxerui::Color::Rgb(235, 122, 112);        // 柔和红
    return spec;
}

huxerui::ThemeSpec MinimalLightThemeSpec() {
    huxerui::ThemeSpec spec = huxerui::MaterialLightThemeSpec();
    spec.typography = huxerui::TypographyScheme{
        .body_large = 16.0F,
        .body_medium = font_size::kBody,
        .body_small = font_size::kChip,
        .label_large = font_size::kBody,
        .title_large = font_size::kTitle,
        .headline_small = 24.0F,
    };
    // 冷中性灰白：保留柔和层级，去掉米白中过强的黄/棕分量。
    spec.colors.primary = huxerui::Color::Rgb(37, 40, 45);         // #25282D
    spec.colors.on_primary = huxerui::Color::Rgb(250, 250, 251);   // #FAFAFB
    spec.colors.secondary = huxerui::Color::Rgb(104, 112, 124);    // #68707C
    spec.colors.on_secondary = huxerui::Color::Rgb(250, 250, 251);
    spec.colors.secondary_container = huxerui::Color::Rgb(231, 234, 240);
    spec.colors.on_secondary_container = huxerui::Color::Rgb(37, 40, 45);
    spec.colors.background = huxerui::Color::Rgb(243, 244, 246);   // #F3F4F6 海面
    spec.colors.surface = huxerui::Color::Rgb(250, 250, 251);      // #FAFAFB
    spec.colors.surface_container_low = huxerui::Color::Rgb(248, 249, 250);
    spec.colors.surface_container = huxerui::Color::Rgb(241, 243, 245);
    spec.colors.surface_container_high = huxerui::Color::Rgb(231, 234, 238);
    spec.colors.surface_container_highest = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.on_surface = huxerui::Color::Rgb(36, 39, 44);      // #24272C
    spec.colors.on_surface_variant = huxerui::Color::Rgb(107, 114, 128); // #6B7280
    spec.colors.outline = huxerui::Color::Rgb(216, 220, 226);      // #D8DCE2
    spec.colors.inverse_surface = huxerui::Color::Rgb(36, 39, 44);
    spec.colors.inverse_on_surface = huxerui::Color::Rgb(250, 250, 251);
    spec.colors.error = huxerui::Color::Rgb(204, 64, 51);
    return spec;
}

// 主题边界：MaterialThemeDefinition(spec) 之上用 typed style 覆盖组件样式——
// 按钮/分段按钮/菜单圆角统一 8px（M3 默认全圆胶囊），叠加层用 on_surface
// 半透明（深色下黑叠黑、浅色黑底上白叠加不可见，故不用 M3 ripple）。
huxerui::View MinimalThemed(bool dark, huxerui::View content) {
    const huxerui::ThemeSpec spec = dark ? MinimalDarkThemeSpec() : MinimalLightThemeSpec();
    huxerui::ThemeDefinition definition = huxerui::MaterialThemeDefinition(spec);

    const auto withAlpha = [](huxerui::Color c, float a) {
        c.alpha = a;
        return c;
    };

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

    // 内置确认框跟随主题（DialogStyle 是 Environment 值，经 ThemeDefinition::Set
    // 全局覆盖）；Default() 基线是白底浅色配色，逐字段换色。
    huxerui::DialogStyle dialogs = huxerui::DialogStyle::Default();
    dialogs.background = spec.colors.surface_container_high;
    dialogs.title_style = huxerui::TextStyle{
        huxerui::Font::System(font_size::kTitle).WithWeight(huxerui::FontWeight::Bold),
        spec.colors.on_surface};
    dialogs.message_style = huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                               spec.colors.on_surface};
    dialogs.positive_action_style = huxerui::TextStyle{
        huxerui::Font::System(font_size::kBody), spec.colors.on_primary};
    dialogs.positive_action_background = spec.colors.primary;
    dialogs.positive_action_indication = huxerui::Indication{
        .hover = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_primary, 0.10F)},
        .press = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_primary, 0.18F)},
    };
    dialogs.negative_action_style = huxerui::TextStyle{
        huxerui::Font::System(font_size::kBody), spec.colors.on_surface};
    dialogs.negative_action_indication = huxerui::Indication{
        .hover = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_surface, 0.06F)},
        .press = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_surface, 0.12F)},
    };
    dialogs.action_separator_color = spec.colors.outline;
    definition.Set(dialogs);

    // 下拉选择（Select）跟随主题：触发框与弹出菜单圆角统一 8px。
    huxerui::SelectStyle selects;
    selects.background = spec.colors.surface_container_highest;
    selects.foreground = spec.colors.on_surface;
    selects.border = spec.colors.outline;
    selects.indicator = spec.colors.on_surface_variant;
    selects.popup_background = spec.colors.surface_container;
    selects.active_item_background = withAlpha(spec.colors.primary, 0.08F);
    selects.selected_item_background = withAlpha(spec.colors.primary, 0.12F);
    selects.validation_error = spec.colors.error;
    selects.validation_text_style = huxerui::TextStyle{
        huxerui::Font::System(font_size::kChip), spec.colors.error};
    selects.trigger_padding = huxerui::EdgeInsets::Symmetric(spec.spacing.medium,
                                                             spec.spacing.small);
    selects.item_padding = selects.trigger_padding;
    selects.popup_shadow = huxerui::Shadow{huxerui::Color::Rgb(0, 0, 0, 0.24F), {}, 8.0F, 0.0F};
    selects.content_spacing = spec.spacing.small;
    selects.validation_spacing = spec.spacing.extra_small;
    selects.minimum_height = 48.0F;
    selects.minimum_item_height = 40.0F;
    selects.indicator_size = 20.0F;
    selects.corner_radius = spec.shapes.small;
    selects.popup_corner_radius = spec.shapes.small;
    const huxerui::Indication selectIndication{
        .hover = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_surface, 0.08F)},
        .press = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_surface, 0.12F)},
    };
    selects.indication = selectIndication;
    selects.item_indication = selectIndication;
    definition.Set(selects);

    // 菜单类弹层统一 8px 圆角、同表面同阴影。
    huxerui::MenuStyle menus = huxerui::MenuStyle::Default();
    menus.background = spec.colors.surface_container;
    menus.foreground = spec.colors.on_surface;
    menus.icon_tint = spec.colors.on_surface_variant;
    menus.separator_color = spec.colors.outline;
    menus.shadow = huxerui::Shadow{huxerui::Color::Rgb(0, 0, 0, 0.24F), {}, 8.0F, 0.0F};
    menus.corner_radius = spec.shapes.small;
    menus.item_indication = selectIndication;
    definition.Set(menus);

    return huxerui::Theme(std::move(definition), content);
}

// 左列：图标侧边栏（无岛屿包裹，选中态用实心图标变体，悬停显示文字提示）。
[[huxerui::composable]] huxerui::View SideShell(huxerui::State<std::size_t> navPage) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    // 响应式：Compact(<600) 收窄侧栏宽度与内边距。
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    struct Item {
        huxerui::ImageResource icon;
        huxerui::ImageResource icon_selected;
        const char* tooltip;
        std::size_t page;
    };
    const std::array<Item, 7> items{
        Item{app::images::home, app::images::home_selected, "首页", pages::kHome},
        Item{app::images::request, app::images::request_selected, "订阅", pages::kProfiles},
        Item{app::images::websocket, app::images::websocket_selected, "代理", pages::kProxies},
        Item{app::images::loadtest, app::images::loadtest_selected, "规则", pages::kRules},
        Item{app::images::tcp, app::images::tcp_selected, "连接", pages::kConnections},
        Item{app::images::history, app::images::history_selected, "日志", pages::kLogs},
        Item{app::images::project_settings, app::images::project_settings_selected, "设置",
             pages::kSettings},
    };

    std::vector<huxerui::View> buttons;
    for (const Item& item : items) {
        const std::size_t page = item.page;
        const huxerui::ImageResource& icon =
            navPage.Get() == page ? item.icon_selected : item.icon;
        buttons.push_back(
            huxerui::IconButton(icon, item.tooltip)
                .OnClick([tasks, navPage, page] {
                    // 切页会卸载内容子树：推迟出指针事件路径
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        co_await huxerui::Delay(std::chrono::duration<double>{0});
                        navPage = page;
                    });
                })
                .With(huxerui::Tooltip(item.tooltip)));
    }
    return huxerui::Column(std::move(buttons))
        .With(huxerui::Padding(compact ? theme.spacing.small
                                       : theme.spacing.medium),
              huxerui::Spacing(theme.spacing.small),
              huxerui::Frame{.width = compact ? 44.0F : 56.0F},
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

} // namespace

[[huxerui::composable]] huxerui::View AppRoot() {
    const huxerui::ApplicationHandle application = huxerui::UseApplication();
    const huxerui::WindowHandle window = huxerui::UseWindow();
    const huxerui::SystemTrayHandle tray = application.SystemTray();
    const bool trayAvailable = tray.IsAvailable();
    auto tasks = huxerui::UseTaskScope();

    // 初始值在 UseState 之前算好（组合体内不写 State）：
    // 主题模式 0=跟随系统 1=深色 2=浅色；未保存偏好时默认深色（极简黑白黑底）。
    int initialThemeMode = 1;
    {
        const std::string saved = store::coreStore().setting("ui.theme_mode", "1");
        if (saved == "0" || saved == "2") initialThemeMode = std::stoi(saved);
    }
    auto themeMode = huxerui::UseState<int>(std::move(initialThemeMode));
    auto navPage = huxerui::UseState<std::size_t>(pages::kHome);
    // 托盘菜单勾选态：内核泵每拍刷新；变更触发托盘菜单 Lifecycle 重建，
    // Checked 勾选保持与真实状态同步。
    auto traySysProxy = huxerui::UseState(false);
    auto trayTun = huxerui::UseState(false);
    // 托盘启用（设置页开关写 KV，泵每拍带回；关闭后关窗即退出）与关闭询问
    // 弹窗防重标记。
    auto trayEnabled =
        huxerui::UseState(store::coreStore().setting("tray.enabled", "true") == "true");
    auto closeDialogOpen = huxerui::UseState(false);
    // 托盘 TUN 门禁 Denied 时的引导弹窗（挂在主窗口上）。
    auto dialog = huxerui::UseDialog();
    auto clipboard = huxerui::UseService<huxerui::Clipboard>();

    // 内核自启 + 崩溃检测泵：启动是阻塞活，整段在任务线程；泵每 500ms 检查
    // 进程存活（异常退出 → Failed，快照由各页面/状态胶囊自行轮询）。
    huxerui::Lifecycle(
        [tasks, traySysProxy, trayTun, trayEnabled] {
            tasks.Launch([=]() -> huxerui::Task<void> {
                co_await RunOnTaskThread([] {
                    auto& core = store::coreStore();
                    core.init();
                    if (!cfg::mihomoBinary().empty()) {
                        core.startCore(store::profilesStore().selectedYaml());
                    }
                });
                co_await PollWhile(std::chrono::duration<double>{0.5}, [=] {
                    auto& core = store::coreStore();
                    core.checkAlive();
                    traySysProxy = core.systemProxyEnabled();
                    trayTun = core.snapshot().tunEnabled;
                    trayEnabled =
                        core.setting("tray.enabled", "true") == "true";
                    return true;
                });
            });
            return [] {
                // 卸载（退出）时停内核：阻塞调用走任务线程池，不等结果。
            };
        },
        0);

    // 订阅自动更新泵：每 30s 在任务线程扫描一次「允许自动更新 + 间隔已到」
    // 的订阅并逐个拉新（store::refreshDue 全程阻塞）。错误落在订阅行的
    // error 字段，由订阅卡展示，这里不弹提示。
    huxerui::Lifecycle(
        [tasks] {
            tasks.Launch([]() -> huxerui::Task<void> {
                for (;;) {
                    co_await huxerui::Delay(std::chrono::duration<double>{30.0});
                    co_await RunOnTaskThread([] {
                        store::profilesStore().refreshDue();
                    });
                }
            });
            return [] {};
        },
        0);

    // 主题派生（托盘 TUN 引导弹窗也要取 rootSpec 配色，故先于托盘块计算）。
    const bool dark =
        themeMode.Get() == 1 || (themeMode.Get() == 0 && cfg::systemPrefersDark());
    const huxerui::ThemeSpec rootSpec = dark ? MinimalDarkThemeSpec() : MinimalLightThemeSpec();
    const IslandTheme rootIslands = ResolveIslandTheme(rootSpec);

    // 托盘：图标 + 菜单（显示主窗口 / 系统代理 / TUN / 退出）；点击托盘图标
    // 激活主窗口。仅在可用时注册。系统代理/TUN 以勾选态展示，Lifecycle 依赖
    // 两个 State——任意一处（首页/设置/托盘自身）切换后菜单带最新勾选重建。
    if (trayAvailable) {
        tray.OnActivate([window] { window.Activate(); });
        huxerui::Lifecycle(
            [tray, window, application, tasks, traySysProxy, trayTun, dialog,
             clipboard, trayEnabled, textColor = rootSpec.colors.on_surface,
             hintColor = rootSpec.colors.on_surface_variant] {
                // 设置页关掉托盘：跳过注册（依赖变化重建时不 Show）；Hide 对
                // 未显示的托盘是幂等 no-op，cleanup 统一执行。
                if (trayEnabled.Get()) {
                    std::vector<huxerui::MenuEntry> menuEntries;
                menuEntries.push_back(
                    huxerui::MenuItem("显示主窗口", [window] { window.Activate(); }));
                menuEntries.push_back(huxerui::MenuSection{});
                menuEntries.push_back(
                    huxerui::MenuItem("系统代理", [tasks, traySysProxy] {
                        tasks.Launch([=]() -> huxerui::Task<void> {
                            const bool next = !traySysProxy.Get();
                            const bool ok = co_await RunOnTaskThread([next] {
                                return store::coreStore().applySystemProxy(next);
                            });
                            if (ok) traySysProxy = next;  // 失败由泵回滚显示
                        });
                    }).Checked(traySysProxy.Get()));
                menuEntries.push_back(
                    huxerui::MenuItem("TUN 模式",
                                      [tasks, trayTun, window, dialog, clipboard,
                                       textColor, hintColor] {
                        tasks.Launch([=]() -> huxerui::Task<void> {
                            const bool next = !trayTun.Get();
                            if (next) {
                                // 门禁/弹窗会卸载点击路径：先让出一拍（约定 4/6）。
                                co_await huxerui::Delay(
                                    std::chrono::duration<double>{0});
                                const core::TunGate gate = co_await RunOnTaskThread(
                                    [] { return core::tunGate(); });
                                if (gate == core::TunGate::Elevated) {
                                    co_return;  // 新实例自行开启 TUN
                                }
                                if (gate == core::TunGate::Denied) {
                                    window.Activate();  // 引导弹窗在窗口里
                                    ShowTunGuideDialog(dialog, clipboard,
                                                       textColor, hintColor);
                                    co_return;
                                }
                            }
                            const bool ok = co_await RunOnTaskThread([next] {
                                return store::coreStore().applyTun(next);
                            });
                            if (ok) trayTun = next;
                        });
                    }).Checked(trayTun.Get()));
                menuEntries.push_back(huxerui::MenuSection{});
                menuEntries.push_back(
                    huxerui::MenuItem("退出", [application] { application.Quit(); }));
                tray.Show(app::images::tray,
                          huxerui::SystemTrayOptions{
                              .tooltip = "Clash-Flux",
                              .menu = std::move(menuEntries)});
                }
                return [tray] { tray.Hide(); };
            },
            traySysProxy, trayTun, trayEnabled);
    }

    // ---- 关闭窗口行为（托盘功能核心：驻留托盘继续代理）----
    // tray.close_behavior：0 = 每次询问 / 1 = 直接退出 / 2 = 最小化到托盘。
    // 托盘不可用（平台不支持或设置页关闭）时一律直接退出。
    {
        const huxerui::Color closeHintColor = rootSpec.colors.on_surface_variant;
        // 隐藏到托盘：Hide 会卸载窗口子树，推迟出事件路径。
        auto hideToTray = [tasks, window] {
            tasks.Launch([=]() -> huxerui::Task<void> {
                co_await huxerui::Delay(std::chrono::duration<double>{0});
                window.Hide();
            });
        };
        window.OnCloseRequest(
            [=]() mutable -> bool {
                if (!trayAvailable || !trayEnabled.Get()) return false;
                const std::string behavior =
                    store::coreStore().setting("tray.close_behavior", "0");
                if (behavior == "1") return false;
                if (behavior == "2") {
                    hideToTray();
                    return true;
                }
                // 0 = 询问（防重复弹窗；事件路径上只置标记，弹窗推迟）。
                if (closeDialogOpen.Get()) return true;
                closeDialogOpen = true;
                tasks.Launch([=]() -> huxerui::Task<void> {
                    co_await huxerui::Delay(std::chrono::duration<double>{0});
                    dialog.Show(
                        [=](huxerui::DialogContext ctx) -> huxerui::View {
                            return DialogCard(huxerui::Column {
                                huxerui::Text("关闭 Clash-Flux？",
                                              huxerui::TextRole::Title),
                                huxerui::Text("直接退出将停止代理；最小化到托盘"
                                              "后代理继续在后台运行。")
                                    .Style(huxerui::TextStyle{
                                        huxerui::Font::System(
                                            font_size::kCaption),
                                        closeHintColor}),
                                huxerui::Row {
                                    huxerui::Button("直接关闭")
                                        .OnClick([=] {
                                            ctx.Dismiss();
                                            closeDialogOpen = false;
                                            tasks.Launch(
                                                [=]() -> huxerui::Task<void> {
                                                    co_await huxerui::Delay(
                                                        std::chrono::duration<
                                                            double>{0});
                                                    window.Close();
                                                });
                                        }),
                                    huxerui::Button("最小化到托盘")
                                        .OnClick([=] {
                                            ctx.Dismiss();
                                            closeDialogOpen = false;
                                            hideToTray();
                                        }),
                                    huxerui::Button("取消")
                                        .OnClick([=] {
                                            ctx.Dismiss();
                                            closeDialogOpen = false;
                                        }),
                                }.With(
                                    huxerui::Spacing(8.0F),
                                    huxerui::MainAlign(
                                        huxerui::MainAxisAlignment::
                                            SpaceBetween)),
                            }
                                              .With(
                                                  huxerui::Spacing(12.0F),
                                                  huxerui::Frame{.width = 420.0F},
                                                  huxerui::CrossAlign(
                                                      huxerui::
                                                          CrossAxisAlignment::
                                                              Stretch)));
                        },
                        huxerui::DialogOptions{});
                });
                return true;
            },
            0);

        // 启动时隐藏到托盘（需托盘可用且启用，否则无入口恢复窗口）。
        if (trayAvailable && trayEnabled.Get() &&
            store::coreStore().setting("tray.start_minimized", "false") ==
                "true") {
            window.Hide();
        }
    }

    std::vector<huxerui::View> pages;
    pages.push_back(HomePage().Key("home").With(huxerui::Grow(1.0F)));
    pages.push_back(ProfilesPage().Key("profiles").With(huxerui::Grow(1.0F)));
    pages.push_back(ProxiesPage().Key("proxies").With(huxerui::Grow(1.0F)));
    pages.push_back(RulesPage().Key("rules").With(huxerui::Grow(1.0F)));
    pages.push_back(ConnectionsPage().Key("connections").With(huxerui::Grow(1.0F)));
    pages.push_back(LogsPage().Key("logs").With(huxerui::Grow(1.0F)));
    pages.push_back(SettingsPage(themeMode).Key("settings").With(huxerui::Grow(1.0F)));

    huxerui::View content = huxerui::Column {
        // 自定义标题栏：应用名 + 拖拽区 + 内核状态胶囊（框架在其右侧渲染窗口
        // 按钮）。收窄 + 去背景：直接融入窗口海面底色；垂直零内边距，内容本身
        // 24pt 高，与 title_bar_height 对齐。
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
        // 主行：图标侧栏（无岛屿包裹）+ 内容区；Grow 吃满标题栏之外剩余高度。
        // 内容区不再套外壳岛：区域划分由各页面自己的一级岛（PageScaffold）承担。
        huxerui::Row {
            SideShell(navPage),
            huxerui::IndexedPages(std::move(pages), navPage.Get())
                .With(huxerui::Grow(1.0F)),
        }
            .With(huxerui::Spacing(rootIslands.page_gap),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                  huxerui::Grow(1.0F)),
    }
        .With(huxerui::Spacing(rootSpec.spacing.extra_small),
              // 窗口整体海面底色刷满根节点：岛间缝隙透出底色形成层次。
              huxerui::Background(rootSpec.colors.background),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    return MinimalThemed(dark, std::move(content));
}

} // namespace clashflux::ui
