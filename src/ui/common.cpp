// common.cpp — 页面骨架 / 卡片 / 内核状态胶囊等跨页通用部件。
#include <huxerui/huxerui.h>

#include <chrono>
#include <string>

#include "ui.h"
#include "task_bridge.h"

import clashflux.core;
import clashflux.store.core;

namespace clashflux::ui {

huxerui::View PageScaffold(const std::string& title, huxerui::View actions,
                           huxerui::View content) {
    return huxerui::Column {
        huxerui::Row {
            huxerui::Text(title, huxerui::TextRole::Title),
            huxerui::Spacer(),
            std::move(actions),
        }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        std::move(content).With(huxerui::Grow(1.0F)),
    }.With(huxerui::Padding(16.0F),
           huxerui::Spacing(12.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View Card(huxerui::View content) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Column { std::move(content) }
        .With(huxerui::Padding(theme.spacing.medium),
              huxerui::Background(theme.colors.surface_container),
              huxerui::CornerRadius(theme.shapes.medium),
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
           huxerui::Background(theme.colors.surface_container),
           huxerui::CornerRadius(theme.shapes.large),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
           huxerui::Frame{.height = kTitleBarContentHeight},
           huxerui::Tooltip("mihomo 内核状态"));
}

} // namespace clashflux::ui
