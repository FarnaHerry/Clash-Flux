#pragma once

#include <functional>
#include <string>
#include <vector>
#include <huxerui/huxerui.h>

namespace clashflux::ui {

// Shared by every viewport; only the available text width changes.
inline constexpr float kSubscriptionRuleEstimatedExtent = 60.0F;

huxerui::View SubscriptionRulesToolbar(std::vector<std::string> sources, std::size_t source,
    std::vector<std::string> views, std::size_t view, bool busy,
    std::function<void(std::size_t)> selectSource, std::function<void(std::size_t)> selectView,
    huxerui::View more);

huxerui::View SubscriptionRuleContent(std::string kind, std::string pattern, std::string target,
    std::string status, huxerui::View more, bool enabled = true);

} // namespace clashflux::ui
