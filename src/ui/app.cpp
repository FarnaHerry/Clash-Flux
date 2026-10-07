// app.cpp — 应用壳（桌面平面布局 + 自定义标题栏 + 托盘）：
//   桌面页名放在顶部标题栏；侧栏、标题栏与内容分区以细线分隔，不套卡片。
//   手机保留现有页面与卡片布局。品牌水蓝用于交互与选中态，中性灰用于大面积底色。
//   根节点刷整窗底色
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
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>
#include <variant>

#include "ui.h"
#include "app.h"
#include "responsive_shell.h"
#include "app_resources.h"
#include "proxies_model.h"
#include "task_bridge.h"

import clashflux.config;
import clashflux.cli;
import clashflux.cli_ipc;
import clashflux.core;
import clashflux.db;
import clashflux.store.core;
import clashflux.store.profiles;
import clashflux.store.vpn;
import clashflux.persistence;
import clashflux.stream;

// CoreView 含 store::CoreSnapshot、ProfilesModel 含 db::Profile，必须在模块导入
// 之后（同 profiles_cache.h）。
#include "core_model.h"
#include "profiles_model.h"
#include "settings_model.h"
#include "vpn_model.h"

#include "profiles_cache.h"

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

// 伪 CLI 与单实例命令转发只存在于桌面/CLI 形态：Android 没有 main()，也没有第二
// 个进程可以转发命令。平台差异在这里一次性收束成同名函数，composable 函数体内
// 只调用统一名字（composable 体内不能出现条件编译）。
#if defined(__ANDROID__)
namespace {

bool CliRuntimeCommandMode() { return false; }

void CliSetPendingExitCode(int) {}

void CliStartCommandServer() {}

std::vector<std::string> CliTakePendingCommand() { return {}; }

int CliRun(const std::vector<std::string>&) { return 0; }

} // namespace
#else
namespace {

bool CliRuntimeCommandMode() { return cli::runtimeCommandMode(); }

void CliSetPendingExitCode(int code) { cli::setPendingExitCode(code); }

void CliStartCommandServer() { clashflux::cli_ipc::startCommandServer(); }

std::vector<std::string> CliTakePendingCommand() {
    return cli::takePendingCommand();
}

int CliRun(const std::vector<std::string>& args) { return cli::run(args); }

} // namespace
#endif

// 品牌蓝只出现在主交互和选中态。页面、卡片、导航与浮层使用中性灰阶，
// 通过稳定的明度差区分层级，避免整页被蓝色表面染色。
huxerui::ThemeSpec FluxDarkThemeSpec(std::string_view accent) {
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
    spec.colors = FluxDarkColors(accent);
    spec.interactions.focus_ring = huxerui::FocusRing{spec.colors.on_primary_container, 2.0F, 2.0F};
    return spec;
}

// 浅色模式用干净的中性白灰做底，主色容器只保留浅水蓝色调；正文和边界使用
// 石墨灰，保证内容层级清晰，品牌色不会扩散到大面积背景。
huxerui::ThemeSpec FluxLightThemeSpec(std::string_view accent) {
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
    spec.colors = FluxLightColors(accent);
    spec.interactions.focus_ring = huxerui::FocusRing{ResolveFluxAccent(accent).dark, 2.0F, 2.0F};
    return spec;
}

// 主题边界：MaterialThemeDefinition(spec) 之上用 typed style 覆盖组件样式——
// 按钮/分段按钮/菜单圆角统一 8px（M3 默认全圆胶囊），叠加层用 on_surface
// 半透明，让深浅模式的交互反馈都留在品牌色相内。
huxerui::View FluxThemed(const huxerui::ThemeSpec& spec, huxerui::View content) {
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
    selects.popup_shadow = huxerui::Shadow{ThemeShadowColor(spec, 0.24F), {}, 8.0F, 0.0F};
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
    menus.shadow = huxerui::Shadow{ThemeShadowColor(spec, 0.24F), {}, 8.0F, 0.0F};
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
    navigationPane.compact_width = kTopNavigationRailWidth;
    navigationPane.expanded_min_width = 220.0F;
    navigationPane.item_height = kTopNavigationItemHeight;
    navigationPane.item_margin = huxerui::EdgeInsets::Symmetric(
        0.0F, kTopNavigationItemVerticalMargin);
    navigationPane.compact_indicator_size =
        huxerui::Size{kTopNavigationIndicatorSize, kTopNavigationIndicatorSize};
    navigationPane.indicator_corner_radius = spec.shapes.medium;
    huxerui::Color navigationHover = spec.colors.on_surface;
    navigationHover.alpha = 0.08F;
    huxerui::Color navigationPress = spec.colors.on_surface;
    navigationPress.alpha = 0.14F;
    navigationPane.indication = huxerui::Indication{
        .geometry = huxerui::IndicationGeometry{
            .layer_size = navigationPane.compact_indicator_size,
            .clip_corner_radii =
                huxerui::CornerRadii{navigationPane.indicator_corner_radius},
        },
        .hover = huxerui::IndicationLayer{.fill = navigationHover},
        .press = huxerui::IndicationLayer{.fill = navigationPress},
    };
    definition.Set(navigationPane);

    // Android 的主题级回退也必须跟随页面壳层的内容底色；否则状态栏候选
    // 暂不可见的过渡帧会露出 Material 默认白色。桌面仍沿用 background。
    huxerui::SystemBarsAppearance systemBars =
        huxerui::SystemBarsAppearance::Default();
#if defined(__ANDROID__)
    const huxerui::Color pageBackground = ResolveIslandTheme(spec).base;
    systemBars.status_bar_background = pageBackground;
    systemBars.navigation_bar_background = pageBackground;
#else
    systemBars.status_bar_background = spec.colors.background;
    systemBars.navigation_bar_background = spec.colors.background;
#endif
    definition.Set(systemBars);

    return huxerui::Theme(std::move(definition), content);
}

// 桌面与手机共用图标定义；选中只改变内容色和指示器，不替换图标轮廓。
struct NavigationEntry {
    huxerui::ImageResource icon;
    huxerui::StringVariant label;
};

const std::array<NavigationEntry, 7> kNavigationEntries{
    NavigationEntry{app::images::home, Localized("首页")},
    NavigationEntry{app::images::request, Localized("订阅")},
    NavigationEntry{app::images::proxies, Localized("代理")},
    NavigationEntry{app::images::route, Localized("规则")},
    NavigationEntry{app::images::connections, Localized("连接")},
    NavigationEntry{app::images::logs, Localized("日志")},
    NavigationEntry{app::images::gear, Localized("设置")},
};

std::vector<huxerui::NavigationItem> DesktopNavigationItems() {
    std::vector<huxerui::NavigationItem> destinations;
    destinations.reserve(kNavigationEntries.size());
    for (const NavigationEntry& entry : kNavigationEntries) {
        destinations.push_back(huxerui::NavigationItem(entry.icon, entry.label));
    }
    return destinations;
}

// Android 仅暴露四个一级页；规则、连接和日志由设置页的“更多”入口承载。
constexpr std::array<std::size_t, 4> kAndroidNavDestinations{
    pages::kHome, pages::kProxies, pages::kProfiles, pages::kSettings};

struct AndroidNavigationIndicator {
    class Extension;

    std::size_t selected_index = 0;
    huxerui::Color fill = huxerui::Color::Transparent();
    huxerui::AnimationSpec animation = huxerui::TweenSpec{0.2};
    bool reduced_motion = false;

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
        animation_ = spec.animation;
        reduced_motion_ = spec.reduced_motion;
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
        const float width = std::clamp(selected.LayoutSize().width - 12.0F,
                                       0.0F, 76.0F);
        const float y = selected.LayoutOffset().y +
                        (selected.LayoutSize().height - 50.0F) * 0.5F;
        const float target = selected.LayoutOffset().x +
                             (selected.LayoutSize().width - width) *
                                 0.5F;
        const bool resized = viewport_width_ != node.LayoutSize().width;
        const bool shape_changed = indicator_width_ != width || indicator_y_ != y;
        viewport_width_ = node.LayoutSize().width;
        indicator_width_ = width;
        indicator_y_ = y;
        geometry_pending_ = false;
        // 首帧、缩放和减少动态效果时直接定位，避免旧宽度的轨道越过新槽位。
        if (!geometry_initialized_ || resized || reduced_motion_) {
            const bool changed = !geometry_initialized_ || shape_changed ||
                                 offset_.Value() != target;
            geometry_initialized_ = true;
            offset_.Set(target);
            return changed ? PaintInvalidation::Content : PaintInvalidation::None;
        }
        if (offset_.Target() == target) {
            return shape_changed ? PaintInvalidation::Content : PaintInvalidation::None;
        }
        offset_.AnimateTo(target, animation_);
        return PaintInvalidation::Content;
    }

    void PaintBehindContent(const huxerui::ViewNode& node,
                            huxerui::PaintContext& context) const override {
        if (!geometry_initialized_ || fill_.alpha <= 0.0F) return;
        context.DrawRect(
            huxerui::Rect{offset_.Value(), indicator_y_, indicator_width_, 50.0F},
            fill_, 25.0F);
    }

private:
    std::size_t selected_index_ = 0;
    huxerui::Color fill_ = huxerui::Color::Transparent();
    huxerui::MotionController offset_;
    huxerui::AnimationSpec animation_ = huxerui::TweenSpec{0.2};
    bool reduced_motion_ = false;
    float viewport_width_ = 0.0F;
    float indicator_width_ = 0.0F;
    float indicator_y_ = 0.0F;
    bool initialized_ = false;
    bool geometry_initialized_ = false;
    bool geometry_pending_ = false;
};

// 桌面一级导航始终使用无底板的紧凑图标栏；文字保留为语义名称。
[[huxerui::composable]] huxerui::View DesktopNavigationSurface(
    huxerui::State<std::size_t> navPage) {
    const std::vector<huxerui::NavigationItem> items = DesktopNavigationItems();
    const auto onChanged = [navPage](std::size_t index) { navPage = index; };
    return huxerui::NavigationPane(items, navPage, false).OnChanged(onChanged)
        .With(huxerui::Padding(huxerui::EdgeInsets{.top = kDesktopTopContentGap}));
}

// 底部导航：选中态把 icon 与文字作为一个整体包进胶囊，而不是只高亮 icon。
// 内建 NavigationBar 的指示器只覆盖图标层，所以这里自绘条目；同时保留底部
// 安全区消费与系统导航栏底色，行为对齐原先的内建组件。
[[huxerui::composable]] huxerui::View AndroidNavigationSurface(
    huxerui::State<std::size_t> navPage, bool fromSwipe = false,
    std::function<void()> onSelect = {}) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const std::size_t selected = navPage.Get() == pages::kHome
        ? 0U
        : navPage.Get() == pages::kProxies ? 1U
        : navPage.Get() == pages::kProfiles ? 2U : 3U;

    std::vector<huxerui::View> entries;
    entries.reserve(kAndroidNavDestinations.size());
    const double duration = theme.motion.reduced_motion ? 0.0 : 0.2;
    const huxerui::AnimationSpec indicatorMotion = fromSwipe
        ? huxerui::AnimationSpec(huxerui::TweenSpec{0.2, huxerui::Easing::EaseOut})
        : huxerui::AnimationSpec(huxerui::SpringSpec{
              .stiffness = 240.0F, .damping_ratio = 0.78F});

    for (std::size_t index = 0; index < kAndroidNavDestinations.size(); ++index) {
        const std::size_t destination = kAndroidNavDestinations[index];
        const NavigationEntry& entry = kNavigationEntries[destination];
        const bool isSelected = index == selected;
        // Tint/文字色没有动画值接口：保留普通色底层，仅渐变选中色覆盖层。
        // 动画是框架的呈现修饰符，不逐帧写 State，也不参与槽位布局。
        const auto content = [entry](huxerui::Color color) {
            return huxerui::Column {
                huxerui::Image(entry.icon)
                    .Tint(color)
                    .With(huxerui::Frame{.width = 20.0F, .height = 20.0F}),
                huxerui::Text(entry.label).Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption), color})
                    .Align(huxerui::TextAlign::Center),
            }.With(huxerui::Spacing(2.0F),
                   huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
        };
        entries.push_back(
            huxerui::Stack {
                content(theme.colors.on_surface_variant),
                content(theme.colors.on_primary_container)
                    .With(huxerui::Opacity(huxerui::AnimateTo(
                        isSelected ? 1.0F : 0.0F,
                        huxerui::TweenSpec{duration, huxerui::Easing::EaseOut}))),
            }.With(huxerui::Grow(1.0F),
                      huxerui::Frame{.height = 64.0F},
                      huxerui::Padding(
                          huxerui::EdgeInsets::Symmetric(2.0F, 0.0F)),
                      huxerui::Align(huxerui::HorizontalAlignment::Center,
                                     huxerui::VerticalAlignment::Center))
                .OnClick([navPage, destination, onSelect] {
                    if (onSelect) onSelect();
                    navPage = destination;
                })
                .With(huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                         .label = huxerui::UseString(entry.label),
                                         .selected = isSelected},
                      // 显式覆盖 OnClick 的默认指示层，保留纯选中态过渡。
                      huxerui::Indication{},
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
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                  AndroidNavigationIndicator{selected,
                                             theme.colors.primary_container,
                                             indicatorMotion,
                                             theme.motion.reduced_motion}),
    }.With(huxerui::SafeAreaPadding{.top = false},
           systemBars,
           huxerui::Semantics{.role = huxerui::SemanticRole::Navigation},
           huxerui::Background(CompactNavigationSurfaceColor(theme)),
           huxerui::CornerRadius(20.0F), huxerui::ClipChildren(),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// CLI 请求服务：阻塞的 serve 放到任务线程，完成后请订阅模型跟上（CLI 可能
// 增删/切换订阅）。事件路径（Linux inotify）与兜底轮询共用本函数。
huxerui::Task<void> ServeCliRequests(std::shared_ptr<ProfilesModel> profilesModel) {
#if defined(__ANDROID__)
    // Android 没有 CLI 转发通道（clashflux.cli_ipc 不在 legacy 接口集合里），
    // 平台边界收在这里，composable 体内保持无条件编译。
    static_cast<void>(profilesModel);
    co_return;
#else
    const int served = co_await RunOnTaskThread(
        [] { return clashflux::cli_ipc::servePendingCommands(); });
    if (served > 0) profilesModel->RequestSync();
#endif
}

// 启动 CLI 请求监视（Linux 事件驱动；其他平台 no-op）。
// 平台差异收在函数边界：Android 编译不到 cli_ipc，非 Linux 没有 inotify。
void StartCliRequestWatcher(huxerui::TaskScope tasks,
                            std::shared_ptr<ProfilesModel> profilesModel) {
#if defined(__ANDROID__) || !defined(__linux__)
    static_cast<void>(tasks);
    static_cast<void>(profilesModel);
#else
    clashflux::cli_ipc::startRequestWatcher([tasks, profilesModel] {
        // 后台线程：只 Post 回 UI 线程（scope 关闭后 Post 被忽略）。
        tasks.Post([tasks, profilesModel] {
            tasks.Launch([profilesModel]() -> huxerui::Task<void> {
                co_await ServeCliRequests(profilesModel);
            });
        });
    });
#endif
}

void QueueProfileActivation(const huxerui::ApplicationActivation& activation,
                            const std::shared_ptr<ProfilesModel>& profiles) {
    const auto* payload = std::get_if<huxerui::UrlActivation>(&activation);
    if (!payload) return;
    const auto& uri = payload->url;
    if (!profile_link::IsSupportedScheme(uri.Scheme())) return;
    std::string error;
    auto profile = profile_link::ParseParts(uri.Scheme(), uri.Authority().value_or(""),
        uri.Path(), uri.Query().value_or(""), uri.Fragment().value_or(""), error);
    if (!profile || !profile_link::Submit(std::move(*profile), error))
        profiles->linkError = error;
}

// 桌面主内容保留七个一级页面；二级设置只覆盖这个内容槽。
[[huxerui::composable]] huxerui::View DesktopPrimaryPages(
    huxerui::State<std::size_t> navPage, huxerui::State<int> themeMode,
    ProfilesCache profilesCache) {
    const auto navigation = huxerui::UseNavigation();
    const auto model = huxerui::UseEnvironment<DesktopSettingsNavigation>().model;
    huxerui::Lifecycle([navPage, navigation, model] {
        if (model->secondaryOpen.Get() && navPage.Get() != pages::kSettings) {
            static_cast<void>(navigation.Pop());
        }
        return [] {};
    }, navPage);
    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    const auto backToSettings = compact ? std::function<void()>{[navPage] { navPage = pages::kSettings; }}
                                        : std::function<void()>{};
    std::vector<huxerui::View> pages;
    pages.reserve(7);
    // 不可见的一级页仍挂载（保住 State/Lifecycle）但不构建内容：IndexedPages 让
    // 所有页同帧参与测量，隐藏页的重子树会拖慢每一次渲染。
    const std::size_t desktopActivePage = navPage.Get();
    pages.push_back(HomePage(navPage, desktopActivePage == pages::kHome)
                        .Key("home").With(huxerui::Grow(1.0F)));
    pages.push_back(ProfilesPage(profilesCache, desktopActivePage == pages::kProfiles)
                        .Key("profiles").With(huxerui::Grow(1.0F)));
    pages.push_back(ProxiesPage(desktopActivePage == pages::kProxies)
                        .Key("proxies").With(huxerui::Grow(1.0F)));
    pages.push_back(RulesPage(profilesCache, backToSettings,
                              desktopActivePage == pages::kRules)
                        .Key("rules").With(huxerui::Grow(1.0F)));
    pages.push_back(ConnectionsPage(backToSettings, desktopActivePage == pages::kConnections)
                        .Key("connections").With(huxerui::Grow(1.0F)));
    pages.push_back(LogsPage(backToSettings, desktopActivePage == pages::kLogs)
                        .Key("logs").With(huxerui::Grow(1.0F)));
    pages.push_back(SettingsPage(themeMode, navPage, profilesCache,
                                 desktopActivePage == pages::kSettings)
                        .Key("settings").With(huxerui::Grow(1.0F)));
    return huxerui::IndexedPages(std::move(pages), navPage.Get()).With(huxerui::Grow(1.0F));
}

// 导航栈只占内容区域，左侧栏和 Compact 系统标题栏留在持久外壳中。
[[huxerui::composable]] huxerui::View DesktopMainContent(
    huxerui::State<std::size_t> navPage, huxerui::State<std::size_t>,
    huxerui::State<int> themeMode, const IslandTheme& islands,
    const huxerui::ThemeSpec& spec, ProfilesCache profilesCache) {
    static_cast<void>(islands);
    const auto model = huxerui::UseState(std::make_shared<DesktopSettingsNavigationModel>()).Get();
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const auto coreModel = huxerui::UseService<CoreModel>();
    const CoreView coreView = coreModel->view.Get();
    huxerui::Color coreStatusColor = theme.colors.on_surface_variant;
    huxerui::StringVariant coreStatusLabel = Localized("内核已停止");
    switch (coreView.core.state) {
    case core::CoreState::Running:
        coreStatusColor = theme.colors.primary;
        coreStatusLabel = LocalizedFormat(
            "内核运行中 · {}", coreView.core.version.empty()
                                  ? std::string{"sing-box"}
                                  : coreView.core.version);
        break;
    case core::CoreState::Starting:
        coreStatusColor = SemanticWarningColor(theme);
        coreStatusLabel = Localized("内核启动中");
        break;
    case core::CoreState::Failed:
        coreStatusColor = theme.colors.error;
        coreStatusLabel = Localized("内核启动失败");
        break;
    case core::CoreState::Stopped:
        break;
    }
    const std::size_t desktopActivePage = navPage.Get();
    huxerui::View logoArt = huxerui::Image(
        IsDarkTheme(spec) ? app::images::clash_flux_logo_vector_dark_refined
                         : app::images::clash_flux_logo_vector_light_refined)
        .Fit(huxerui::ImageFit::Contain)
        .With(huxerui::Frame{.width = 32.0F, .height = 32.0F});
    huxerui::View coreStatusDot = huxerui::Stack{}.With(
        huxerui::Frame{.width = 9.0F, .height = 9.0F},
        huxerui::Background(coreStatusColor),
        huxerui::Border{theme.colors.background, 1.5F},
        huxerui::CornerRadius(4.5F));
    huxerui::View logoStatusPlacement = huxerui::Column {
        huxerui::Row {
            huxerui::Spacer(),
            std::move(coreStatusDot),
        }.With(huxerui::Frame{.height = 11.0F},
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Spacer(),
    }.With(huxerui::Frame{.width = 32.0F, .height = 32.0F},
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    huxerui::View sidebarLogo = huxerui::Stack {
        std::move(logoArt),
        std::move(logoStatusPlacement),
    }.With(huxerui::Frame{.width = 32.0F, .height = 32.0F},
           huxerui::Tooltip(coreStatusLabel),
           huxerui::Semantics{.role = huxerui::SemanticRole::Image,
                              .label = huxerui::UseString(coreStatusLabel)})
        .Key("desktop-brand-logo");
    huxerui::View sidebar = huxerui::Row {
        huxerui::Column{
            huxerui::Row{
                std::move(sidebarLogo),
            }.With(huxerui::Frame{.height = kDesktopTitleBarHeight},
                   huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                   huxerui::WindowDragRegion{}),
            DesktopNavigationSurface(navPage).With(huxerui::Grow(1.0F)),
        }.With(huxerui::Frame{.width = kTopNavigationRailWidth},
               huxerui::Spacing(0.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
        huxerui::Row{}.With(huxerui::Frame{.width = 1.0F},
                            huxerui::Background(spec.colors.outline)),
    }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    huxerui::View chrome = huxerui::WindowTitleBar{
        huxerui::Text("Clash-Flux").With(huxerui::Padding(8.0F)),
        huxerui::Spacer{}.With(huxerui::Grow(1.0F)),
    }.With(huxerui::Frame{.height = kDesktopTitleBarHeight});
    const bool secondary = desktopActivePage == pages::kRules ||
        desktopActivePage == pages::kConnections || desktopActivePage == pages::kLogs ||
        model->secondaryOpen.Get();
    huxerui::View navigation;
    if (!secondary) navigation = AndroidNavigationSurface(navPage);
    // Environment 只提供作用域；外层真实布局承接骨架的 Grow/Key 行为。
    huxerui::View content = huxerui::Column {
      huxerui::ProvideEnvironment(DesktopSettingsNavigation{model},
          huxerui::NavigationStack(DesktopPrimaryPages, navPage, themeMode, profilesCache)
              .With(huxerui::Grow(1.0F))),
    }.With(huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    return ResponsiveDesktopShell(content, sidebar, chrome, navigation);
}

// 二级页的进入与返回动画：新页自右缘滑入、父页左移 20%，
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

#if defined(__ANDROID__)
// 一级外壳：四个一级页由 Pager 承载，悬浮 dock 与一级页同属 NavigationStack
// 的根页面；二级页 push 后整页覆盖 dock，弹出后一级页状态原样保留。
[[huxerui::composable]] huxerui::View AndroidPrimaryShell(
    huxerui::State<std::size_t> navPage, huxerui::State<std::size_t> pagerPage,
    huxerui::State<int> themeMode, const IslandTheme& islands,
    const huxerui::ThemeSpec& spec, ProfilesCache profilesCache) {
    auto fromSwipe = huxerui::UseState(false);
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

    // 只有当前显示的槽位构建内容：Pager 会把四个一级页同时挂载，隐藏页的重子树
    // （节点网格/订阅列表/设置项）即使不重组也会被逐帧重新测量，真机上这是代理页
    // 卡顿的主因（实测每帧 1443→28 次测量请求，MeasureStage 20ms→0ms）。
    const std::size_t activePagerPage = pagerPage.Get();
    std::vector<huxerui::View> primaryPages;
    primaryPages.reserve(4);
    primaryPages.push_back(HomePage(navPage, activePagerPage == 0)
                               .Key("home").With(huxerui::Grow(1.0F)));
    primaryPages.push_back(
        ProxiesPage(activePagerPage == 1).Key("proxies").With(huxerui::Grow(1.0F)));
    primaryPages.push_back(
        AndroidProfilesPage(huxerui::UseNavigation(), profilesCache,
                            activePagerPage == 2)
            .Key("profiles").With(huxerui::Grow(1.0F)));
    primaryPages.push_back(SettingsPage(themeMode, navPage, profilesCache,
                                        activePagerPage == 3)
                               .Key("settings").With(huxerui::Grow(1.0F)));

    huxerui::View pager =
        huxerui::Pager(std::move(primaryPages), pagerPage)
        .ScrollAxis(huxerui::Axis::Horizontal)
        .DragEnabled(true)
        .OnChanged([navPage, pagerPage, fromSwipe](std::size_t index) {
            constexpr std::array<std::size_t, 4> kDestinations{
                pages::kHome, pages::kProxies, pages::kProfiles, pages::kSettings};
            const std::size_t clamped =
                std::min(index, kDestinations.size() - 1);
            // 只有真正改变目标的 Pager 提交才来自横滑；同页通知不改点击的弹簧轨道。
            if (navPage.Get() != kDestinations[clamped]) fromSwipe = true;
            pagerPage = clamped;
            navPage = kDestinations[clamped];
        })
        .With(huxerui::Grow(1.0F));

    // 只给胶囊底色添加 Alpha；不叠加不透明外层，也不淡化文字和图标。
    huxerui::View dock = CompactNavigationDock(
        AndroidNavigationSurface(navPage, fromSwipe.Get(), [fromSwipe] { fromSwipe = false; }), spec);
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
    const huxerui::ThemeSpec& spec, ProfilesCache profilesCache) {
    return huxerui::NavigationStack(AndroidPrimaryShell, navPage, pagerPage,
                                    themeMode, islands, spec, profilesCache)
        .With(huxerui::Grow(1.0F));
}
#endif

} // namespace

#if defined(__ANDROID__)
// 手机端二级页：由设置页「更多」入口 push 到 NavigationStack，标题栏返回箭头
// 与系统返回键统一调用 Pop，因此进入和返回都使用上面的页面动画。
[[huxerui::composable]] huxerui::View AndroidRulesPage(
    ProfilesCache profilesCache) {
    const huxerui::NavigationController navigation = huxerui::UseNavigation();
    return RulesPage(profilesCache,
                     [navigation] { static_cast<void>(navigation.Pop()); })
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

[[huxerui::composable]] huxerui::View AndroidLanguagePage() {
    const huxerui::NavigationController navigation = huxerui::UseNavigation();
    return LanguagePage([navigation] { static_cast<void>(navigation.Pop()); })
        .With(SecondaryPageTransition(huxerui::UseTheme().motion));
}

[[huxerui::composable]] huxerui::View AndroidThemePage(huxerui::State<int> themeMode) {
    const huxerui::NavigationController navigation = huxerui::UseNavigation();
    return ThemePage(themeMode, [navigation] { static_cast<void>(navigation.Pop()); })
        .With(SecondaryPageTransition(huxerui::UseTheme().motion));
}
#endif

#if !defined(__ANDROID__)
[[huxerui::composable]] huxerui::View DesktopThemePage(huxerui::State<int> themeMode) {
    const huxerui::NavigationController navigation = huxerui::UseNavigation();
    const auto model = huxerui::UseEnvironment<DesktopSettingsNavigation>().model;
    huxerui::Lifecycle([model] {
        return [model] { model->secondaryOpen = false; };
    }, 0);
    return ThemePage(themeMode, [navigation] { static_cast<void>(navigation.Pop()); }, true)
        .With(SecondaryPageTransition(huxerui::UseTheme().motion));
}
#endif

void InstallProfileLinkActivation(huxerui::ApplicationContext& context,
                                 std::shared_ptr<ProfilesModel> profiles) {
    context.OnActivation([profiles](huxerui::ApplicationActivation activation) {
        QueueProfileActivation(activation, profiles);
    });
}

[[huxerui::composable]] huxerui::View AppRoot() {
    const huxerui::ApplicationHandle application = huxerui::UseApplication();
    const huxerui::Locale systemLocale =
        huxerui::UseEnvironment<huxerui::Locale>();
    CLASHFLUX_PREPARE_PLATFORM_DATA(application);

    // 阻塞首帧构建，集中读取首帧外观和启动副作用所需配置。快照只取一次，
    // 不在重组时读磁盘；临时连接只读，设置/订阅的 ORM hydrate 仍走异步流程。
    static const auto startupSettings =
        clashflux::persistence::readStartupSettings(cfg::databaseFile());
    if (!startupSettings.error.empty()) {
        std::fprintf(stderr, "Clash-Flux 启动失败：%s；原数据库与订阅文件已保留\n", startupSettings.error.c_str());
        CliSetPendingExitCode(1);
        application.Quit();
        return huxerui::Row{};
    }
    auto themeMode = huxerui::UseState<int>(int{startupSettings.themeMode});
    // TEMP-PERF-ONLY: 直接落到指定一级页做帧分析，测完删除。
    std::size_t initialNavPage = pages::kHome;
    if (const char* perf_page = std::getenv("CLASHFLUX_PERF_PAGE")) {
        initialNavPage = static_cast<std::size_t>(std::atoi(perf_page));
    }
    auto navPage = huxerui::UseState<std::size_t>(std::move(initialNavPage));
    auto pagerPage = huxerui::UseState<std::size_t>(0);
    // 订阅列表：模型是唯一来源，页面拿到的 StateList 只是它的镜像；
    // 乐观选中标记与模型共享同一个 State（见 profiles_model.h）。
    const auto profilesModel = huxerui::UseService<ProfilesModel>();
    // 设置：hydrate 完成后要立刻请它重读一次（首帧组合早于 hydrate，见
    // settings_model.h），所以在这里就取好，供下面的启动任务使用。
    const auto settingsModel = huxerui::UseService<SettingsModel>();
    huxerui::Lifecycle(
        [themeMode, settingsModel] {
            const SettingsView settings = settingsModel->view.Get();
            if (settings.ready && themeMode.Get() != settings.themeMode) {
                themeMode = settings.themeMode;
            }
            return [] {};
        },
        settingsModel->view);
    auto profilesCacheList = huxerui::UseStateList<db::Profile>();
    const ProfilesCache profilesCache{profilesCacheList,
                                      profilesModel->selectionPending};

    // 持久化：启动任务打开 ORM 库并 hydrate settings/profiles 缓存，然后补齐
    // core.secret，最后长期跑 flush 泵
    // （settings 与 profiles 都是「写缓存 + 异步落库」）。user_version=0 的老库
    // 由 open 里的 0→1 迁移重建表并保留数据。
    auto tasks = huxerui::UseTaskScope();
    const auto linkToast = huxerui::UseToast();
    const auto linkWindow = huxerui::UseWindow();
    huxerui::Lifecycle([application, profilesModel] {
        if (const auto& startup = application.StartupActivation(); startup)
            QueueProfileActivation(*startup, profilesModel);
        return [] {};
    }, 0);
    huxerui::Lifecycle([profilesModel, linkToast] {
        const auto error = profilesModel->linkError.Get();
        if (!error.empty()) {
            linkToast.Show(error);
            profilesModel->linkError = std::string{};
        }
        return [] {};
    }, profilesModel->linkError);
    huxerui::Lifecycle([tasks, profilesModel, navPage, pagerPage, linkWindow, linkToast] {
        profile_link::SetWakeHandler([tasks, profilesModel, navPage, pagerPage, linkWindow, linkToast] {
            tasks.Post([profilesModel, navPage, pagerPage, linkWindow, linkToast] {
                auto incoming = profile_link::TakePending();
                if (incoming.empty()) return;
                auto links = profilesModel->importLinks.Get();
                for (auto& request : incoming) {
                    if (links.size() >= 16) {
                        linkToast.Show("待处理的订阅链接过多，请先完成导入");
                        break;
                    }
                    links.push_back(std::move(request));
                }
                profilesModel->importLinks = std::move(links);
                navPage = pages::kProfiles;
                pagerPage = 2;
                linkWindow.Activate();
            });
        });
        return [] { profile_link::SetWakeHandler({}); };
    }, 0);
    huxerui::Lifecycle(
        [tasks, application, profilesModel, settingsModel] {
            tasks.Launch([application, profilesModel,
                          settingsModel]() -> huxerui::Task<void> {
                try {
                    auto& db = clashflux::persistence::persistence();
                    if (!db.ready() && !co_await db.open(cfg::databaseFile(), cfg::profilesDir())) {
                        // A failed hydrate must never initialize an empty
                        // writable session or allocate IDs over existing files.
                        stream::logApplication("error", db.lastError());
                        std::fprintf(stderr, "Clash-Flux 启动失败：%s；原数据库与订阅文件已保留\n", db.lastError().c_str());
                        CliSetPendingExitCode(1);
                        application.Quit();
                        co_return;
                    }
                    store::coreStore().init();
                    store::coreStore().ensureSecret();
                    if (db.ready()) {
                        // hydrate advances the content revision; explicitly
                        // request the initial model publication as well.
                        profilesModel->RequestSync();
                        // 设置同理：让依赖「hydrate 完成后补读」的消费者（首页布局、
                        // 环境 shell 选择）立刻拿到库里的值，而不是等下一拍。
                        settingsModel->RequestSync();
                    }

                    // 单实例：owner 启动转发服务；非 owner 的命令由这里代跑。
                    CliStartCommandServer();

                    // 伪 CLI：命令在运行时内执行（窗口隐藏到托盘），落库后退出。
                    auto args = CliTakePendingCommand();
                    if (!args.empty()) {
                        int code = co_await RunOnTaskThread(
                            [args = std::move(args)] { return CliRun(args); });
                        const bool settingsSaved = co_await db.flushSettings();
                        const bool profilesSaved = co_await db.flushProfiles();
                        if (!settingsSaved || !profilesSaved) {
                            std::fprintf(stderr, "Clash-Flux 命令落库失败：%s\n", db.lastError().c_str());
                            code = 1;
                        }
                        CliSetPendingExitCode(code);
                        application.Quit();
                        co_return;
                    }

                    // 落库失败必须可见：持久化降级（库打不开）时 flush* 会返回
                    // false，以前这里直接丢弃返回值，用户直到重启丢数据才发现。
                    bool storageFailureReported = false;
                    for (;;) {
                        // 兜底节拍：Linux 的主路径是 inotify（见 startRequestWatcher），
                        // 非 Linux 平台这里是唯一路径。1s 足够，不必 4Hz 扫目录。
                        co_await huxerui::Delay(std::chrono::duration<double>{1.0});
                        // 服务其他进程转发来的 CLI 命令（单实例下唯一执行点）。
                        co_await ServeCliRequests(profilesModel);
                        const bool settingsFlushed = co_await db.flushSettings();
                        const bool profilesFlushed = co_await db.flushProfiles();
                        if (settingsFlushed && profilesFlushed) {
                            storageFailureReported = false;
                        } else if (!storageFailureReported) {
                            storageFailureReported = true;
                            stream::logApplication(
                                "error",
                                "设置/订阅未能落库：" +
                                    (db.lastError().empty()
                                         ? std::string{"未知原因"}
                                         : db.lastError()));
                        }
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
                // Startup exceptions fail closed in GUI and CLI alike.
                CliSetPendingExitCode(1);
                application.Quit();
            });
            return [] {};
        },
        0);
    // 订阅列表：唯一来源是 ProfilesModel（见 profiles_model.h）；页面用的
    // StateList 是它的镜像——模型一变就同步一次，不再是 2s 全量泵。
    huxerui::Lifecycle(
        [profilesCache, profilesModel] {
            ReplaceStateList(profilesCache.list, profilesModel->list.Get());
            return [] {};
        },
        profilesModel->list);
    // 唯一的数据泵：1s 修订号脏检查（变了才拷贝），显式 RequestSync 时立刻同步。
    huxerui::Lifecycle(
        [tasks, profilesModel] {
            DriveProfilesModel(tasks, profilesModel);
            return [] {};
        },
        0);

    // CLI 请求目录监视：Linux 上 inotify 事件驱动——新请求一到立刻服务，不再
    // 靠 0.25s 扫目录；其他平台该调用是 no-op，由下面的兜底轮询负责。
    huxerui::Lifecycle(
        [tasks, profilesModel] {
            StartCliRequestWatcher(tasks, profilesModel);
            return [] {};
        },
        0);

    // 平台刷新泵和应用生命周期各自由平台组件收束，通用壳层只挂载它们。
    huxerui::View profileRefreshPump = CLASHFLUX_PROFILE_REFRESH_PUMP();

    // 策略组快照的唯一数据泵：首页卡片、代理页、托盘菜单都只读 ProxiesModel
    // 的 State，不再各自拉 /proxies（此前是 2s/3s/1s 三条独立轮询 + 三次解析）。
    const auto proxiesModel = huxerui::UseService<ProxiesModel>();
    huxerui::Lifecycle(
        [tasks, proxiesModel] {
            DriveProxiesModel(tasks, proxiesModel);
            return [] {};
        },
        0);

    // 内核/接管状态的唯一数据泵：首页三张桌面卡片、代理页、设置页、内核设置段与
    // 托盘菜单都只读 CoreModel 的 State，不再各自读 coreStore()（此前是
    // 0.5s×4 + 1s×2 六条轮询，托盘那条还带 2Hz 的子进程/HTTP 探测）。
    const auto coreModel = huxerui::UseService<CoreModel>();
    huxerui::Lifecycle(
        [tasks, coreModel] {
            DriveCoreModel(tasks, coreModel);
            return [] {};
        },
        0);

    // 原生连接（PPTP/OpenVPN）状态的唯一数据泵：订阅页不再自建 0.5s 泵。
    const auto vpnModel = huxerui::UseService<VpnModel>();
    huxerui::Lifecycle(
        [tasks, vpnModel] {
            DriveVpnModel(tasks, vpnModel);
            return [] {};
        },
        0);

    // 应用级设置（KV）的唯一读取点：组合期不再直接 setting(...)，否则首帧会拿到
    // hydrate 之前的默认值（见 settings_model.h）。
    huxerui::Lifecycle(
        [tasks, settingsModel] {
            DriveSettingsModel(tasks, settingsModel);
            return [] {};
        },
        0);

    // 主题派生（托盘 TUN 引导弹窗也要取 rootSpec 配色，故先于托盘块计算）。
    const bool dark =
        themeMode.Get() == 1 || (themeMode.Get() == 0 && cfg::systemPrefersDark());
    const SettingsView settings = settingsModel->view.Get();
    const std::string& accent = settings.ready ? settings.themeColor : startupSettings.themeColor;
    const huxerui::ThemeSpec rootSpec = dark ? FluxDarkThemeSpec(accent) : FluxLightThemeSpec(accent);
    const IslandTheme rootIslands = ResolveIslandTheme(rootSpec);
    const std::string& language = settings.ready ? settings.language : startupSettings.language;
    const huxerui::Locale locale =
        language == "zh" ? huxerui::Locale::FromLanguageTag("zh")
        : language == "en" ? huxerui::Locale::FromLanguageTag("en")
                            : systemLocale;

    huxerui::View applicationEffects =
        CLASHFLUX_APPLICATION_EFFECTS(application, rootSpec,
                                      startupSettings.trayEnabled,
                                      startupSettings.startMinimized);
    huxerui::View mainRow = CLASHFLUX_MAIN_CONTENT(
        navPage, pagerPage, themeMode, rootIslands, rootSpec, profilesCache);
    huxerui::View content = CLASHFLUX_APP_CONTENT(std::move(mainRow), rootSpec);

    return huxerui::ProvideEnvironment(locale, FluxThemed(
        rootSpec,
        huxerui::Column {
            std::move(profileRefreshPump),
            std::move(applicationEffects),
            std::move(content),
        }.With(huxerui::Grow(1.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))));
}

} // namespace clashflux::ui

#undef CLASHFLUX_PREPARE_PLATFORM_DATA
#undef CLASHFLUX_PROFILE_REFRESH_PUMP
#undef CLASHFLUX_APPLICATION_EFFECTS
#undef CLASHFLUX_APP_CONTENT
#undef CLASHFLUX_MAIN_CONTENT
