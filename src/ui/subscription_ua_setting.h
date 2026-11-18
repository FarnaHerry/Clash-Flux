#pragma once

#include <huxerui/huxerui.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../subscription_user_agents.h"
#include "page_layout.h"
#include "settings_group_layout.h"

namespace clashflux::ui {

// Two complete UA sequences. The selected sequence is written immediately;
// each option shows the exact identities it will send, without explanatory copy.
inline huxerui::View SubscriptionUserAgentChoices(
    bool ready, std::string order, std::function<void(std::size_t)> onChanged) {
    return huxerui::Scope([ready, order, onChanged] {
        const auto& theme = huxerui::UseTheme();
        const auto insets = huxerui::UseEnvironment<SettingsRowInsets>();
        const std::size_t selected = subscription_http::OrderIndex(order);
        std::vector<huxerui::View> options;
        for (std::size_t index = 0; index < subscription_http::kOrders.size(); ++index) {
            if (index != 0) options.push_back(huxerui::Divider());
            const auto agents = subscription_http::OrderUserAgents(index);
            std::vector<huxerui::View> values;
            for (const auto agent : agents) {
                values.push_back(huxerui::Text(std::string{agent}).Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    theme.colors.on_surface_variant}));
            }
            huxerui::View item = huxerui::Column {
              huxerui::RadioButton(subscription_http::OrderLabel(index), ready && index == selected)
                  .With(huxerui::Enabled(ready))
                  .OnChanged([onChanged, index](bool checked) {
                      if (checked) onChanged(index);
                  })
                  .Key("subscription-ua-option-" + std::to_string(index)),
              huxerui::Column(std::move(values))
                  .With(huxerui::Padding(huxerui::EdgeInsets{.left = 30.0F}),
                        huxerui::Spacing(2.0F),
                        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
            }.With(huxerui::Spacing(4.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
            if (insets == SettingsRowInsets{}) {
                item = huxerui::Column {
                  item,
                }.With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(0.0F, 8.0F)),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
            } else {
                item = SettingsContentRow(item);
            }
            options.push_back(item);
        }
        return huxerui::Column(std::move(options)).With(
            huxerui::Spacing(0.0F),
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)).Key("subscription-ua-options");
    });
}

inline huxerui::View SubscriptionUserAgentPageBody(
    bool ready, std::string order, std::function<void(std::size_t)> onChanged,
    std::shared_ptr<PageScrollOffset> scroll) {
    return huxerui::Scope([ready, order, onChanged, scroll] {
        const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
        huxerui::View content = huxerui::Column {
          huxerui::ProvideEnvironment(
              SettingsRowInsets{compact ? 12.0F : 0.0F, 10.0F, 48.0F},
              SubscriptionUserAgentChoices(ready, order, onChanged)),
        }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
        if (!compact) {
            content = huxerui::Row {
              huxerui::Spacer(),
              huxerui::View{content}.With(huxerui::Frame{.max_width = 720.0F}),
              huxerui::Spacer(),
            }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Start));
        }
        return ScrollablePageContent(content, scroll).Key("subscription-ua-scroll");
    });
}

inline huxerui::View SubscriptionUserAgentDialogView(
    bool ready, std::string order, std::function<void(std::size_t)> onChanged,
    std::function<void()> onClose) {
    return DialogScaffold(
        DialogHeader(Localized("UA 尝试顺序")),
        huxerui::ProvideEnvironment(
            SettingsRowInsets{4.0F, 8.0F, 48.0F},
            SubscriptionUserAgentChoices(ready, std::move(order), std::move(onChanged))),
        DialogActions({DialogAction(Localized("关闭"), std::move(onClose), false).Key("subscription-ua-close")}),
        DialogSize::Small);
}

} // namespace clashflux::ui
