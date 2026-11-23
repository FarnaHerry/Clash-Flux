#pragma once

#include "website_test_model.h"
#include "ui.h"
#include "app_resources.h"
#include "proxy_node_tile.h"
#include "proxy_speed_button.h"

namespace clashflux::ui {

inline huxerui::View WebsiteTestBatchAction(bool testing, std::function<void()> start) {
    const auto label = Localized("全部测试");
    return SpeedTestButton(label, label, std::move(start), testing);
}

inline huxerui::View WebsiteTestCard(WebsiteTestSite site, std::function<void()> start) {
    return huxerui::Scope([site = std::move(site), start = std::move(start)] {
        const auto& theme = huxerui::UseTheme();
        const auto color = theme.colors.on_surface_variant;
        const auto& result = site.result;
        const bool busy = result.phase == WebsiteTestPhase::Queued || result.phase == WebsiteTestPhase::Running;
        const bool failed = result.phase == WebsiteTestPhase::Failed;
        const auto testLabel = result.phase == WebsiteTestPhase::Queued ? Localized("等待测试") :
            busy ? Localized("测试中…") : Localized("测速");
        const auto accessibleLabel = huxerui::UseString(testLabel) + ": " + site.name;
        const auto key = std::to_string(site.id);

        huxerui::View route = PlaceholderSlot();
        if (!result.outbound.empty() || result.status > 0) {
            const huxerui::StringVariant outbound =
                result.outbound.empty() ? Localized("出站未确认") :
                result.outbound == "DIRECT" ? Localized("直连") : huxerui::StringVariant{result.outbound};
            route = huxerui::Text(outbound)
                .LineLimit(1).Overflow(huxerui::TextOverflow::Ellipsis)
                .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption), color})
                .With(huxerui::Tooltip(outbound));
        }

        huxerui::View indicator;
        huxerui::StringVariant hint = testLabel;
        std::optional<huxerui::StringVariant> accessibleValue;
        if (busy) {
            indicator = ProxyProbeDots(color, 24.0F);
        } else if (failed) {
            const auto status = result.error == clashflux::website::kTimeoutError ? Localized("超时") : Localized("失败");
            accessibleValue = status;
            hint = result.error.empty() ? status : huxerui::StringVariant{result.error};
            indicator = huxerui::Text(status)
                .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption), theme.colors.error});
        } else if (result.latencyMs) {
            hint = huxerui::StringVariant{std::to_string(*result.latencyMs) + " ms"};
            accessibleValue = hint;
            indicator = huxerui::Text(std::to_string(*result.latencyMs))
                .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption),
                                         DelayLevelColor(theme, std::max(1, *result.latencyMs))});
        } else {
            indicator = huxerui::Image(app::images::proxy_node_probe_outline).Tint(color)
                .With(huxerui::Frame{.width = 14.0F, .height = 14.0F}).Key("website-probe-icon");
        }

        constexpr float resultHeight = 22.0F;
        const auto action = huxerui::Row {std::move(indicator).With(huxerui::Semantics{.hidden = true})}
            .With(huxerui::Frame{.height = resultHeight},
                  huxerui::Padding(huxerui::EdgeInsets::Symmetric(3.0F, 0.0F)),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                  huxerui::Enabled(!busy),
                  huxerui::Focusable(!busy), huxerui::CornerRadius(theme.shapes.small),
                  huxerui::PointerCursor(busy ? huxerui::PointerCursorKind::Default : huxerui::PointerCursorKind::Hand),
                  busy ? huxerui::Indication{} : theme.interactions.indication,
                  huxerui::Tooltip(hint),
                  huxerui::Semantics{.role = huxerui::SemanticRole::Button, .label = accessibleLabel,
                                     .value = accessibleValue, .busy = busy})
            .OnClick([busy, start] { if (!busy) start(); })
            .Key("website-test-" + key);
        const auto testSlot = huxerui::Stack {action}
            .With(huxerui::Frame{.width = kProxyDelaySlotWidth, .height = resultHeight},
                  huxerui::Align(huxerui::HorizontalAlignment::End, huxerui::VerticalAlignment::Center))
            .Key("website-test-slot-" + key);
        const auto logo = (site.icon == app::images::github
                ? huxerui::Image(site.icon).Tint(color) : huxerui::Image(site.icon))
            .With(huxerui::Frame{.width = 20.0F, .height = 20.0F}).Key("website-icon-" + key);
        return Card(huxerui::Column {
          huxerui::Row {
            logo,
            huxerui::Text(site.name, huxerui::TextRole::Title)
                .LineLimit(1).Overflow(huxerui::TextOverflow::Ellipsis).With(huxerui::Grow(1.0F)),
          }.With(huxerui::Frame{.min_height = 28.0F}, huxerui::Spacing(8.0F),
                 huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
          huxerui::Row {
            huxerui::Stack {route}
                .With(huxerui::Frame{.height = resultHeight}, huxerui::Grow(1.0F),
                      huxerui::Align(huxerui::HorizontalAlignment::Stretch, huxerui::VerticalAlignment::Center))
                .Key("website-result-" + key),
            testSlot,
          }.With(huxerui::Spacing(8.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        }.With(huxerui::Spacing(8.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
    });
}

} // namespace clashflux::ui
