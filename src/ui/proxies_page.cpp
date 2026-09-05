// proxies_page.cpp — 代理页：顶部收束区（出站模式 规则/全局/直连 切换 + 订阅
// 自带分组的横向 chips）→ 选中组的统一矩形节点卡网格 → 底部嵌套分支导航
// （组里嵌组的节点可点入，面包屑显示当前路径 + 返回上一级）。
//
// 节点网格列数随视口分级：Compact(<600) 2 列 / Medium 3 列 / Expanded 4 列；
// 末行用空占位补齐，保证同一组内所有节点卡同宽。
//
// 分支语义：点组类型节点 = 选中该分支到当前组（selectProxy）并进入浏览；
// 叶子节点 = 常规切换。路径 State 只存用户走出的链，渲染期经 resolvePath
// 对最新 groups 校验裁剪，订阅刷新导致组消失时自动回落，无需泵写状态。
//
// 数据流：PollWhile 每 3s 拉 GET /proxies 解析成组模型写 State。测速 / 切节点
// 都是阻塞 REST，全部走 RunOnTaskThread；点击事件处理器内不直接写 State
// （约定 6），只 Launch 协程。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "ui.h"
#include "task_bridge.h"

import nlohmann.json;
import clashflux.core;
import clashflux.store.core;
import clashflux.utils;

namespace clashflux::ui {
namespace {

// 与首页模式卡同源（home_page.cpp 匿名命名空间各持一份）。
const std::vector<huxerui::StringVariant> kModeNames{"规则", "全局", "直连"};
const std::vector<std::string> kModes{"rule", "global", "direct"};

struct ProxyNode {
    std::string name;
    std::string type;
    int delay = 0;      // 0 = 未测；来自 history 或测速结果
    bool timeout = false;
    bool udp = false;
    bool isGroup = false;      // 该节点本身是策略组（可点入的分支）
    std::string groupNow;      // isGroup 时：分支内当前选中节点

    bool operator==(const ProxyNode&) const = default;
};

struct ProxyGroup {
    std::string name;
    std::string type;   // Selector / URLTest / Fallback / LoadBalance / Relay
    std::string now;    // 当前选中节点
    std::vector<ProxyNode> nodes;

    bool operator==(const ProxyGroup&) const = default;
};

// 组类型：带 all 列表的才是策略组（Selector/URLTest/Fallback/LoadBalance/Relay），
// 其余（Direct/Reject/具体协议节点）不进组列表。
bool isGroupType(const std::string& t) {
    return t == "Selector" || t == "URLTest" || t == "Fallback" ||
           t == "LoadBalance" || t == "Relay";
}

std::vector<ProxyGroup> parseProxies(const std::string& body) {
    std::vector<ProxyGroup> groups;
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (!j.is_object() || !j.contains("proxies") || !j["proxies"].is_object()) {
        return groups;
    }
    const auto& all = j["proxies"];
    // 不用 .items() 结构化绑定：nlohmann 模块导出不含迭代代理的 get<>，
    // 迭代器 + key()/value() 在模块下可用。
    for (auto it = all.begin(); it != all.end(); ++it) {
        const auto& v = it.value();
        if (!v.is_object()) continue;
        const std::string type = v.value("type", "");
        if (!isGroupType(type) || !v.contains("all") || !v["all"].is_array()) continue;
        ProxyGroup g;
        g.name = it.key();
        g.type = type;
        g.now = v.value("now", "");
        for (const auto& nodeName : v["all"]) {
            if (!nodeName.is_string()) continue;
            ProxyNode node;
            node.name = nodeName.get<std::string>();
            const auto nit = all.find(node.name);
            if (nit != all.end() && nit->is_object()) {
                node.type = nit->value("type", "");
                node.udp = nit->value("udp", false);
                node.isGroup = isGroupType(node.type);
                node.groupNow = node.isGroup ? nit->value("now", "") : "";
                // history 最新一条延迟（unified-delay 下含完整耗时）。
                if (nit->contains("history") && (*nit)["history"].is_array() &&
                    !(*nit)["history"].empty()) {
                    const auto& last = (*nit)["history"].back();
                    if (last.is_object()) node.delay = last.value("delay", 0);
                }
            }
            g.nodes.push_back(std::move(node));
        }
        groups.push_back(std::move(g));
    }
    // GLOBAL 组太长且无意义时沉底（mihomo 自带，列出全部节点）。
    std::ranges::stable_sort(groups, [](const ProxyGroup& a, const ProxyGroup& b) {
        return (a.name == "GLOBAL") < (b.name == "GLOBAL");
    });
    return groups;
}

const ProxyGroup* findGroup(const std::vector<ProxyGroup>& groups,
                            const std::string& name) {
    for (const auto& g : groups) {
        if (g.name == name) return &g;
    }
    return nullptr;
}

// 渲染期校验：只保留 navPath 中仍然存在且逐级可达（组类型子节点）的前缀；
// 空输入或根组消失返回空（调用方回落第一个组）。
std::vector<std::string> resolvePath(const std::vector<ProxyGroup>& groups,
                                     const std::vector<std::string>& raw) {
    std::vector<std::string> out;
    if (raw.empty()) return out;
    const ProxyGroup* g = findGroup(groups, raw.front());
    if (!g) return out;
    out.push_back(g->name);
    for (std::size_t i = 1; i < raw.size(); ++i) {
        bool reachable = false;
        for (const auto& n : g->nodes) {
            if (n.name == raw[i] && n.isGroup) {
                reachable = true;
                break;
            }
        }
        if (!reachable) break;
        out.push_back(raw[i]);
        g = findGroup(groups, raw[i]);
        if (!g) break;
    }
    return out;
}

// 延迟着色：未测灰 / <300ms 绿 / <1000ms 琥珀 / 超时红。
huxerui::Color delayColor(const huxerui::ThemeSpec& theme, int delay, bool timeout) {
    if (timeout) return theme.colors.error;
    if (delay <= 0) return theme.colors.on_surface_variant;
    if (delay < 300) return huxerui::Color::Rgb(34, 197, 94);
    if (delay < 1000) return huxerui::Color::Rgb(245, 158, 11);
    return theme.colors.error;
}

// 节点统一矩形卡：名称（正文级）+ 元信息行（延迟着色；组类型节点显示
// 「组 · 分支当前选中」作为可点入提示），选中态 primary 底。
// 宽度由 NodeGrid 的 Grow(1) 均分，高度随行内 Stretch 拉齐。
[[huxerui::composable]] huxerui::View NodeCard(
    const ProxyNode& node, bool selected,
    huxerui::State<std::unordered_map<std::string, int>> delays,
    huxerui::State<std::unordered_map<std::string, bool>> timeouts,
    std::function<void()> onSelect) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    // 测速结果优先于 history 快照。
    int delay = node.delay;
    bool timeout = node.timeout;
    if (const auto it = delays.Get().find(node.name); it != delays.Get().end()) {
        delay = it->second;
        timeout = false;
    }
    if (const auto it = timeouts.Get().find(node.name);
        it != timeouts.Get().end() && it->second) {
        timeout = true;
        delay = 0;
    }

    std::string meta;
    if (node.isGroup) {
        meta = "组 · " + (node.groupNow.empty() ? node.type : node.groupNow);
    } else {
        meta = timeout ? "超时"
                       : (delay > 0 ? std::format("{} ms", delay) : node.type);
    }

    huxerui::Color bg = islands.active;
    huxerui::Color fg = theme.colors.on_surface;
    if (selected) {
        bg = theme.colors.primary;
        fg = theme.colors.on_primary;
    }
    return huxerui::Column {
        huxerui::Text(node.name).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kBody), fg}),
        huxerui::Text(meta)
            .Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption),
                selected ? fg
                         : (node.isGroup ? theme.colors.on_surface_variant
                                         : delayColor(theme, delay, timeout))}),
    }
        .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 8.0F)),
              huxerui::Background(bg),
              huxerui::CornerRadius(islands.nested_radius),
              huxerui::Grow(1.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Start),
              huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                 .label = node.name},
              huxerui::Focusable(true))
        .OnClick([onSelect = std::move(onSelect)] { onSelect(); });
}

// 节点统一网格：按 cols 切行，每张卡 Grow(1) 等宽；行 CrossAlign(Stretch)
// 等高；末行以空 Row 占位补齐（约定 5：不用 Spacer），所有卡同宽。
constexpr float kNodeGridGap = 8.0F;

[[huxerui::composable]] huxerui::View NodeGrid(
    const ProxyGroup& group, std::size_t cols,
    huxerui::State<std::unordered_map<std::string, int>> delays,
    huxerui::State<std::unordered_map<std::string, bool>> timeouts,
    huxerui::TaskScope tasks, huxerui::State<std::vector<ProxyGroup>> groups,
    huxerui::State<std::vector<std::string>> navPath) {
    const std::vector<ProxyNode>& nodes = group.nodes;
    const std::string groupName = group.name;
    std::vector<huxerui::View> rows;
    for (std::size_t begin = 0; begin < nodes.size(); begin += cols) {
        const std::size_t end = std::min(begin + cols, nodes.size());
        std::vector<huxerui::View> cells;
        for (std::size_t i = begin; i < end; ++i) {
            const ProxyNode& node = nodes[i];
            const std::string nodeName = node.name;
            // 分支节点：先选中到当前组（selectProxy），再进入浏览（路径入栈）。
            // 叶子节点：常规切换。路径写在任务协程里（点击节点会随网格换组卸载）。
            std::function<void()> onSelect;
            if (node.isGroup) {
                onSelect = [tasks, groups, navPath, groupName, nodeName] {
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        co_await RunOnTaskThread([=] {
                            store::coreStore().api().selectProxy(groupName,
                                                                 nodeName);
                        });
                        std::vector<std::string> p =
                            resolvePath(groups.Get(), navPath.Get());
                        if (p.empty() || p.back() != groupName) co_return;
                        p.push_back(nodeName);
                        navPath = p;
                    });
                };
            } else {
                onSelect = [tasks, groupName, nodeName] {
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        co_await RunOnTaskThread([=] {
                            store::coreStore().api().selectProxy(groupName,
                                                                 nodeName);
                        });
                    });
                };
            }
            cells.push_back(
                NodeCard(node, node.name == group.now, delays, timeouts,
                         std::move(onSelect))
                    .Key(node.name));
        }
        for (std::size_t i = end; i < begin + cols; ++i) {
            cells.push_back(huxerui::Row{}.With(huxerui::Grow(1.0F)));
        }
        rows.push_back(
            huxerui::Row(std::move(cells))
                .With(huxerui::Spacing(kNodeGridGap),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
                .Key(nodes[begin].name));
    }
    return huxerui::Column(std::move(rows))
        .With(huxerui::Spacing(kNodeGridGap),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// 分组 chips 横向条（订阅自带的分组）：选中 chip primary 底，GLOBAL 沉底
// （parseProxies 已排序）。
constexpr float kChipHeight = 32.0F;
constexpr float kChipGap = 8.0F;

[[huxerui::composable]] huxerui::View GroupChipBar(
    const std::vector<ProxyGroup>& groups, const std::string& selected,
    huxerui::State<std::vector<std::string>> navPath, huxerui::TaskScope tasks) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    std::vector<huxerui::View> chips;
    for (const auto& g : groups) {
        const bool active = g.name == selected;
        const std::string name = g.name;
        chips.push_back(
            huxerui::Row {
                huxerui::Text(g.name).Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kChip),
                    active ? theme.colors.on_primary
                           : theme.colors.on_surface}),
            }
                .With(huxerui::Padding(
                          huxerui::EdgeInsets::Symmetric(12.0F, 0.0F)),
                      huxerui::Background(active ? theme.colors.primary
                                                 : islands.raised),
                      huxerui::CornerRadius(islands.nested_radius),
                      huxerui::Frame{.height = kChipHeight},
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                      huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                         .label = g.name},
                      huxerui::Focusable(true))
                .OnClick([tasks, navPath, name] {
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        co_await huxerui::Delay(
                            std::chrono::duration<double>{0});
                        navPath = std::vector<std::string>{name};
                    });
                })
                .Key(g.name));
    }
    return huxerui::ScrollView(
               huxerui::Row(std::move(chips))
                   .With(huxerui::Spacing(kChipGap)))
        .ScrollAxis(huxerui::Axis::Horizontal);
}

// 底部嵌套分支导航：‹ 返回（根层级禁用）+ 面包屑路径。
[[huxerui::composable]] huxerui::View BranchBar(
    const std::vector<std::string>& path, std::size_t nodeCount,
    huxerui::State<std::vector<std::string>> navPath, huxerui::TaskScope tasks) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);

    std::string breadcrumb;
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (i > 0) breadcrumb += " / ";
        breadcrumb += path[i];
    }

    huxerui::View back;
    if (path.size() > 1) {
        back = huxerui::Row {
            huxerui::Text("‹").Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody),
                theme.colors.on_surface}),
            huxerui::Text("返回").Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kChip),
                theme.colors.on_surface}),
        }
            .With(huxerui::Spacing(4.0F),
                  huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 4.0F)),
                  huxerui::Background(islands.active),
                  huxerui::CornerRadius(islands.nested_radius),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                  huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                     .label = "返回上一级分支"},
                  huxerui::Focusable(true))
            .OnClick([tasks, navPath] {
                tasks.Launch([=]() -> huxerui::Task<void> {
                    // 返回后本按钮可能随层级收起被卸载：先让出一拍再写 State。
                    co_await huxerui::Delay(std::chrono::duration<double>{0});
                    std::vector<std::string> p = navPath.Get();
                    if (p.size() <= 1) co_return;
                    p.pop_back();
                    navPath = p;
                });
            });
    } else {
        back = huxerui::Row{};
    }

    return huxerui::Row {
        std::move(back),
        huxerui::Text(breadcrumb).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kCaption),
            theme.colors.on_surface_variant}),
        huxerui::Spacer(),
        huxerui::Text(std::format("{} 节点", nodeCount))
            .Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption),
                theme.colors.on_surface_variant}),
    }
        .With(huxerui::Spacing(8.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

} // namespace

[[huxerui::composable]] huxerui::View ProxiesPage() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto groups = huxerui::UseState<std::vector<ProxyGroup>>({});
    auto coreState = huxerui::UseState<core::CoreState>(core::CoreState::Stopped);
    auto mode = huxerui::UseState<std::string>("rule");
    auto delays = huxerui::UseState<std::unordered_map<std::string, int>>({});
    auto timeouts = huxerui::UseState<std::unordered_map<std::string, bool>>({});
    auto testingGroup = huxerui::UseState<std::string>("");
    // 分支路径：path[0] = 选中组（chips），后续 = 逐级点入的嵌套子组。
    auto navPath = huxerui::UseState<std::vector<std::string>>({});

    // 数据泵：内核 Running 时每 3s 刷一次 /proxies（保持 now/history 新鲜）；
    // 非 Running 清空列表。
    huxerui::Lifecycle(
        [tasks, groups, coreState, mode] {
            tasks.Launch([=]() -> huxerui::Task<void> {
                for (;;) {
                    const auto snap = store::coreStore().snapshot();
                    coreState = snap.state;
                    if (!snap.mode.empty()) mode = snap.mode;
                    if (snap.state == core::CoreState::Running) {
                        const auto r = co_await RunOnTaskThread([] {
                            return store::coreStore().api().proxies();
                        });
                        if (r.ok) groups = parseProxies(r.body);
                        co_await huxerui::Delay(std::chrono::duration<double>{3.0});
                    } else {
                        if (!groups.Get().empty()) groups = {};
                        co_await huxerui::Delay(std::chrono::duration<double>{0.5});
                    }
                }
            });
            return [] {};
        },
        0);

    // 顶部收束区：出站模式切换（同首页 SegmentedButton）+ 分组 chips。
    std::size_t modeIndex = 0;
    for (std::size_t i = 0; i < kModes.size(); ++i) {
        if (mode.Get() == kModes[i]) modeIndex = i;
    }
    huxerui::View modeSwitch =
        huxerui::SegmentedButton(kModeNames, modeIndex)
            .OnChanged([tasks, toast, mode](std::size_t idx) {
                tasks.Launch([=]() -> huxerui::Task<void> {
                    const bool ok = co_await RunOnTaskThread(
                        [idx] { return store::coreStore().applyMode(kModes[idx]); });
                    if (!ok) {
                        toast.Show("切换失败（内核未运行？）");
                    } else {
                        mode = kModes[idx];  // 乐观更新，免等下一泵
                    }
                });
            });

    // 渲染期校验路径：根组消失回落第一个组；嵌套前缀逐级校验。
    const std::vector<ProxyGroup>& all = groups.Get();
    std::vector<std::string> path = resolvePath(all, navPath.Get());
    if (path.empty() && !all.empty()) path = {all.front().name};
    const ProxyGroup* current = path.empty() ? nullptr : findGroup(all, path.back());
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    const std::size_t cols = compact ? 2
                             : huxerui::UseViewportClass() ==
                                     huxerui::ViewportClass::Medium
                                 ? 3
                                 : 4;

    // 网格区：有组 → 选中组的统一节点网格（滚动）；无组 → 原提示。
    huxerui::View gridArea;
    if (current != nullptr) {
        gridArea = huxerui::ScrollView(
                       NodeGrid(*current, cols, delays, timeouts, tasks, groups,
                                navPath))
            .With(huxerui::Grow(1.0F));
    } else {
        gridArea = huxerui::Column {
            huxerui::Text(coreState.Get() == core::CoreState::Running
                              ? "暂无策略组（检查订阅配置）"
                              : "内核未运行 —— 请到设置页启动内核")
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kBody),
                    theme.colors.on_surface_variant}),
        }
            .With(huxerui::Padding(32.0F),
                  huxerui::Grow(1.0F),
                  huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    }

    huxerui::View content = huxerui::Column {
        huxerui::Row { std::move(modeSwitch), huxerui::Spacer() },
        all.empty() ? huxerui::View{huxerui::Row{}}
                    : huxerui::View{GroupChipBar(all, path.front(), navPath,
                                                 tasks)},
        std::move(gridArea),
        current == nullptr ? huxerui::View{huxerui::Row{}}
                           : huxerui::View{BranchBar(path, current->nodes.size(),
                                                     navPath, tasks)},
    }
        .With(huxerui::Spacing(10.0F),
              huxerui::Grow(1.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    return PageScaffold("代理", huxerui::Row{}, std::move(content));
}

} // namespace clashflux::ui
