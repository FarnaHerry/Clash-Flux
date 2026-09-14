// app.cpp — 应用壳（岛屿架构 + 自定义标题栏 + 托盘，对齐 apitab 岛屿风）：
//   标题栏：应用名 + 内核状态胶囊 + 框架窗口按钮；收窄为 24px 高、去背景直接
//     融入窗口底色。主题沿用 Clash-Flux 品牌的午夜蓝、靛蓝和冰青配色，
//     深浅两套模式共用同一品牌色相，只调整明度和对比度。
//   下方：左侧图标侧边栏（无岛屿包裹，直接落在窗口背景上）｜内容区（页面自己的
//   一级岛屿划分区域——PageScaffold，外壳不再套岛）。根节点刷整窗海面底色
//   （rootSpec.colors.background——AppRoot 在主题 provider 之上，UseTheme 只能
//   拿到默认浅色 spec，须按 dark 自选；子树在 provider 之下 UseTheme 正常）。
//
// 内核：Android 壳层在 VPN 服务就绪后独立启动数据面，桌面端首个组合经
// RunOnTaskThread 启动（内核缺失时安静降级，状态胶囊显示「未安装」）；托盘：
// 显示主窗口 / 退出。
#include <huxerui/huxerui.h>

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "ui.h"
#include "app.h"
#include "app_resources.h"

import clashflux.config;
import clashflux.store.core;

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

#if defined(__ANDROID__)
#define CLASHFLUX_PREPARE_PLATFORM_DATA AndroidPreparePlatformDataDirectory
#define CLASHFLUX_PROFILE_REFRESH_PUMP AndroidProfileRefreshPump
#define CLASHFLUX_APPLICATION_EFFECTS AndroidApplicationEffects
#define CLASHFLUX_APP_CONTENT AndroidAppContent
#else
#define CLASHFLUX_PREPARE_PLATFORM_DATA DesktopPreparePlatformDataDirectory
#define CLASHFLUX_PROFILE_REFRESH_PUMP DesktopProfileRefreshPump
#define CLASHFLUX_APPLICATION_EFFECTS DesktopApplicationEffects
#define CLASHFLUX_APP_CONTENT DesktopAppContent
#endif

struct FluxPalette {
    static constexpr huxerui::Color deep_navy() noexcept {
        return huxerui::Color::Rgb(11, 16, 32); // 品牌午夜蓝 #0B1020
    }

    static constexpr huxerui::Color midnight() noexcept {
        return huxerui::Color::Rgb(17, 21, 38); // face #111526
    }

    static constexpr huxerui::Color indigo() noexcept {
        return huxerui::Color::Rgb(58, 99, 224); // edge #3A63E0
    }

    static constexpr huxerui::Color indigo_soft() noexcept {
        return huxerui::Color::Rgb(111, 131, 222); // edge highlight #6F83DE
    }

    static constexpr huxerui::Color indigo_bright() noexcept {
        return huxerui::Color::Rgb(184, 200, 255); // edge highlight #B8C8FF
    }

    static constexpr huxerui::Color cyan() noexcept {
        return huxerui::Color::Rgb(85, 191, 241); // eye gradient #55BFF1
    }

    static constexpr huxerui::Color cyan_bright() noexcept {
        return huxerui::Color::Rgb(155, 230, 255); // eye gradient #9BE6FF
    }

};

// Clash-Flux 品牌深色主题：午夜蓝作为海面和最底层，靛蓝作为主要交互色，
// 冰青作为次级强调色。所有 M3 语义色都在这里落到同一品牌色相上。
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
    spec.colors.primary = FluxPalette::indigo_soft();
    spec.colors.on_primary = FluxPalette::deep_navy();
    spec.colors.primary_container = huxerui::Color::Rgb(38, 57, 141);
    spec.colors.on_primary_container = huxerui::Color::Rgb(231, 235, 255);
    spec.colors.secondary = FluxPalette::cyan();
    spec.colors.on_secondary = huxerui::Color::Rgb(7, 25, 39);
    spec.colors.secondary_container = huxerui::Color::Rgb(22, 59, 85);
    spec.colors.on_secondary_container = FluxPalette::cyan_bright();
    spec.colors.tertiary_container = huxerui::Color::Rgb(52, 52, 93);
    spec.colors.on_tertiary_container = huxerui::Color::Rgb(232, 229, 255);
    spec.colors.background = FluxPalette::deep_navy();
    spec.colors.surface = FluxPalette::midnight();
    spec.colors.surface_container_low = huxerui::Color::Rgb(20, 26, 46);
    spec.colors.surface_container = huxerui::Color::Rgb(27, 35, 64);
    spec.colors.surface_container_high = huxerui::Color::Rgb(36, 46, 82);
    spec.colors.surface_container_highest = huxerui::Color::Rgb(45, 57, 98);
    spec.colors.on_surface = huxerui::Color::Rgb(241, 244, 255);
    spec.colors.on_surface_variant = FluxPalette::indigo_bright();
    spec.colors.outline = huxerui::Color::Rgb(82, 105, 177);
    spec.colors.inverse_surface = huxerui::Color::Rgb(232, 238, 255);
    spec.colors.inverse_on_surface = FluxPalette::midnight();
    spec.colors.scrim = huxerui::Color::Rgb(5, 8, 18, 0.62F);
    spec.colors.error = huxerui::Color::Rgb(255, 155, 168);
    spec.interactions.focus_ring = huxerui::FocusRing{FluxPalette::cyan(), 2.0F, 2.0F};
    return spec;
}

// Clash-Flux 品牌浅色主题：冰蓝白作为背景，靛蓝负责主要交互，深青负责次级文字，
// 让冷色调在浅色模式依然清晰而不刺眼。
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
    spec.colors.primary = FluxPalette::indigo();
    spec.colors.on_primary = huxerui::Color::White();
    spec.colors.primary_container = huxerui::Color::Rgb(221, 229, 255);
    spec.colors.on_primary_container = huxerui::Color::Rgb(27, 47, 132);
    spec.colors.secondary = huxerui::Color::Rgb(23, 127, 168);
    spec.colors.on_secondary = huxerui::Color::White();
    spec.colors.secondary_container = huxerui::Color::Rgb(217, 243, 255);
    spec.colors.on_secondary_container = huxerui::Color::Rgb(10, 65, 90);
    spec.colors.tertiary_container = huxerui::Color::Rgb(230, 229, 255);
    spec.colors.on_tertiary_container = huxerui::Color::Rgb(52, 54, 109);
    spec.colors.background = huxerui::Color::Rgb(243, 247, 255);
    spec.colors.surface = huxerui::Color::Rgb(252, 253, 255);
    spec.colors.surface_container_low = huxerui::Color::Rgb(246, 249, 255);
    spec.colors.surface_container = huxerui::Color::Rgb(234, 240, 253);
    spec.colors.surface_container_high = huxerui::Color::Rgb(223, 232, 251);
    spec.colors.surface_container_highest = huxerui::Color::White();
    spec.colors.on_surface = huxerui::Color::Rgb(17, 26, 52);
    spec.colors.on_surface_variant = huxerui::Color::Rgb(82, 100, 142);
    spec.colors.outline = huxerui::Color::Rgb(174, 188, 224);
    spec.colors.inverse_surface = huxerui::Color::Rgb(27, 42, 88);
    spec.colors.inverse_on_surface = huxerui::Color::Rgb(244, 247, 255);
    spec.colors.scrim = huxerui::Color::Rgb(8, 16, 42, 0.32F);
    spec.colors.error = huxerui::Color::Rgb(186, 26, 58);
    spec.interactions.focus_ring = huxerui::FocusRing{FluxPalette::indigo(), 2.0F, 2.0F};
    return spec;
}

// 主题边界：MaterialThemeDefinition(spec) 之上用 typed style 覆盖组件样式——
// 按钮/分段按钮/菜单圆角统一 8px（M3 默认全圆胶囊），叠加层用 on_surface
// 半透明，让深浅模式的交互反馈都留在品牌色相内。
huxerui::View FluxThemed(bool dark, huxerui::View content) {
    const huxerui::ThemeSpec spec = dark ? FluxDarkThemeSpec() : FluxLightThemeSpec();
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
    segments.border = huxerui::Border{spec.colors.outline, 1.0F};
    segments.selected_border = huxerui::Border{spec.colors.primary, 1.0F};
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
    selects.border = huxerui::Border{spec.colors.outline, 1.0F};
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
    selects.corner_radii = spec.shapes.small;
    selects.popup_corner_radii = spec.shapes.small;
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
    menus.corner_radii = spec.shapes.small;
    menus.item_indication = selectIndication;
    definition.Set(menus);

    // 响应式导航跟随品牌主题：Compact 使用底部 NavigationBar，Medium/Expanded
    // 使用官方 NavigationPane；不再由应用手工拼接侧栏几何。
    huxerui::NavigationBarStyle navigationBar =
        huxerui::NavigationBarStyle::Default();
    navigationBar.background = spec.colors.surface_container_low;
    navigationBar.label_style = huxerui::TextStyle{
        huxerui::Font::System(font_size::kCaption), spec.colors.on_surface_variant};
    navigationBar.selected_content = spec.colors.on_primary_container;
    navigationBar.indicator = spec.colors.primary_container;
    navigationBar.indicator_size = huxerui::Size{40.0F, 30.0F};
    navigationBar.indicator_corner_radius = spec.shapes.small;
    navigationBar.item_padding = huxerui::EdgeInsets::Symmetric(2.0F, 2.0F);
    navigationBar.minimum_item_width = 44.0F;
    navigationBar.icon_size = 20.0F;
    navigationBar.icon_spacing = 2.0F;
    navigationBar.show_unselected_labels = false;
    // 悬浮导航岛内部的选中切换使用胶囊圆角，与外层岛屿保持同一套圆润语言。
    navigationBar.indicator_corner_radius = 20.0F;
    definition.Set(navigationBar);

    huxerui::NavigationPaneStyle navigationPane =
        huxerui::NavigationPaneStyle::Default();
    navigationPane.background = spec.colors.surface_container_low;
    navigationPane.label_style = huxerui::TextStyle{
        huxerui::Font::System(font_size::kBody), spec.colors.on_surface_variant};
    navigationPane.selected_content = spec.colors.on_primary_container;
    navigationPane.indicator = spec.colors.primary_container;
    navigationPane.compact_width = 72.0F;
    navigationPane.expanded_min_width = 220.0F;
    navigationPane.indicator_corner_radius = spec.shapes.small;
    definition.Set(navigationPane);

    return huxerui::Theme(std::move(definition), content);
}

std::vector<huxerui::NavigationItem> NavigationItems() {
    struct Item {
        huxerui::ImageResource icon;
        huxerui::ImageResource icon_selected;
        const char* tooltip;
    };
    const std::array<Item, 7> items{
        Item{app::images::home, app::images::home_selected, "首页"},
        Item{app::images::request, app::images::request_selected, "订阅"},
        Item{app::images::websocket, app::images::websocket_selected, "代理"},
        Item{app::images::loadtest, app::images::loadtest_selected, "规则"},
        Item{app::images::tcp, app::images::tcp_selected, "连接"},
        Item{app::images::history, app::images::history_selected, "日志"},
        Item{app::images::project_settings, app::images::project_settings_selected, "设置"},
    };

    std::vector<huxerui::NavigationItem> destinations;
    for (const Item& item : items) {
        destinations.push_back(huxerui::NavigationItem(item.icon, item.tooltip)
                                   .SelectedIcon(item.icon_selected));
    }
    return destinations;
}

// 响应式导航：Compact 使用底部导航栏，Medium 使用紧凑导航栏，Expanded 展开
// 为带文字的导航面板。三种结构共享同一个 navPage，IndexedPages 继续保留页面。
[[huxerui::composable]] huxerui::View NavigationSurface(
    huxerui::State<std::size_t> navPage) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const huxerui::ViewportClass viewport = huxerui::UseViewportClass();
    const std::vector<huxerui::NavigationItem> items = NavigationItems();
    const auto onChanged = [navPage](std::size_t index) { navPage = index; };

    if (viewport == huxerui::ViewportClass::Compact) {
        return huxerui::NavigationBar(items, navPage)
            .OnChanged(onChanged)
            .With(huxerui::Background(theme.colors.surface_container_low),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }

    return huxerui::NavigationPane(items, navPage,
                                   viewport == huxerui::ViewportClass::Expanded)
        .OnChanged(onChanged)
        .With(huxerui::Background(theme.colors.surface_container_low));
}

} // namespace

[[huxerui::composable]] huxerui::View AppRoot() {
    const huxerui::ApplicationHandle application = huxerui::UseApplication();
    CLASHFLUX_PREPARE_PLATFORM_DATA(application);

    // 初始值在 UseState 之前算好（组合体内不写 State）：
    // 主题模式 0=跟随系统 1=深色 2=浅色；未保存偏好时默认使用品牌深色主题。
    int initialThemeMode = 1;
    {
        const std::string saved = store::coreStore().setting("ui.theme_mode", "1");
        if (saved == "0" || saved == "2") initialThemeMode = std::stoi(saved);
    }
    auto themeMode = huxerui::UseState<int>(std::move(initialThemeMode));
    auto navPage = huxerui::UseState<std::size_t>(pages::kHome);
    // 平台刷新泵和应用生命周期各自由平台组件收束，通用壳层只挂载它们。
    huxerui::View profileRefreshPump = CLASHFLUX_PROFILE_REFRESH_PUMP();

    // 主题派生（托盘 TUN 引导弹窗也要取 rootSpec 配色，故先于托盘块计算）。
    const bool dark =
        themeMode.Get() == 1 || (themeMode.Get() == 0 && cfg::systemPrefersDark());
    const huxerui::ThemeSpec rootSpec = dark ? FluxDarkThemeSpec() : FluxLightThemeSpec();
    const IslandTheme rootIslands = ResolveIslandTheme(rootSpec);

    huxerui::View applicationEffects =
        CLASHFLUX_APPLICATION_EFFECTS(application, rootSpec);
    std::vector<huxerui::View> pages;
    pages.push_back(HomePage().Key("home").With(huxerui::Grow(1.0F)));
    pages.push_back(ProfilesPage().Key("profiles").With(huxerui::Grow(1.0F)));
    pages.push_back(ProxiesPage().Key("proxies").With(huxerui::Grow(1.0F)));
    pages.push_back(RulesPage().Key("rules").With(huxerui::Grow(1.0F)));
    pages.push_back(ConnectionsPage().Key("connections").With(huxerui::Grow(1.0F)));
    pages.push_back(LogsPage().Key("logs").With(huxerui::Grow(1.0F)));
    pages.push_back(SettingsPage(themeMode).Key("settings").With(huxerui::Grow(1.0F)));

    const huxerui::ViewportClass viewport = huxerui::UseViewportClass();
    huxerui::View indexedPages =
        huxerui::IndexedPages(std::move(pages), navPage.Get())
            .With(huxerui::Grow(1.0F));
    huxerui::View mainRow;
    if (viewport == huxerui::ViewportClass::Compact) {
        // 手机/窄窗口：官方 NavigationBar 直接悬浮在内容岛上，不再占用
        // 页面底部的布局空间，滚动内容自然从悬浮岛下方经过。
        huxerui::View floatingNavigation = NavigationSurface(navPage).With(
            huxerui::Frame{.max_width = 520.0F},
            huxerui::Background(rootIslands.base),
            huxerui::CornerRadius(28.0F),
            huxerui::Border{rootIslands.outline_soft, 1.0F},
            huxerui::Shadow{huxerui::Color::Rgb(0, 0, 0, 0.28F), {}, 18.0F,
                            2.0F},
            huxerui::ClipChildren());
        huxerui::View floatingNavigationDock = huxerui::Column {
            std::move(floatingNavigation),
        }.With(
            huxerui::Padding(huxerui::EdgeInsets{
                .right = rootSpec.spacing.medium,
                .bottom = rootSpec.spacing.small,
                .left = rootSpec.spacing.medium,
            }),
            huxerui::MainAlign(huxerui::MainAxisAlignment::End),
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
        mainRow = huxerui::Stack {
            std::move(indexedPages),
            std::move(floatingNavigationDock),
        }
            .With(huxerui::Grow(1.0F),
                  huxerui::Align(huxerui::HorizontalAlignment::Stretch,
                                 huxerui::VerticalAlignment::Stretch));
    } else {
        // Medium 保留紧凑图标栏，Expanded 展开官方导航面板并显示文字。
        mainRow = huxerui::Row {
            NavigationSurface(navPage),
            std::move(indexedPages),
        }
            .With(huxerui::Spacing(rootIslands.page_gap),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                  huxerui::Grow(1.0F));
    }

    huxerui::View content = CLASHFLUX_APP_CONTENT(mainRow, rootSpec);

    return FluxThemed(
        dark,
        huxerui::Column {
            std::move(profileRefreshPump),
            std::move(applicationEffects),
            std::move(content),
        }.With(huxerui::Grow(1.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
}

} // namespace clashflux::ui

#undef CLASHFLUX_PREPARE_PLATFORM_DATA
#undef CLASHFLUX_PROFILE_REFRESH_PUMP
#undef CLASHFLUX_APPLICATION_EFFECTS
#undef CLASHFLUX_APP_CONTENT
