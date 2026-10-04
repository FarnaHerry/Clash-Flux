#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
#include <huxerui/huxerui.h>
#include "ui.h"

namespace clashflux::ui {
struct RuleTargetObject {
    std::size_t kind = 0; // RouteRule TargetKind: default=0, group=1, node=2.
    std::string name;
    bool operator==(const RuleTargetObject&) const = default;
};
struct RuleTargetCatalog {
    std::int64_t profileId = 0;
    bool ready = false;
    bool defaultOnly = false;
    std::string error;
    std::vector<RuleTargetObject> objects;
    bool operator==(const RuleTargetCatalog&) const = default;
};

inline bool RuleTargetSelectionValid(const RuleTargetCatalog& catalog,
    std::int64_t profileId, std::size_t kind, const std::string& name) {
    if (kind == 0) return true;
    return catalog.ready && !catalog.defaultOnly && catalog.error.empty() &&
        catalog.profileId == profileId && std::ranges::any_of(catalog.objects, [&](const auto& object) {
            return object.kind == kind && object.name == name;
        });
}

inline void ResetRuleTargetSelection(huxerui::State<RuleTargetCatalog> catalog,
    huxerui::State<huxerui::TextEditingValue> object,
    huxerui::State<huxerui::TextEditingValue> search) {
    object = huxerui::TextEditingValue{};
    search = huxerui::TextEditingValue{};
    catalog = RuleTargetCatalog{};
}

// This is editor-local data; it does not mirror or modify the running catalog.
// Search limits visible choices, never membership validation or saved identity.
inline huxerui::View RuleTargetPicker(huxerui::State<RuleTargetCatalog> catalog,
    huxerui::State<std::size_t> kind, huxerui::State<huxerui::TextEditingValue> object,
    huxerui::State<huxerui::TextEditingValue> search) {
    const auto& directory = catalog.Get();
    std::vector<std::string> kinds{"默认出口"};
    std::size_t kindIndex = 0;
    const bool defaultOnly = directory.defaultOnly || !directory.ready || !directory.error.empty();
    if (!defaultOnly) {
        kinds.insert(kinds.end(), {"策略组", "节点"});
        kindIndex = std::min(kind.Get(), std::size_t{2});
    } else if (kind.Get() != 0) {
        kinds.push_back(!directory.ready ? (kind.Get() == 1 ? "策略组" : "节点") : "原目标不可用");
        kindIndex = 1;
    }
    std::vector<std::string> names;
    const auto selected = object.Get().text;
    if (directory.ready && directory.error.empty() && !directory.defaultOnly) {
        for (const auto& entry : directory.objects) {
            if (entry.kind != kind.Get() || entry.name.find(search.Get().text) == std::string::npos) continue;
            if (names.size() < 200) names.push_back(entry.name);
        }
        if (!selected.empty() && RuleTargetSelectionValid(directory, directory.profileId, kind.Get(), selected) &&
            std::ranges::find(names, selected) == names.end()) names.push_back(selected);
    }
    std::vector<std::size_t> choices;
    for (std::size_t i = 0; i <= names.size(); ++i) choices.push_back(i);
    const auto placeholder = selected.empty() ? Localized("请选择目标对象") :
        LocalizedFormat(!directory.ready || !directory.error.empty() ? "原目标：{}" : "已失效：{}", selected);
    const auto found = std::ranges::find(names, selected);
    const std::size_t selectedIndex = found == names.end() ? 0 : static_cast<std::size_t>(found - names.begin()) + 1;
    const auto status = !directory.ready ? "正在读取对象…" : !directory.error.empty() ? directory.error :
        directory.defaultOnly ? "此来源使用默认出口" : names.empty() ? "没有匹配的对象" :
        "对象来自订阅内容；保存时检查可用性";
    return huxerui::Column{
        huxerui::Text(Localized("目标对象")),
        huxerui::Select(kinds, kindIndex, [](const std::string& text) { return huxerui::Text(Localized(text)); })
            .Key("rule-target-kind").OnChanged([kind, object, search, defaultOnly](std::size_t index) {
                if (defaultOnly && index != 0) return;
                kind = index; object = huxerui::TextEditingValue{}; search = huxerui::TextEditingValue{};
            }),
        kind.Get() == 0 ? huxerui::View{huxerui::Row{}} : huxerui::View{huxerui::Column{
            huxerui::TextField(search.Get()).Label(Localized("搜索对象名称"))
                .Variant(huxerui::TextFieldVariant::Outlined).Key("rule-target-search")
                .OnChanged([search](const huxerui::TextEditingValue& value) { search = value; }),
            huxerui::Select(choices, selectedIndex, [names, placeholder](std::size_t index) {
                return index == 0 ? huxerui::Text(placeholder) : huxerui::Text(names[index - 1]);
            })
                .Key("rule-target-object").OnChanged([names, object](std::size_t index) {
                    if (index > 0 && index <= names.size()) object = huxerui::TextEditingValue::FromText(names[index - 1]);
                }),
        }.With(huxerui::Spacing(8.0F))},
        huxerui::Text(directory.error.empty() ? Localized(status) : huxerui::StringVariant(status)).Key("rule-target-status"),
    }.With(huxerui::Spacing(8.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}
} // namespace clashflux::ui
