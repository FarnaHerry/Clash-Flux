// app.cpp — 应用壳（岛屿架构 + 自定义标题栏 + 托盘，对齐 apitab 岛屿风）：
//   标题栏：应用名 + 内核状态胶囊 + 框架窗口按钮；收窄为 24px 高、去背景直接
//     融入窗口底色。品牌色为水猫标识的亮水蓝和冰青，只用于交互与强调；
//     大面积背景是中性石墨（深色）/冰雾白（浅色），深浅两套模式共用同一
//     套品牌强调色，只调整背景明度和对比度。
//   下方：左侧图标侧边栏（无岛屿包裹，直接落在窗口背景上）｜内容区（页面自己的
//   一级岛屿划分区域——PageScaffold，外壳不再套岛）。根节点刷整窗海面底色
//   （rootSpec.colors.background——AppRoot 在主题 provider 之上，UseTheme 只能
//   拿到默认浅色 spec，须按 dark 自选；子树在 provider 之下 UseTheme 正常）。
//
// 内核：Android 壳层在 VPN 服务就绪后独立启动数据面，桌面端首个组合经
// RunOnTaskThread 启动（内核缺失时安静降级，状态胶囊显示「未安装」）；托盘：
// 显示主窗口 / 退出。
#include <huxerui/huxerui.h>

#include <algorithm>
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
#define CLASHFLUX_NAVIGATION_SURFACE AndroidNavigationSurface
#define CLASHFLUX_MAIN_CONTENT AndroidMainContent
#define CLASHFLUX_SETTINGS_BACK(navPage) [navPage] { navPage = pages::kSettings; }
#else
#define CLASHFLUX_PREPARE_PLATFORM_DATA DesktopPreparePlatformDataDirectory
#define CLASHFLUX_PROFILE_REFRESH_PUMP DesktopProfileRefreshPump
#define CLASHFLUX_APPLICATION_EFFECTS DesktopApplicationEffects
#define CLASHFLUX_APP_CONTENT DesktopAppContent
#define CLASHFLUX_NAVIGATION_SURFACE DesktopNavigationSurface
#define CLASHFLUX_MAIN_CONTENT DesktopMainContent
#define CLASHFLUX_SETTINGS_BACK(navPage) std::function<void()>{}
#endif

struct FluxPalette {
    static constexpr huxerui::Color deep_navy() noexcept {
        return huxerui::Color::Rgb(11, 30, 58); // 品牌深海蓝 #0B1E3A
    }

    static constexpr huxerui::Color abyss() noexcept {
        return huxerui::Color::Rgb(6, 20, 39); // 品牌深蓝 #061427，亮色表面上的文字
    }

    static constexpr huxerui::Color water() noexcept {
        return huxerui::Color::Rgb(63, 184, 255); // 品牌亮水蓝 #3FB8FF
    }

    static constexpr huxerui::Color water_deep() noexcept {
        return huxerui::Color::Rgb(18, 137, 204); // 浅色模式交互蓝
    }

    static constexpr huxerui::Color ice() noexcept {
        return huxerui::Color::Rgb(182, 242, 255); // 品牌冰青 #B6F2FF
    }

    static constexpr huxerui::Color mist() noexcept {
        return huxerui::Color::Rgb(232, 248, 255); // 浅色水雾背景
    }
};

// Clash-Flux 品牌深色主题：石墨色（带一点冷调）作为海面和表面阶梯，靠每级
// 9-11 的亮度差拉开层次；亮水蓝只出现在交互色和容器强调上，冰青作为次级
// 强调色，避免整页淹没在同色相的深蓝里。
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
    spec.shapes = huxerui::ShapeScheme{6.0F, 12.0F, 16.0F, 22.0F, 28.0F,
                                        10000.0F};
    spec.colors.primary = FluxPalette::water();
    spec.colors.on_primary = FluxPalette::abyss();
    spec.colors.primary_container = huxerui::Color::Rgb(16, 76, 113);
    spec.colors.on_primary_container = huxerui::Color::Rgb(213, 246, 255);
    spec.colors.secondary = FluxPalette::ice();
    spec.colors.on_secondary = FluxPalette::abyss();
    spec.colors.secondary_container = huxerui::Color::Rgb(18, 66, 91);
    spec.colors.on_secondary_container = FluxPalette::ice();
    spec.colors.tertiary_container = huxerui::Color::Rgb(24, 72, 105);
    spec.colors.on_tertiary_container = huxerui::Color::Rgb(222, 248, 255);
    spec.colors.background = huxerui::Color::Rgb(13, 17, 23);
    spec.colors.surface = huxerui::Color::Rgb(22, 27, 34);
    spec.colors.surface_container_low = huxerui::Color::Rgb(28, 34, 43);
    spec.colors.surface_container = huxerui::Color::Rgb(35, 42, 53);
    spec.colors.surface_container_high = huxerui::Color::Rgb(43, 51, 64);
    spec.colors.surface_container_highest = huxerui::Color::Rgb(52, 62, 77);
    spec.colors.on_surface = huxerui::Color::Rgb(233, 237, 243);
    spec.colors.on_surface_variant = huxerui::Color::Rgb(166, 179, 196);
    spec.colors.outline = huxerui::Color::Rgb(67, 80, 95);
    spec.colors.inverse_surface = FluxPalette::mist();
    spec.colors.inverse_on_surface = FluxPalette::deep_navy();
    spec.colors.scrim = huxerui::Color::Rgb(4, 8, 14, 0.66F);
    spec.colors.error = huxerui::Color::Rgb(255, 155, 168);
    spec.interactions.focus_ring = huxerui::FocusRing{FluxPalette::ice(), 2.0F, 2.0F};
    return spec;
}

// Clash-Flux 品牌浅色主题：冰蓝白作为背景，水蓝负责主要交互，深海蓝负责次级文字，
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
    spec.shapes = huxerui::ShapeScheme{6.0F, 12.0F, 16.0F, 22.0F, 28.0F,
                                        10000.0F};
    spec.colors.primary = FluxPalette::water_deep();
    spec.colors.on_primary = huxerui::Color::White();
    spec.colors.primary_container = huxerui::Color::Rgb(199, 238, 255);
    spec.colors.on_primary_container = huxerui::Color::Rgb(8, 66, 101);
    spec.colors.secondary = huxerui::Color::Rgb(17, 119, 169);
    spec.colors.on_secondary = huxerui::Color::White();
    spec.colors.secondary_container = huxerui::Color::Rgb(215, 247, 255);
    spec.colors.on_secondary_container = huxerui::Color::Rgb(9, 70, 94);
    spec.colors.tertiary_container = FluxPalette::ice();
    spec.colors.on_tertiary_container = huxerui::Color::Rgb(8, 66, 84);
    spec.colors.background = huxerui::Color::Rgb(227, 244, 252);
    spec.colors.surface = huxerui::Color::Rgb(248, 253, 255);
    spec.colors.surface_container_low = huxerui::Color::Rgb(239, 249, 254);
    spec.colors.surface_container = huxerui::Color::Rgb(226, 243, 251);
    spec.colors.surface_container_high = huxerui::Color::Rgb(213, 237, 248);
    spec.colors.surface_container_highest = huxerui::Color::White();
    spec.colors.on_surface = FluxPalette::deep_navy();
    spec.colors.on_surface_variant = huxerui::Color::Rgb(66, 101, 127);
    spec.colors.outline = huxerui::Color::Rgb(171, 211, 231);
    spec.colors.inverse_surface = FluxPalette::deep_navy();
    spec.colors.inverse_on_surface = FluxPalette::mist();
    spec.colors.scrim = huxerui::Color::Rgb(4, 24, 46, 0.34F);
    spec.colors.error = huxerui::Color::Rgb(186, 26, 58);
    spec.interactions.focus_ring = huxerui::FocusRing{FluxPalette::water(), 2.0F, 2.0F};
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

    huxerui::ButtonStyle buttons;
    buttons.corner_radii = huxerui::CornerRadii{spec.shapes.small};
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
    navigationPane.indicator_corner_radius = spec.shapes.full;
    definition.Set(navigationPane);

    return huxerui::Theme(std::move(definition), content);
}

std::vector<huxerui::NavigationItem> DesktopNavigationItems() {
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

std::vector<huxerui::NavigationItem> AndroidNavigationItems() {
    struct Item {
        huxerui::ImageResource icon;
        huxerui::ImageResource iconSelected;
        const char* label;
    };
    const std::array<Item, 4> items{
        Item{app::images::home, app::images::home_selected, "首页"},
        Item{app::images::websocket, app::images::websocket_selected, "代理"},
        Item{app::images::request, app::images::request_selected, "订阅"},
        Item{app::images::project_settings,
             app::images::project_settings_selected, "设置"},
    };
    std::vector<huxerui::NavigationItem> destinations;
    for (const Item& item : items) {
        destinations.push_back(huxerui::NavigationItem(item.icon, item.label)
                                   .SelectedIcon(item.iconSelected));
    }
    return destinations;
}

// 桌面端不兼容 Compact：窗口最小尺寸保证 Medium，导航只构造侧边栏。
[[huxerui::composable]] huxerui::View DesktopNavigationSurface(
    huxerui::State<std::size_t> navPage) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const huxerui::ViewportClass viewport = huxerui::UseViewportClass();
    const std::vector<huxerui::NavigationItem> items = DesktopNavigationItems();
    const auto onChanged = [navPage](std::size_t index) { navPage = index; };
    return huxerui::NavigationPane(items, navPage,
                                   viewport == huxerui::ViewportClass::Expanded)
        .OnChanged(onChanged)
        .With(huxerui::Background(theme.colors.surface_container_low),
              huxerui::CornerRadius(20.0F),
              huxerui::ClipChildren());
}

// Android 仅暴露四个一级页；规则、连接和日志由设置页的“更多”入口承载。
[[huxerui::composable]] huxerui::View AndroidNavigationSurface(
    huxerui::State<std::size_t> navPage) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const std::size_t selected = navPage.Get() == pages::kHome
        ? 0U
        : navPage.Get() == pages::kProxies ? 1U
        : navPage.Get() == pages::kProfiles ? 2U : 3U;
    const auto onChanged = [navPage](std::size_t index) {
        constexpr std::array<std::size_t, 4> kDestinations{
            pages::kHome, pages::kProxies, pages::kProfiles, pages::kSettings};
        navPage = kDestinations[std::min(index, kDestinations.size() - 1)];
    };
    return huxerui::NavigationBar(AndroidNavigationItems(), selected)
        .OnChanged(onChanged)
        .With(huxerui::Background(theme.colors.surface_container_low),
              huxerui::CornerRadius(20.0F), huxerui::ClipChildren(),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View DesktopMainContent(
    huxerui::State<std::size_t> navPage, huxerui::View indexedPages,
    const IslandTheme& islands, const huxerui::ThemeSpec&) {
    return huxerui::Row {
        DesktopNavigationSurface(navPage),
        std::move(indexedPages),
    }.With(huxerui::Spacing(islands.page_gap),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
           huxerui::Grow(1.0F));
}

[[huxerui::composable]] huxerui::View AndroidMainContent(
    huxerui::State<std::size_t> navPage, huxerui::View indexedPages,
    const IslandTheme& islands, const huxerui::ThemeSpec& spec) {
    huxerui::View floatingNavigation = AndroidNavigationSurface(navPage).With(
        huxerui::Frame{.max_width = 520.0F},
        huxerui::Background(islands.base), huxerui::CornerRadius(28.0F),
        huxerui::Border{islands.outline_soft, 1.0F},
        huxerui::Shadow{huxerui::Color::Rgb(0, 0, 0, 0.28F), {}, 18.0F, 2.0F},
        huxerui::ClipChildren());
    huxerui::View dock = huxerui::Column {
        std::move(floatingNavigation),
    }.With(huxerui::Padding(huxerui::EdgeInsets{
               .right = spec.spacing.medium,
               .bottom = spec.spacing.small,
               .left = spec.spacing.medium,
           }),
           huxerui::MainAlign(huxerui::MainAxisAlignment::End),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    const bool secondary = navPage.Get() == pages::kRules ||
                           navPage.Get() == pages::kConnections ||
                           navPage.Get() == pages::kLogs;
    return huxerui::Stack {
        std::move(indexedPages),
        secondary ? huxerui::View{huxerui::Row{}} : std::move(dock),
    }.With(huxerui::Grow(1.0F),
           huxerui::Align(huxerui::HorizontalAlignment::Stretch,
                          huxerui::VerticalAlignment::Stretch));
}

// Android 的 Pager 只承载四个一级页；规则、连接和日志仍保留为完整的
// 二级页，但由设置页入口打开，不进入左右滑动范围。桌面端仍使用
// IndexedPages，避免侧边导航获得无意的拖拽行为。
#if defined(__ANDROID__)
huxerui::View BuildPageContainer(std::vector<huxerui::View> pages,
                                 huxerui::State<std::size_t> navPage,
                                 huxerui::State<std::size_t> pagerPage) {
    std::vector<huxerui::View> primaryPages;
    primaryPages.reserve(4);
    primaryPages.push_back(std::move(pages[pages::kHome]));
    primaryPages.push_back(std::move(pages[pages::kProxies]));
    primaryPages.push_back(std::move(pages[pages::kProfiles]));
    primaryPages.push_back(std::move(pages[pages::kSettings]));

    std::vector<huxerui::View> secondaryPages;
    secondaryPages.reserve(3);
    secondaryPages.push_back(std::move(pages[pages::kRules]));
    secondaryPages.push_back(std::move(pages[pages::kConnections]));
    secondaryPages.push_back(std::move(pages[pages::kLogs]));

    const bool secondary = navPage.Get() >= pages::kRules &&
                           navPage.Get() <= pages::kLogs;
    const std::size_t secondaryIndex =
        secondary ? navPage.Get() - pages::kRules : 0;
    huxerui::View primaryPager =
        huxerui::Pager(std::move(primaryPages), pagerPage)
        .ScrollAxis(huxerui::Axis::Horizontal)
        .DragEnabled(true)
        .OnChanged([navPage, pagerPage](std::size_t index) {
            constexpr std::array<std::size_t, 4> kDestinations{
                pages::kHome, pages::kProxies, pages::kProfiles, pages::kSettings};
            const std::size_t clamped =
                std::min(index, kDestinations.size() - 1);
            pagerPage = clamped;
            navPage = kDestinations[clamped];
        })
        .With(huxerui::Grow(1.0F));

    huxerui::View secondaryPage =
        huxerui::IndexedPages(std::move(secondaryPages), secondaryIndex)
            .With(huxerui::Grow(1.0F));
    return huxerui::Stack{
        secondary ? huxerui::View{huxerui::Row{}} : std::move(primaryPager),
        secondary ? std::move(secondaryPage) : huxerui::View{huxerui::Row{}},
    }.With(huxerui::Grow(1.0F));
}
#else
huxerui::View BuildPageContainer(std::vector<huxerui::View> pages,
                                 huxerui::State<std::size_t> navPage,
                                 huxerui::State<std::size_t>) {
    return huxerui::IndexedPages(std::move(pages), navPage.Get())
        .With(huxerui::Grow(1.0F));
}
#endif

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
    auto pagerPage = huxerui::UseState<std::size_t>(0);
    // Home/settings cards still navigate by the absolute page index. Keep the
    // compact Pager's four-slot index synchronized with that shared state.
    huxerui::Lifecycle(
        [navPage, pagerPage] {
            const std::size_t selected = navPage.Get();
            const std::size_t target =
                selected == pages::kProxies ? 1U
                : selected == pages::kProfiles ? 2U
                : selected == pages::kSettings ? 3U : 0U;
            if (pagerPage.Get() != target) pagerPage = target;
            return [] {};
        },
        navPage);
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
    pages.push_back(HomePage(navPage).Key("home").With(huxerui::Grow(1.0F)));
    pages.push_back(ProfilesPage().Key("profiles").With(huxerui::Grow(1.0F)));
    pages.push_back(ProxiesPage().Key("proxies").With(huxerui::Grow(1.0F)));
    pages.push_back(RulesPage(CLASHFLUX_SETTINGS_BACK(navPage))
                        .Key("rules").With(huxerui::Grow(1.0F)));
    pages.push_back(ConnectionsPage(CLASHFLUX_SETTINGS_BACK(navPage))
                        .Key("connections").With(huxerui::Grow(1.0F)));
    pages.push_back(LogsPage(CLASHFLUX_SETTINGS_BACK(navPage))
                        .Key("logs").With(huxerui::Grow(1.0F)));
    pages.push_back(SettingsPage(themeMode, navPage)
                        .Key("settings").With(huxerui::Grow(1.0F)));

    huxerui::View indexedPages =
        BuildPageContainer(std::move(pages), navPage, pagerPage);
    huxerui::View mainRow = CLASHFLUX_MAIN_CONTENT(
        navPage, std::move(indexedPages), rootIslands, rootSpec);
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
#undef CLASHFLUX_NAVIGATION_SURFACE
#undef CLASHFLUX_MAIN_CONTENT
#undef CLASHFLUX_SETTINGS_BACK
