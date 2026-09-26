// app.cpp — 应用壳（岛屿架构 + 自定义标题栏 + 托盘，对齐 apitab 岛屿风）：
//   标题栏：应用名 + 内核状态胶囊 + 框架窗口按钮；收窄为 24px 高、去背景直接
//     融入窗口底色。品牌水蓝只用于主要交互和少量强调，大面积区域使用中性灰阶；
//     深浅模式共享品牌色，仅调整底色明度与文本对比度。
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
#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include "ui.h"
#include "app.h"
#include "app_resources.h"
#include "task_bridge.h"

import clashflux.config;
import clashflux.cli;
import clashflux.cli_ipc;
import clashflux.store.core;
import clashflux.persistence;
import clashflux.stream;

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
#define CLASHFLUX_MAIN_CONTENT AndroidMainContent
#else
#define CLASHFLUX_PREPARE_PLATFORM_DATA DesktopPreparePlatformDataDirectory
#define CLASHFLUX_PROFILE_REFRESH_PUMP DesktopProfileRefreshPump
#define CLASHFLUX_APPLICATION_EFFECTS DesktopApplicationEffects
#define CLASHFLUX_APP_CONTENT DesktopAppContent
#define CLASHFLUX_MAIN_CONTENT DesktopMainContent
#endif

struct FluxPalette {
    static constexpr huxerui::Color abyss() noexcept {
        return huxerui::Color::Rgb(6, 20, 39); // 深蓝字色，用于品牌色按钮
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
};

// 品牌蓝只出现在主交互和选中态。页面、卡片、导航与浮层使用中性灰阶，
// 通过稳定的明度差区分层级，避免整页被蓝色表面染色。
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
    spec.colors.primary_container = huxerui::Color::Rgb(44, 45, 46);
    spec.colors.on_primary_container = FluxPalette::ice();
    spec.colors.secondary = huxerui::Color::Rgb(185, 185, 185);
    spec.colors.on_secondary = huxerui::Color::Rgb(32, 32, 32);
    spec.colors.secondary_container = huxerui::Color::Rgb(53, 53, 53);
    spec.colors.on_secondary_container = huxerui::Color::Rgb(226, 226, 226);
    spec.colors.tertiary_container = huxerui::Color::Rgb(46, 46, 46);
    spec.colors.on_tertiary_container = huxerui::Color::Rgb(222, 222, 222);
    spec.colors.background = huxerui::Color::Rgb(18, 19, 20);
    spec.colors.surface = huxerui::Color::Rgb(25, 26, 27);
    spec.colors.surface_container_low = huxerui::Color::Rgb(32, 33, 34);
    spec.colors.surface_container = huxerui::Color::Rgb(40, 41, 42);
    spec.colors.surface_container_high = huxerui::Color::Rgb(48, 49, 50);
    spec.colors.surface_container_highest = huxerui::Color::Rgb(56, 57, 58);
    spec.colors.on_surface = huxerui::Color::Rgb(241, 241, 241);
    spec.colors.on_surface_variant = huxerui::Color::Rgb(176, 176, 176);
    spec.colors.outline = huxerui::Color::Rgb(75, 75, 75);
    spec.colors.inverse_surface = huxerui::Color::Rgb(232, 232, 232);
    spec.colors.inverse_on_surface = huxerui::Color::Rgb(32, 32, 32);
    spec.colors.scrim = huxerui::Color::Rgb(7, 7, 7, 0.66F);
    spec.colors.error = huxerui::Color::Rgb(255, 144, 153);
    spec.interactions.focus_ring = huxerui::FocusRing{FluxPalette::ice(), 2.0F, 2.0F};
    return spec;
}

// 浅色模式用干净的中性白灰做底，主色容器只保留浅水蓝色调；正文和边界使用
// 石墨灰，保证内容层级清晰，品牌色不会扩散到大面积背景。
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
    spec.colors.on_primary = FluxPalette::abyss();
    spec.colors.primary_container = huxerui::Color::Rgb(220, 238, 255);
    spec.colors.on_primary_container = huxerui::Color::Rgb(16, 73, 103);
    spec.colors.secondary = huxerui::Color::Rgb(94, 104, 114);
    spec.colors.on_secondary = huxerui::Color::White();
    spec.colors.secondary_container = huxerui::Color::Rgb(229, 233, 237);
    spec.colors.on_secondary_container = huxerui::Color::Rgb(48, 56, 64);
    spec.colors.tertiary_container = huxerui::Color::Rgb(238, 240, 242);
    spec.colors.on_tertiary_container = huxerui::Color::Rgb(63, 70, 77);
    spec.colors.background = huxerui::Color::Rgb(239, 241, 244);
    spec.colors.surface = huxerui::Color::White();
    spec.colors.surface_container_low = huxerui::Color::Rgb(252, 253, 254);
    spec.colors.surface_container = huxerui::Color::Rgb(245, 246, 248);
    spec.colors.surface_container_high = huxerui::Color::Rgb(235, 238, 241);
    spec.colors.surface_container_highest = huxerui::Color::White();
    spec.colors.on_surface = huxerui::Color::Rgb(32, 36, 41);
    spec.colors.on_surface_variant = huxerui::Color::Rgb(98, 108, 118);
    spec.colors.outline = huxerui::Color::Rgb(211, 216, 222);
    spec.colors.inverse_surface = huxerui::Color::Rgb(37, 42, 48);
    spec.colors.inverse_on_surface = huxerui::Color::Rgb(242, 244, 246);
    spec.colors.scrim = huxerui::Color::Rgb(17, 24, 32, 0.34F);
    spec.colors.error = huxerui::Color::Rgb(180, 35, 50);
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
    // 底部悬浮导航常驻四个页签的文字标签，而不是只在选中项显示。
    navigationBar.show_unselected_labels = true;
    // 悬浮导航岛内部的选中切换使用胶囊圆角，与外层岛屿保持同一套圆润语言。
    navigationBar.indicator_corner_radius = 20.0F;
    definition.Set(navigationBar);

    huxerui::NavigationPaneStyle navigationPane =
        huxerui::NavigationPaneStyle::Default();
    navigationPane.background = huxerui::Color::Transparent();
    navigationPane.label_style = huxerui::TextStyle{
        huxerui::Font::System(font_size::kBody), spec.colors.on_surface_variant};
    navigationPane.selected_content = spec.colors.on_primary_container;
    navigationPane.indicator = spec.colors.primary_container;
    navigationPane.compact_width = 72.0F;
    navigationPane.expanded_min_width = 220.0F;
    navigationPane.item_margin = huxerui::EdgeInsets::Symmetric(0.0F, 0.0F);
    navigationPane.compact_indicator_size = huxerui::Size{44.0F, 44.0F};
    navigationPane.indicator_corner_radius = spec.shapes.medium;
    definition.Set(navigationPane);

    // 顶部状态栏与底部系统导航栏的底色统一取页面海面底色（background），
    // 避免状态栏出现一条与页面不同色的横条；Automatic 亮度会按底色深浅
    // 自动选择图标明暗。
    huxerui::SystemBarsAppearance systemBars =
        huxerui::SystemBarsAppearance::Default();
    systemBars.status_bar_background = spec.colors.background;
    systemBars.navigation_bar_background = spec.colors.background;
    definition.Set(systemBars);

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
        Item{app::images::gear, app::images::gear, "设置"},
    };

    std::vector<huxerui::NavigationItem> destinations;
    for (const Item& item : items) {
        destinations.push_back(huxerui::NavigationItem(item.icon, item.tooltip)
                                   .SelectedIcon(item.icon_selected));
    }
    return destinations;
}

// Android 底部导航条目：普通态/选中态图标 + 文字标签。
struct AndroidNavEntry {
    huxerui::ImageResource icon;
    huxerui::ImageResource icon_selected;
    const char* label;
};

// Android 仅暴露四个一级页；规则、连接和日志由设置页的“更多”入口承载。
const std::array<AndroidNavEntry, 4> kAndroidNavEntries{
    AndroidNavEntry{app::images::home, app::images::home_selected, "首页"},
    AndroidNavEntry{app::images::websocket, app::images::websocket_selected,
                    "代理"},
    AndroidNavEntry{app::images::request, app::images::request_selected, "订阅"},
    AndroidNavEntry{app::images::gear, app::images::gear, "设置"},
};

constexpr std::array<std::size_t, 4> kAndroidNavDestinations{
    pages::kHome, pages::kProxies, pages::kProfiles, pages::kSettings};

struct AndroidNavigationIndicator {
    class Extension;

    std::size_t selected_index = 0;
    huxerui::Color fill = huxerui::Color::Transparent();

    bool operator==(const AndroidNavigationIndicator&) const = default;
};

class AndroidNavigationIndicator::Extension final
    : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode& node, const AndroidNavigationIndicator& spec) {
        Update(node, spec);
    }

    void Update(huxerui::ViewNode& node,
                const AndroidNavigationIndicator& spec) {
        static_cast<void>(node);
        geometry_pending_ =
            geometry_pending_ || !initialized_ ||
            selected_index_ != spec.selected_index;
        selected_index_ = spec.selected_index;
        fill_ = spec.fill;
        initialized_ = true;
    }

    FrameResult OnFrame(huxerui::ViewNode& node,
                        const huxerui::FrameInfo& frame) override {
        static_cast<void>(node);
        const huxerui::MotionAdvanceResult result = offset_.Advance(frame);
        if (result.changed) {
            InvalidatePaint(PaintInvalidation::Content);
        }
        return FrameResult{
            .needs_frame = geometry_pending_ || result.needs_frame,
            .wake_after = result.wake_after};
    }

    PaintInvalidation PrepareGeometry(
        huxerui::ViewNode& node, huxerui::TextMeasurer&) override {
        if (selected_index_ >= node.ChildCount()) {
            return PaintInvalidation::None;
        }
        const huxerui::ViewNode& selected = node.ChildAt(selected_index_);
        const float target = selected.LayoutOffset().x +
                             (selected.LayoutSize().width - kIndicatorWidth) *
                                 0.5F;
        geometry_pending_ = false;
        if (!geometry_initialized_) {
            geometry_initialized_ = true;
            offset_.Set(target);
            return PaintInvalidation::Content;
        }
        if (offset_.Target() == target) {
            return PaintInvalidation::None;
        }
        offset_.AnimateTo(
            target, huxerui::TweenSpec{0.24, huxerui::Easing::EaseOut});
        return PaintInvalidation::Content;
    }

    void PaintBehindContent(const huxerui::ViewNode& node,
                            huxerui::PaintContext& context) const override {
        if (!geometry_initialized_ || fill_.alpha <= 0.0F) return;
        context.DrawRect(
            huxerui::Rect{offset_.Value(), 4.0F, kIndicatorWidth, 56.0F},
            fill_, 30.0F);
    }

private:
    static constexpr float kIndicatorWidth = 88.0F;
    std::size_t selected_index_ = 0;
    huxerui::Color fill_ = huxerui::Color::Transparent();
    huxerui::MotionController offset_;
    bool initialized_ = false;
    bool geometry_initialized_ = false;
    bool geometry_pending_ = false;
};

// 桌面一级导航始终使用无底板的紧凑图标栏；文字保留为语义名称。
[[huxerui::composable]] huxerui::View DesktopNavigationSurface(
    huxerui::State<std::size_t> navPage) {
    const std::vector<huxerui::NavigationItem> items = DesktopNavigationItems();
    const auto onChanged = [navPage](std::size_t index) { navPage = index; };
    return huxerui::NavigationPane(items, navPage, false).OnChanged(onChanged);
}

// 底部导航：选中态把 icon 与文字作为一个整体包进胶囊，而不是只高亮 icon。
// 内建 NavigationBar 的指示器只覆盖图标层，所以这里自绘条目；同时保留底部
// 安全区消费与系统导航栏底色，行为对齐原先的内建组件。
[[huxerui::composable]] huxerui::View AndroidNavigationSurface(
    huxerui::State<std::size_t> navPage) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const std::size_t selected = navPage.Get() == pages::kHome
        ? 0U
        : navPage.Get() == pages::kProxies ? 1U
        : navPage.Get() == pages::kProfiles ? 2U : 3U;

    std::vector<huxerui::View> entries;
    entries.reserve(kAndroidNavEntries.size());

    // 点击/悬停反馈的几何与选中胶囊完全一致（88×56、圆角 30）：默认指示层
    // 会按条目方框铺满，点按时能看到直角方框。
    huxerui::Color stateHover = theme.colors.on_surface;
    stateHover.alpha = 0.08F;
    huxerui::Color statePress = theme.colors.on_surface;
    statePress.alpha = 0.14F;
    const huxerui::Indication itemIndication{
        .geometry = huxerui::IndicationGeometry{
            .layer_size = huxerui::Size{88.0F, 56.0F},
            .clip_corner_radii = huxerui::CornerRadii{30.0F},
        },
        .hover = huxerui::IndicationLayer{.fill = stateHover},
        .press = huxerui::IndicationLayer{.fill = statePress},
    };

    for (std::size_t index = 0; index < kAndroidNavEntries.size(); ++index) {
        const AndroidNavEntry& entry = kAndroidNavEntries[index];
        const bool isSelected = index == selected;
        const huxerui::Color content =
            isSelected ? theme.colors.on_primary_container
                       : theme.colors.on_surface_variant;
        // 整块胶囊（图标 + 文字）由 AndroidNavigationIndicator 绘制并滑动；
        // 内容色随选中态即时切换。
        huxerui::View pill = huxerui::Column {
            huxerui::Image(isSelected ? entry.icon_selected : entry.icon)
                .Tint(content)
                .With(huxerui::Frame{.width = 20.0F, .height = 20.0F}),
            huxerui::Text(entry.label).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption), content}),
        }.With(huxerui::Frame{.height = 56.0F, .min_width = 88.0F},
               huxerui::Spacing(2.0F),
               huxerui::Padding(huxerui::EdgeInsets::Symmetric(16.0F, 0.0F)),
               huxerui::Background(huxerui::Color::Transparent()),
               huxerui::CornerRadius(30.0F),
               huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
        const std::size_t destination = kAndroidNavDestinations[index];
        entries.push_back(
            huxerui::Row { std::move(pill) }
                .With(huxerui::Grow(1.0F),
                      // 负向水平内边距：放宽条目的内容约束，让选中胶囊可以越过
                      // 自身方框（不再被单个条目的槽位宽度裁掉）。
                      huxerui::Padding(
                          huxerui::EdgeInsets::Symmetric(-7.0F, 0.0F)),
                      huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
                .OnClick([navPage, destination] { navPage = destination; })
                .With(huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                         .label = entry.label,
                                         .selected = isSelected},
                      itemIndication,
                      huxerui::Focusable(true), huxerui::Enabled(true))
                .Key(index));
    }

    // 系统导航栏底色跟随页面海面底色（悬浮胶囊本身不着色系统栏区域）。
    huxerui::SystemBarsAppearance systemBars =
        huxerui::UseEnvironment<huxerui::SystemBarsAppearance>();
    systemBars.navigation_bar_background = theme.colors.background;

    return huxerui::Column {
        huxerui::Row(std::move(entries))
            .With(huxerui::Frame{.height = 64.0F},
                  // 与条目负内边距配合：给越界胶囊留出空间，并在最左/最右
                  // 条目与外层胶囊之间形成一道间隙（HuxerUI 无 Margin，
                  // 用 Padding 表达）。
                  huxerui::Padding(
                      huxerui::EdgeInsets::Symmetric(14.0F, 0.0F)),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                  AndroidNavigationIndicator{selected,
                                             theme.colors.primary_container}),
    }.With(huxerui::SafeAreaPadding{.top = false},
           systemBars,
           huxerui::Semantics{.role = huxerui::SemanticRole::Navigation},
           huxerui::Background(theme.colors.surface_container_low),
           huxerui::CornerRadius(20.0F), huxerui::ClipChildren(),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// 桌面：侧边导航 + 七页 IndexedPages（规则/连接/日志是一级页）。
[[huxerui::composable]] huxerui::View DesktopMainContent(
    huxerui::State<std::size_t> navPage, huxerui::State<std::size_t>,
    huxerui::State<int> themeMode, const IslandTheme& islands,
    const huxerui::ThemeSpec&) {
    std::vector<huxerui::View> pages;
    pages.reserve(7);
    pages.push_back(HomePage(navPage).Key("home").With(huxerui::Grow(1.0F)));
    pages.push_back(ProfilesPage().Key("profiles").With(huxerui::Grow(1.0F)));
    pages.push_back(ProxiesPage().Key("proxies").With(huxerui::Grow(1.0F)));
    pages.push_back(RulesPage().Key("rules").With(huxerui::Grow(1.0F)));
    pages.push_back(
        ConnectionsPage().Key("connections").With(huxerui::Grow(1.0F)));
    pages.push_back(LogsPage().Key("logs").With(huxerui::Grow(1.0F)));
    pages.push_back(SettingsPage(themeMode, navPage)
                        .Key("settings").With(huxerui::Grow(1.0F)));
    return huxerui::Row {
        DesktopNavigationSurface(navPage),
        huxerui::IndexedPages(std::move(pages), navPage.Get())
            .With(huxerui::Grow(1.0F)),
    }.With(huxerui::Spacing(islands.page_gap),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
           huxerui::Grow(1.0F));
}

#if defined(__ANDROID__)
// 二级页（规则/连接/日志）的进入与返回动画：新页自右缘滑入、父页左移 20%，
// 返回时反向滑回。它只作用于 NavigationStack 的页面 push/pop，与一级页 Pager
// 的左右翻页动画相互独立。
huxerui::PageTransition SecondaryPageTransition(
    const huxerui::MotionScheme& motion) {
    const huxerui::TransitionSpec enter{
        huxerui::SlideTransition{.incoming_offset = {1.0F, 0.0F},
                                 .outgoing_offset = {-0.2F, 0.0F}},
        huxerui::TweenSpec{.duration = motion.slow,
                           .easing = huxerui::Easing::EaseOut}};
    return huxerui::PageTransition{
        .push = enter,
        .pop = enter.Reversed(huxerui::TweenSpec{
            .duration = motion.normal, .easing = huxerui::Easing::EaseOut}),
        .replace = enter};
}

// 一级外壳：四个一级页由 Pager 承载，悬浮 dock 与一级页同属 NavigationStack
// 的根页面；二级页 push 后整页覆盖 dock，弹出后一级页状态原样保留。
[[huxerui::composable]] huxerui::View AndroidPrimaryShell(
    huxerui::State<std::size_t> navPage, huxerui::State<std::size_t> pagerPage,
    huxerui::State<int> themeMode, const IslandTheme& islands,
    const huxerui::ThemeSpec& spec) {
    // Home/settings 卡片仍按绝对页号导航，这里把 Pager 的四槽索引与共享
    // 页号状态同步；二级页不属于 Pager，不参与该同步。
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

    std::vector<huxerui::View> primaryPages;
    primaryPages.reserve(4);
    primaryPages.push_back(HomePage(navPage).Key("home").With(huxerui::Grow(1.0F)));
    primaryPages.push_back(
        ProxiesPage().Key("proxies").With(huxerui::Grow(1.0F)));
    primaryPages.push_back(
        AndroidProfilesPage(huxerui::UseNavigation())
            .Key("profiles").With(huxerui::Grow(1.0F)));
    primaryPages.push_back(SettingsPage(themeMode, navPage)
                               .Key("settings").With(huxerui::Grow(1.0F)));

    huxerui::View pager =
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

    // 底部悬浮导航不描边也不投影：只保留表面底色与圆角，直接落在窗口海面
    // 底色上；胶囊边缘不再出现 1pt 描边或投影形成的暗色轮廓线。
    huxerui::View floatingNavigation = AndroidNavigationSurface(navPage).With(
        huxerui::Frame{.max_width = 520.0F},
        huxerui::Background(islands.base), huxerui::CornerRadius(34.0F),
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
    return huxerui::Stack {
        std::move(pager),
        std::move(dock),
    }.With(huxerui::Grow(1.0F),
           huxerui::Align(huxerui::HorizontalAlignment::Stretch,
                          huxerui::VerticalAlignment::Stretch));
}

// 手机端一级/二级页容器：二级页是 NavigationStack 的 push 目标，四个一级页是
// 其根页面，因此二级页覆盖底部导航，并保留一级页的滚动与查询状态。
[[huxerui::composable]] huxerui::View AndroidMainContent(
    huxerui::State<std::size_t> navPage, huxerui::State<std::size_t> pagerPage,
    huxerui::State<int> themeMode, const IslandTheme& islands,
    const huxerui::ThemeSpec& spec) {
    return huxerui::NavigationStack(AndroidPrimaryShell, navPage, pagerPage,
                                    themeMode, islands, spec)
        .With(huxerui::Grow(1.0F));
}
#endif

} // namespace

#if defined(__ANDROID__)
// 手机端二级页：由设置页「更多」入口 push 到 NavigationStack，标题栏返回箭头
// 与系统返回键统一调用 Pop，因此进入和返回都使用上面的页面动画。
[[huxerui::composable]] huxerui::View AndroidRulesPage() {
    const huxerui::NavigationController navigation = huxerui::UseNavigation();
    return RulesPage([navigation] { static_cast<void>(navigation.Pop()); })
        .With(SecondaryPageTransition(huxerui::UseTheme().motion));
}

[[huxerui::composable]] huxerui::View AndroidConnectionsPage() {
    const huxerui::NavigationController navigation = huxerui::UseNavigation();
    return ConnectionsPage([navigation] { static_cast<void>(navigation.Pop()); })
        .With(SecondaryPageTransition(huxerui::UseTheme().motion));
}

[[huxerui::composable]] huxerui::View AndroidLogsPage() {
    const huxerui::NavigationController navigation = huxerui::UseNavigation();
    return LogsPage([navigation] { static_cast<void>(navigation.Pop()); })
        .With(SecondaryPageTransition(huxerui::UseTheme().motion));
}
#endif

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

    // 持久化：启动任务打开 ORM 库并 hydrate settings/profiles 缓存，然后补齐
    // core.secret、把首帧默认主题校正为库里的值，最后长期跑 flush 泵
    // （settings 与 profiles 都是「写缓存 + 异步落库」）。旧结构由 open 删库重建。
    auto tasks = huxerui::UseTaskScope();
    huxerui::Lifecycle(
        [tasks, themeMode, application] {
            tasks.Launch([themeMode, application]() -> huxerui::Task<void> {
                try {
                    auto& db = clashflux::persistence::persistence();
                    if (!db.ready() && !co_await db.open(cfg::databaseFile())) {
                        // 旧版本数据库不兼容：只记录并降级运行，不删除任何文件。
                        stream::logApplication("error", db.lastError());
                        if (cli::runtimeCommandMode()) {
                            cli::setPendingExitCode(1);
                            application.Quit();
                            co_return;
                        }
                    }
                    store::coreStore().init();
                    store::coreStore().ensureSecret();
                    if (db.ready()) {
                        const std::string saved =
                            store::coreStore().setting("ui.theme_mode", "1");
                        if (saved == "0" || saved == "2") {
                            const int mode = std::stoi(saved);
                            if (mode != themeMode.Get()) themeMode = mode;
                        }
                    }

                    // 单实例：owner 启动转发服务；非 owner 的命令由这里代跑。
                    clashflux::cli_ipc::startCommandServer();

                    // 伪 CLI：命令在运行时内执行（窗口隐藏到托盘），落库后退出。
                    auto args = cli::takePendingCommand();
                    if (!args.empty()) {
                        const int code = co_await RunOnTaskThread(
                            [args = std::move(args)] { return cli::run(args); });
                        co_await db.flushSettings();
                        co_await db.flushProfiles();
                        cli::setPendingExitCode(code);
                        application.Quit();
                        co_return;
                    }

                    for (;;) {
                        co_await huxerui::Delay(std::chrono::duration<double>{0.25});
                        // 服务其他进程转发来的 CLI 命令（单实例下唯一执行点）。
                        co_await RunOnTaskThread(
                            [] { return clashflux::cli_ipc::servePendingCommands(); });
                        co_await db.flushSettings();
                        co_await db.flushProfiles();
                    }
                } catch (const std::exception& exception) {
                    // 任务里未捕获的异常会被 HuxerUI 直接 terminate 掉整个进程
                    // （TaskExecution::CompleteOnUi），所以必须在这里兜住并记录。
                    stream::logApplication(
                        "error",
                        std::string{"持久化启动任务异常："} + exception.what());
                } catch (...) {
                    stream::logApplication("error", "持久化启动任务未知异常");
                }
                // 兜底退出：CLI 模式必须给出确定退出码，不能再启动第二个运行时。
                if (cli::runtimeCommandMode()) {
                    cli::setPendingExitCode(1);
                    application.Quit();
                }
            });
            return [] {};
        },
        0);
    // 平台刷新泵和应用生命周期各自由平台组件收束，通用壳层只挂载它们。
    huxerui::View profileRefreshPump = CLASHFLUX_PROFILE_REFRESH_PUMP();

    // 主题派生（托盘 TUN 引导弹窗也要取 rootSpec 配色，故先于托盘块计算）。
    const bool dark =
        themeMode.Get() == 1 || (themeMode.Get() == 0 && cfg::systemPrefersDark());
    const huxerui::ThemeSpec rootSpec = dark ? FluxDarkThemeSpec() : FluxLightThemeSpec();
    const IslandTheme rootIslands = ResolveIslandTheme(rootSpec);

    huxerui::View applicationEffects =
        CLASHFLUX_APPLICATION_EFFECTS(application, rootSpec);
    huxerui::View mainRow = CLASHFLUX_MAIN_CONTENT(
        navPage, pagerPage, themeMode, rootIslands, rootSpec);
    huxerui::View content = CLASHFLUX_APP_CONTENT(std::move(mainRow), rootSpec);

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
#undef CLASHFLUX_MAIN_CONTENT
