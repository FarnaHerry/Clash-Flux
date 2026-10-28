#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <vector>
#include "ui.h"

namespace clashflux::ui {

struct ProfileListSection {
    // No type means one unified list, rather than a synthetic profile type.
    std::optional<std::string> type;
    std::vector<std::size_t> indices;
};

template<class Profiles, class Matches>
std::vector<ProfileListSection> BuildProfileListSections(
    const Profiles& profiles, bool groupByType, Matches matches) {
    if (!groupByType) {
        ProfileListSection all;
        std::size_t index = 0;
        for (const auto& profile : profiles) {
            if (matches(profile)) all.indices.push_back(index);
            ++index;
        }
        if (all.indices.empty()) return {};
        return {std::move(all)};
    }
    std::vector<std::string> types{"remote", "local", "pptp"};
    for (const auto& profile : profiles)
        if (std::find(types.begin(), types.end(), profile.type) == types.end())
            types.push_back(profile.type);
    std::vector<ProfileListSection> sections;
    for (const auto& type : types) {
        ProfileListSection section{type, {}};
        std::size_t index = 0;
        for (const auto& profile : profiles) {
            if (profile.type == type && matches(profile)) section.indices.push_back(index);
            ++index;
        }
        if (!section.indices.empty()) sections.push_back(std::move(section));
    }
    return sections;
}

// Retain the same page insets without mounting a one-page Pager on phones.
inline huxerui::View UnifiedProfileList(huxerui::View list) {
    return huxerui::Scope([list] {
        const auto insets = huxerui::UseEnvironment<SectionTabContentInsets>();
        return huxerui::Column {list}.With(huxerui::Grow(1.0F),
            huxerui::Padding(huxerui::EdgeInsets::Symmetric(insets.horizontal * 0.5F, 0.0F)),
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }).With(huxerui::Grow(1.0F)).Key("profile-unified-list");
}

} // namespace clashflux::ui
