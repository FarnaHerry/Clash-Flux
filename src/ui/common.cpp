// common.cpp — 岛屿原语（ResolveIslandTheme/IslandSurface/IslandSection）、
// 页面骨架（一级岛）/ 卡片（二级岛）/ 内核状态胶囊等跨页通用部件。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "ui.h"
#include "task_bridge.h"

import clashflux.core;
import clashflux.store.core;

namespace clashflux::ui {

IslandTheme ResolveIslandTheme(const huxerui::ThemeSpec& theme) {
    return IslandTheme{
        .page_gap = theme.spacing.small,
        .island_padding = theme.spacing.medium,
        .island_radius = 16.0F,
        .nested_radius = 8.0F,
        .ocean = theme.colors.background,
        .base = theme.colors.surface_container_low,
        .raised = theme.colors.surface_container,
        .active = theme.colors.surface_container_high,
        .overlay = theme.colors.surface_container_highest,
        .outline_soft = theme.colors.outline,
    };
}

float ConcentricRadius(float outer_radius, float inset) {
    // 内层半径 = 外层半径 − inset，保下限 4pt：更小半径与父级圆角几乎相切，
    // 视觉上出现反同心（子角比父角“尖”）。
    constexpr float kMinRadius = 4.0F;
    return std::max(outer_radius - inset, kMinRadius);
}

namespace {

huxerui::Color IslandColor(const IslandTheme& islands, const huxerui::ThemeSpec& theme,
                           IslandLevel level) {
    switch (level) {
        case IslandLevel::Base: return islands.base;
        case IslandLevel::Raised: return islands.raised;
        case IslandLevel::Active: return islands.active;
        case IslandLevel::Overlay: return islands.overlay;
        case IslandLevel::Danger: {
            huxerui::Color danger = theme.colors.error;
            danger.alpha = 0.10F;
            return danger;
        }
    }
    return islands.base;
}

} // namespace

[[huxerui::composable]] huxerui::View IslandSurface(huxerui::View content,
                                                    IslandLevel level) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    // composable 形参被 codegen 固定为 const：拷贝到局部再走右值 With 链。
    huxerui::View surface = content;
    return std::move(surface).With(huxerui::Background(IslandColor(islands, theme, level)),
                                   huxerui::CornerRadius(islands.island_radius),
                                   huxerui::Padding(islands.island_padding));
}

[[huxerui::composable]] huxerui::View IslandSection(std::string title,
                                                    huxerui::View content) {
    return IslandSurface(
        huxerui::Column {
            huxerui::Text(std::move(title), huxerui::TextRole::Title),
            std::move(content),
        }.With(huxerui::Spacing(12.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
        IslandLevel::Base);
}

[[huxerui::composable]] huxerui::View PageScaffold(const std::string& title,
                                                   huxerui::View actions,
                                                   huxerui::View content) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    // 一级岛：页面根本身是岛（Grow + Stretch 占满页面区块，圆角 16pt，
    // base 表面），内容在岛内部滚动；海面底色经岛间缝隙透出。
    // composable 形参被 codegen 固定为 const：拷贝到局部再走右值链。
    huxerui::View body = content;
    return huxerui::Column {
        huxerui::Row {
            huxerui::Text(title, huxerui::TextRole::Title),
            huxerui::Spacer(),
            std::move(actions),
        }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        std::move(body).With(huxerui::Grow(1.0F)),
    }.With(huxerui::Padding(theme.spacing.large),
           huxerui::Spacing(theme.spacing.medium),
           huxerui::Background(islands.base),
           huxerui::CornerRadius(islands.island_radius),
           huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View Card(huxerui::View content) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    // 二级岛：raised 表面（比一级岛高一层级）+ 8pt 同心圆角。
    return huxerui::Column { std::move(content) }
        .With(huxerui::Padding(islands.island_padding),
              huxerui::Background(islands.raised),
              huxerui::CornerRadius(islands.nested_radius),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View CoreStatusPill() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto snap = huxerui::UseState<store::CoreSnapshot>({});

    huxerui::Lifecycle(
        [tasks, snap] {
            tasks.Launch([snap]() -> huxerui::Task<void> {
                co_await PollWhile(std::chrono::duration<double>{0.5}, [snap] {
                    snap = store::coreStore().snapshot();
                    return true;
                });
            });
            return [] {};
        },
        0);

    const store::CoreSnapshot s = snap.Get();
    huxerui::Color dot = theme.colors.on_surface_variant;
    std::string label = "已停止";
    if (s.binaryPath.empty()) {
        label = "内核未安装";
    } else if (s.state == core::CoreState::Running) {
        dot = huxerui::Color::Rgb(34, 197, 94);   // 绿
        label = s.version.empty() ? "运行中" : s.version;
    } else if (s.state == core::CoreState::Starting) {
        dot = huxerui::Color::Rgb(245, 158, 11);  // 琥珀
        label = "启动中";
    } else if (s.state == core::CoreState::Failed) {
        dot = theme.colors.error;
        label = "内核异常";
    }

    return huxerui::Row {
        huxerui::Text("●").Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kCaption), dot}),
        huxerui::Text(label).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kCaption),
            theme.colors.on_surface_variant}),
    }.With(huxerui::Spacing(6.0F),
           huxerui::Padding(huxerui::EdgeInsets::Symmetric(8.0F, 3.0F)),
           huxerui::Background(ResolveIslandTheme(theme).overlay),
           huxerui::CornerRadius(ResolveIslandTheme(theme).nested_radius),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
           huxerui::Frame{.height = kTitleBarContentHeight},
           huxerui::Tooltip("mihomo 内核状态"));
}

} // namespace clashflux::ui
