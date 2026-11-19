#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "placeholder_value.h"
#include "proxies_model.h"
#include "search_text.h"

namespace clashflux::ui {

struct HomeProxyChoice {
    std::string tag;
    std::string label;
};

inline huxerui::View HomeProxyCombo(
    std::vector<HomeProxyChoice> choices, std::string current,
    huxerui::StringVariant placeholder, huxerui::StringVariant noMatches,
    std::string id, bool enabled, std::function<void(std::string)> onSelect) {
    return huxerui::Scope([choices = std::move(choices), current = std::move(current),
                          placeholder, noMatches, id, enabled, onSelect = std::move(onSelect)] {
        auto edit = huxerui::UseState(std::optional<huxerui::TextEditingValue>{});
        const auto& query = edit.Get();
        std::vector<HomeProxyChoice> matches;
        for (const auto& choice : choices) {
            if (!query || SearchTextMatches(choice.label, query->text) ||
                SearchTextMatches(choice.tag, query->text)) {
                matches.push_back(choice);
            }
        }
        const auto value = query.value_or(huxerui::TextEditingValue::FromText(current));
        const std::string label = huxerui::UseString(placeholder);
        return huxerui::ComboBox(value, matches, &HomeProxyChoice::label,
                [](const HomeProxyChoice& choice) {
                    return huxerui::Text(choice.label)
                        .LineLimit(1).Overflow(huxerui::TextOverflow::Ellipsis)
                        .Key(choice.tag);
                })
            .Placeholder(placeholder)
            .Variant(huxerui::TextFieldVariant::Outlined)
            .EmptyContent([noMatches] { return huxerui::Text(noMatches); })
            .OnChanged([edit](const huxerui::TextEditingValue& next) { edit = next; })
            .OnExpandedChanged([edit](bool expanded) {
                if (!expanded) edit = std::nullopt;
            })
            .OnSelected([matches = std::move(matches), onSelect](std::size_t index,
                                                               const huxerui::TextEditingValue&) {
                if (index < matches.size()) onSelect(matches[index].tag);
            })
            .With(huxerui::Enabled(enabled), huxerui::Semantics{.label = label, .identifier = id})
            .Key(id);
    });
}

inline huxerui::View HomeProxyPickers(
    huxerui::State<ProxiesSnapshot> snapshotState, std::uint64_t revision,
    bool globalMode, bool enabled,
    std::function<void(std::string, std::string, std::uint64_t)> onSelect) {
    return huxerui::Scope([snapshotState, revision, globalMode, enabled, onSelect = std::move(onSelect)] {
        constexpr float fieldHeight = 40.0F;
        auto groupTag = huxerui::UseState(std::string{});
        const auto& snapshot = snapshotState.Get();
        if (!snapshot.loaded || snapshot.profilesRevision != revision) {
            return huxerui::View{huxerui::Column {
              PlaceholderSlot(0.0F, fieldHeight),
              PlaceholderSlot(0.0F, fieldHeight),
            }.With(huxerui::Spacing(5.0F))};
        }

        const auto& groups = snapshot.groups;
        const auto found = std::ranges::find(groups, groupTag.Get(), &ProxyGroupSnapshot::name);
        const auto* group = found != groups.end() ? &*found : DefaultProxyGroup(groups, globalMode);
        std::vector<HomeProxyChoice> groupChoices;
        for (const auto& option : groups) {
            groupChoices.push_back({option.name, option.displayName.empty() ? option.name : option.displayName});
        }
        const auto nodeLabel = [group](const std::string& tag) {
            const auto label = group->nodeLabels.find(tag);
            return label != group->nodeLabels.end() ? label->second : tag;
        };
        std::vector<HomeProxyChoice> nodeChoices;
        if (group && group->selectable) {
            for (const auto& tag : group->nodes) nodeChoices.push_back({tag, nodeLabel(tag)});
        }

        auto fields = huxerui::UseEnvironment<huxerui::TextFieldStyle>();
        fields.outlined.minimum_height = fieldHeight;
        fields.padding = huxerui::EdgeInsets::Symmetric(8.0F, 0.0F);
        return huxerui::ProvideEnvironment(fields, huxerui::Column {
          HomeProxyCombo(std::move(groupChoices),
                         group ? (group->displayName.empty() ? group->name : group->displayName) : "",
                         Localized("搜索代理组"), Localized("没有匹配的代理组"), "home-proxy-group",
                         enabled && group, [groupTag](std::string tag) { groupTag = std::move(tag); }),
          HomeProxyCombo(std::move(nodeChoices), group ? nodeLabel(group->current) : "",
                         Localized("搜索节点"), Localized("没有匹配的节点"), "home-proxy-node",
                         enabled && group && group->selectable && !group->nodes.empty(),
                         [onSelect, name = group ? group->name : "", revision](std::string tag) {
                             onSelect(name, std::move(tag), revision);
                         }).Key(group ? group->name : ""),
        }.With(huxerui::Spacing(5.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
    });
}

} // namespace clashflux::ui
