#pragma once

#include <huxerui/huxerui.h>
#include "../subscription_user_agents.h"
#include "settings_group_layout.h"
#include "settings_value.h"

namespace clashflux::ui {

[[huxerui::composable]] inline huxerui::View SubscriptionUserAgentOrderControl(
    bool ready, std::string order, std::function<void(std::size_t)> onChanged) {
    const auto& theme = huxerui::UseTheme();
    std::vector<std::string> labels;
    for (std::size_t index = 0; index < subscription_http::kOrders.size(); ++index)
        labels.push_back(subscription_http::OrderLabel(index));
    std::vector<huxerui::View> versions;
    for (const auto agent : subscription_http::UserAgents(order))
        versions.push_back(huxerui::Text(std::string{agent}, huxerui::TextRole::Label));
    return SettingsContentRow(huxerui::Column {
      huxerui::Text(Localized("UA 尝试顺序")),
      SettingsValue(ready, huxerui::Select(labels, subscription_http::OrderIndex(order),
          [](const std::string& text) { return huxerui::Text(text); })
          .Label(Localized("UA 尝试顺序")).OnChanged(onChanged)
          .With(huxerui::Enabled(ready)).Key("subscription-ua-select")),
      huxerui::Text(Localized("用于之后的订阅下载。HTTP 403 时尝试下一个 UA，成功后停止。"),
          huxerui::TextRole::Label),
      huxerui::Text(Localized("兼容版本固定，不随上游自动升级。"), huxerui::TextRole::Label),
      huxerui::Column(versions).With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Start)),
    }.With(huxerui::Spacing(theme.spacing.small),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
}

} // namespace clashflux::ui
