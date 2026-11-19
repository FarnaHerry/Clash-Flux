#pragma once

#include <algorithm>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ui.h"

namespace clashflux::ui {

struct ActiveProxySelection {
    std::string tag;
    std::string displayName;
    bool directMode = false;

    bool operator==(const ActiveProxySelection&) const = default;
};

inline const ProxyGroupSnapshot* DefaultProxyGroup(
    const std::vector<ProxyGroupSnapshot>& groups, bool globalMode = false) {
    const auto global = std::ranges::find(groups, "GLOBAL", &ProxyGroupSnapshot::name);
    if (global != groups.end() && globalMode) return &*global;
    if (global != groups.end() && !global->current.empty()) {
        const auto target = std::ranges::find(groups, global->current, &ProxyGroupSnapshot::name);
        return target != groups.end() ? &*target : &*global;
    }
    const auto selector = std::ranges::find_if(groups, [](const ProxyGroupSnapshot& group) {
        return group.name != "GLOBAL" && group.selectable;
    });
    if (selector != groups.end()) return &*selector;
    const auto other = std::ranges::find_if(groups, [](const ProxyGroupSnapshot& group) {
        return group.name != "GLOBAL";
    });
    if (other != groups.end()) return &*other;
    return groups.empty() ? nullptr : &groups.front();
}

// 解析当前出站模式与策略组快照实际落到的具体节点（跟随 GLOBAL/主组 → 嵌套组 → 叶子节点）。
// preview=true 时非手动可选组（URLTest/Fallback）尚未经运行内核测速选线，停在该组本身而不伪造叶子结果。
inline ActiveProxySelection ResolveActiveProxySelection(
    bool loaded, bool preview, const std::vector<ProxyGroupSnapshot>& groups,
    const std::map<std::string, std::string>& labels, std::string_view mode) {
    if (mode == "direct") {
        return ActiveProxySelection{
            .tag = "DIRECT",
            .displayName = "DIRECT",
            .directMode = true,
        };
    }
    if (!loaded || groups.empty()) return {};

    const auto findGroup = [&](std::string_view name) -> const ProxyGroupSnapshot* {
        for (const auto& group : groups) {
            if (group.name == name) return &group;
        }
        return nullptr;
    };

    const ProxyGroupSnapshot* current = DefaultProxyGroup(groups);
    std::vector<std::string> visited{current->name};
    visited.reserve(8);
    if (preview && !current->selectable) {
        return ActiveProxySelection{
            .tag = current->name,
            .displayName = current->displayName.empty() ? current->name : current->displayName,
        };
    }

    for (std::size_t depth = 0; depth < 16; ++depth) {
        std::string next = current->current;
        if (next.empty() && current->selectable && !current->nodes.empty()) {
            next = current->nodes.front();
        }
        if (next.empty()) {
            return ActiveProxySelection{
                .tag = current->name,
                .displayName = current->displayName.empty() ? current->name : current->displayName,
            };
        }
        const ProxyGroupSnapshot* nextGroup = findGroup(next);
        if (nextGroup == nullptr) {
            std::string display = next;
            if (const auto label = labels.find(next);
                label != labels.end() && !label->second.empty()) {
                display = label->second;
            } else if (const auto memberLabel = current->nodeLabels.find(next);
                       memberLabel != current->nodeLabels.end() && !memberLabel->second.empty()) {
                display = memberLabel->second;
            }
            return ActiveProxySelection{
                .tag = std::move(next),
                .displayName = std::move(display),
            };
        }
        if (preview && !nextGroup->selectable) {
            return ActiveProxySelection{
                .tag = nextGroup->name,
                .displayName = nextGroup->displayName.empty() ? nextGroup->name : nextGroup->displayName,
            };
        }
        if (std::find(visited.begin(), visited.end(), nextGroup->name) != visited.end()) {
            return ActiveProxySelection{
                .tag = nextGroup->name,
                .displayName = nextGroup->displayName.empty() ? nextGroup->name : nextGroup->displayName,
            };
        }
        visited.push_back(nextGroup->name);
        current = nextGroup;
    }
    return ActiveProxySelection{
        .tag = current->name,
        .displayName = current->displayName.empty() ? current->name : current->displayName,
    };
}

} // namespace clashflux::ui
