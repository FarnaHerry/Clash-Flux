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
        const auto& iconStyle = huxerui::UseEnvironment<huxerui::IconButtonStyle>();
        const float buttonSize = extent.value_or(std::max(iconStyle.icon_size, iconStyle.minimum_interactive_size));
        const huxerui::Frame size{.width = buttonSize, .height = buttonSize};
        huxerui::View content;
        if (testing) {
            content = FluxProgressCircle(
                20.0F, 2.0F,
                huxerui::Semantics{.label = Localized("测速中…"),
                                   .live_region = huxerui::SemanticLiveRegion::Polite})
                .Key("speed-test-loading");
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

inline huxerui::View ProxySpeedButton(ProxyTestModel& tests, std::string_view group,
                                     std::function<void()> start) {
    const bool testing = tests.GetGroup(group)->ticket.Get() != 0;
    return SpeedTestButton(Localized("测速"), Localized("测试当前分组延迟"), std::move(start),
        testing, !testing, kProxySpeedButtonSize);
}

inline huxerui::View ProxyAllSpeedButton(std::shared_ptr<ProxyTestModel> tests, std::vector<std::string> groups,
    std::function<void(const std::string&)> start) {
    return huxerui::Scope([tests, groups = std::move(groups), start = std::move(start)] {
        const auto label = Localized("测试所有分组延迟");
        return SpeedTestButton(label, label, [tests, groups, start] { tests->StartAllGroups(groups, start); },
            tests->AllGroupsTesting(), !groups.empty(), kProxySpeedButtonSize);
    }).Key("proxy-test-all");
}

} // namespace clashflux::ui
