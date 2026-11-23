#pragma once

#include <algorithm>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>

#include <huxerui/huxerui.h>

#include "app_resources.h"
#include "proxy_test_model.h"
#include "ui.h"

namespace clashflux::ui {

inline constexpr float kProxySpeedButtonSize = 32.0F;

inline huxerui::View SpeedTestButton(huxerui::StringVariant label, huxerui::StringVariant tooltip,
    std::function<void()> start, bool testing, bool enabled = true, std::optional<float> extent = {}) {
    return huxerui::Scope([label, tooltip, testing, enabled, extent, start = std::move(start)] {
        const auto& theme = huxerui::UseTheme();
        const bool background = huxerui::UseWindow().LifecycleState() == huxerui::WindowLifecycleState::Background;
        const auto& iconStyle = huxerui::UseEnvironment<huxerui::IconButtonStyle>();
        const float buttonSize = extent.value_or(std::max(iconStyle.icon_size, iconStyle.minimum_interactive_size));
        const huxerui::Frame size{.width = buttonSize, .height = buttonSize};
        huxerui::View content;
        if (testing) {
            auto style = huxerui::UseEnvironment<huxerui::ProgressCircleStyle>();
            style.size = 20.0F;
            style.stroke_width = 2.0F;
            if (theme.motion.reduced_motion || background) {
                style.animation_duration = 0.0;
            }
            content = huxerui::ProvideEnvironment(style, huxerui::ProgressCircle()
                .With(huxerui::Semantics{.label = Localized("测速中…"),
                                         .live_region = huxerui::SemanticLiveRegion::Polite})
                .Key("speed-test-loading"));
        } else {
            content = huxerui::IconButton(app::images::proxy_node_probe, label)
                .With(size, huxerui::Enabled(enabled), huxerui::Foreground(theme.colors.on_surface),
                      huxerui::Tooltip(tooltip))
                .OnClick(start)
                .Key("speed-test-action");
        }
        return huxerui::Column {
          content,
        }.With(size, huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
            .Key("speed-test-button");
    });
}

inline huxerui::View ProxySpeedButton(const ProxyTestModel& tests, std::string_view group,
                                     std::function<void()> start) {
    const bool busy = tests.progress.Get().busy;
    const bool testing = tests.preparing.Get() || (busy && tests.group.Get() == group);
    const bool enabled = !busy && tests.singlePending.Get() == 0;
    return SpeedTestButton(Localized("测速"), Localized("测试当前分组延迟"), std::move(start),
        testing, enabled, kProxySpeedButtonSize);
}

} // namespace clashflux::ui
