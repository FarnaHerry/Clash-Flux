#pragma once

#include <huxerui/huxerui.h>

#include <functional>
#include <string>
#include <vector>

#include "../subscription_user_agents.h"
#include "settings_group_layout.h"

namespace clashflux::ui {

// Two complete UA sequences. The selected sequence is written immediately;
// each option shows the exact identities it will send, without explanatory copy.
[[huxerui::composable]] inline huxerui::View SubscriptionUserAgentChoices(
    bool ready, std::string order, std::function<void(std::size_t)> onChanged) {
    const auto& theme = huxerui::UseTheme();
    const std::size_t selected = subscription_http::OrderIndex(order);
    std::vector<huxerui::View> options;
    for (std::size_t index = 0; index < subscription_http::kOrders.size(); ++index) {
        const auto agents = subscription_http::OrderUserAgents(index);
        std::vector<huxerui::View> values;
        for (const auto agent : agents)
            values.push_back(huxerui::Text(std::string{agent}, huxerui::TextRole::Label));
        options.push_back(SettingsContentRow(huxerui::Column {
          huxerui::RadioButton(subscription_http::OrderLabel(index), index == selected)
              .With(huxerui::Grow(1.0F), huxerui::Enabled(ready))
              .OnChanged([onChanged, index](bool checked) {
                  if (checked) onChanged(index);
              })
              .Key("subscription-ua-option-" + std::to_string(index)),
          huxerui::Column(std::move(values))
              .With(huxerui::Padding(huxerui::EdgeInsets{.left = 30.0F}),
                     huxerui::Spacing(2.0F),
                     huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
        }.With(huxerui::Spacing(4.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))));
    }
    return huxerui::Column(std::move(options)).With(
        huxerui::Spacing(theme.spacing.small),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)).Key("subscription-ua-options");
}

} // namespace clashflux::ui
