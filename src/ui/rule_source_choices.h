#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <huxerui/huxerui.h>

namespace clashflux::ui {

// One entry in the global-rule target dropdown. `direct` marks the built-in
// DIRECT target; it is not a subscription and has no profile id.
struct RuleSourceChoice {
    std::int64_t id;
    std::string name;
    bool direct = false;
    bool operator==(const RuleSourceChoice&) const = default;
};

template <class Profile>
std::vector<RuleSourceChoice> TargetProfiles(const huxerui::StateList<Profile>& profiles,
                                             std::string_view directName,
                                             bool multiProxySources) {
    std::vector<RuleSourceChoice> result;
    for (const auto& profile : profiles) {
        if (!multiProxySources && !profile.selected && profile.type != "pptp") continue;
        result.push_back({profile.id, profile.name});
    }
    // 直连不是订阅，但必须是可显式选择的目标；保留订阅在前，新增规则
    // 的默认目标不变。
    result.push_back({.id = -1, .name = std::string(directName), .direct = true});
    return result;
}

inline std::vector<std::string> TargetNames(const std::vector<RuleSourceChoice>& profiles) {
    std::vector<std::string> result;
    for (const auto& profile : profiles) result.push_back(profile.name);
    return result;
}

} // namespace clashflux::ui
