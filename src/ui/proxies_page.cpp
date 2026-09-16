// proxies_page.cpp — 代理页：顶部收束区（出站模式 规则/全局/直连 切换）+
// 按模式分视图：规则 → 订阅自带分组 chips + 选中组的统一矩形节点卡网格；
// 全局 → GLOBAL 兼容组或平台提供的实际可选组节点网格；直连 → 不展示订阅内容。
// 底部状态条：嵌套分支导航（面包屑 + 返回）+ 当前组测速触发 + 节点数。
//
// 规则/全局两套分支路径 State 独立（rulePath/globalPath），切换模式互不
// 覆盖对方的选择。GLOBAL 组只在全局模式出现，规则 chips 不含它；Android
// 等仅返回真实策略组的平台，则全局模式回落到第一个可选策略组。
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

#include "app_resources.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
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
    bool selectable = false;
    std::vector<ProxyNode> nodes;

    bool operator==(const ProxyGroup&) const = default;
};

struct ProbeState {
    int delay = 0;
    bool timeout = false;
    bool testing = false;

    bool operator==(const ProbeState&) const = default;
};

// 组类型：带 all 列表的才是策略组（Selector/URLTest/Fallback/LoadBalance/Relay），
// 其余（Direct/Reject/具体协议节点）不进组列表。
bool isGroupType(const std::string& t) {
    return t == "Selector" || t == "selector" || t == "URLTest" ||
           t == "urltest" || t == "Fallback" || t == "fallback" ||
           t == "LoadBalance" || t == "loadbalance" || t == "Relay" ||
           t == "relay";
}

bool isSelectorType(const std::string& t) {
    return t == "Selector" || t == "selector";
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
        g.selectable = v.value("selectable", isSelectorType(type));
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
    // GLOBAL 组太长且无意义时沉底（clash_api 兼容组，列出全部节点）。
    std::ranges::stable_sort(groups, [](const ProxyGroup& a, const ProxyGroup& b) {
        return (a.name == "GLOBAL") < (b.name == "GLOBAL");
    });
    return groups;
}

const ProxyGroup* findGroup(const huxerui::StateList<ProxyGroup>& groups,
                            const std::string& name) {
    for (const auto& g : groups) {
        if (g.name == name) return &g;
    }
    return nullptr;
}

// 渲染期校验：只保留 navPath 中仍然存在且逐级可达（组类型子节点）的前缀；
// 空输入或根组消失返回空（调用方回落第一个组）。
std::vector<std::string> resolvePath(
    const huxerui::StateList<ProxyGroup>& groups,
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
    return DelayLevelColor(theme, delay, timeout || delay <= 0);
}

// 节点统一矩形卡：名称（正文级）+ 元信息行（延迟着色；组类型节点显示
// 「组 · 分支当前选中」作为可点入提示），选中态 primary 底。
// 宽度由 VirtualGrid 均分，高度由 EstimatedRowExtent 提供估计。
[[huxerui::composable]] huxerui::View NodeCard(
    const ProxyNode& node, bool selected, const std::string& groupName,
    huxerui::State<int> testGeneration, huxerui::State<std::string> testGroup,
    bool interactive, std::function<void()> onSelect) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    auto tasks = huxerui::UseTaskScope();
    auto probe = huxerui::UseState(ProbeState{
        .delay = node.delay, .timeout = node.timeout, .testing = false});
    auto lastGeneration = huxerui::UseState(testGeneration.Get());
    const std::string nodeName = node.name;

    // 测速请求只作为广播事件；真正的 REST 请求、状态和完成时机都归卡片自己。
    // 卡片被 VirtualGrid 回收时，TaskScope 会取消自己的请求，不影响其他卡片。
    huxerui::Lifecycle(
        [tasks, probe, lastGeneration, testGeneration, testGroup, nodeName,
         groupName] {
            if (testGeneration.Get() == 0 || testGroup.Get() != groupName ||
                testGeneration.Get() == lastGeneration.Get() ||
                probe.Get().testing) {
                return;
            }

            ProbeState started = probe.Get();
            started.delay = 0;
            started.timeout = false;
            started.testing = true;
            probe = started;
            lastGeneration = testGeneration.Get();
            tasks.Launch([probe, nodeName]() -> huxerui::Task<void> {
                try {
                    const int measured = co_await RunOnTaskThread([nodeName] {
                        const auto result = store::coreStore().api().proxyDelay(
                            nodeName, "https://www.gstatic.com/generate_204", 3000);
                        if (!result.ok) return 0;
                        const auto body = nlohmann::json::parse(
                            result.body, nullptr, false);
                        return body.is_object() ? body.value("delay", 0) : 0;
                    });

                    ProbeState completed = probe.Get();
                    completed.delay = measured > 0 ? measured : 0;
                    completed.timeout = measured <= 0;
                    completed.testing = false;
                    probe = completed;
                } catch (const std::exception&) {
                    ProbeState failed = probe.Get();
                    failed.delay = 0;
                    failed.timeout = true;
                    failed.testing = false;
                    probe = failed;
                }
            });
        },
        testGeneration, testGroup);

    const ProbeState currentProbe = probe.Get();
    const int delay = currentProbe.delay;
    const bool timeout = currentProbe.timeout;

    std::string meta;
    if (node.isGroup) {
        meta = "组 · " + (node.groupNow.empty() ? node.type : node.groupNow);
    } else {
        meta = currentProbe.testing ? "测速中…"
                       : (delay > 0 ? std::format("{} ms", delay) : "超时");
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
              huxerui::Focusable(interactive),
              huxerui::Enabled(interactive))
        .OnClick([onSelect = std::move(onSelect)] { onSelect(); });
}

// 节点统一网格：交给 VirtualGrid 按 cols 虚拟化，只创建视口附近的节点卡。
constexpr float kNodeGridGap = 8.0F;

[[huxerui::composable]] huxerui::View NodeGrid(
    const ProxyGroup& group, std::size_t cols, huxerui::State<int> testGeneration,
    huxerui::State<std::string> testGroup, huxerui::TaskScope tasks,
    huxerui::StateList<ProxyGroup> groups,
    huxerui::State<std::vector<std::string>> navPath) {
    const std::size_t nodeCount = group.nodes.size();
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    const std::string groupName = group.name;
    const std::string selectedName = group.now;
    return huxerui::VirtualGrid(
               nodeCount + (compact ? 1U : 0U),
               [groups, selectedName, groupName, testGeneration, testGroup, tasks,
                navPath, compact, nodeCount](std::size_t index) -> huxerui::View {
                   if (compact && index == nodeCount) {
                       return CompactFloatingNavigationFooter()
                           .Key("compact-floating-footer");
                   }
                   const ProxyGroup* current = findGroup(groups, groupName);
                   if (current == nullptr || index >= current->nodes.size()) {
                       return huxerui::View{};
                   }
                   const ProxyNode node = current->nodes[index];
                   const std::string nodeName = node.name;
                   // 分支节点：先选中到当前组（selectProxy），再进入浏览（路径入栈）。
                   // 叶子节点：常规切换。路径写在任务协程里（点击节点会随网格换组卸载）。
                   std::function<void()> onSelect;
                   if (!current->selectable) {
                       // URLTest/Fallback groups expose their members for
                       // inspection and per-card delay, but libbox rejects
                       // manual selection on them.
                       onSelect = [] {};
                   } else if (node.isGroup) {
                       onSelect = [tasks, groups, navPath, groupName, nodeName] {
                           tasks.Launch([=]() -> huxerui::Task<void> {
                               co_await RunOnTaskThread([=] {
                                   SelectProxyLine(groupName, nodeName);
                               });
                               std::vector<std::string> p =
                                   resolvePath(groups, navPath.Get());
                               // 根组由渲染期回落时，State 可能仍为空或保留了
                               // 已消失的 GLOBAL；以当前已校验的组重新建立路径。
                               if (p.empty() || p.back() != groupName) {
                                   p = {groupName};
                               }
                               p.push_back(nodeName);
                               navPath = p;
                           });
                       };
                   } else {
                       onSelect = [tasks, groupName, nodeName] {
                           tasks.Launch([=]() -> huxerui::Task<void> {
                               co_await RunOnTaskThread([=] {
                                   SelectProxyLine(groupName, nodeName);
                               });
                           });
                       };
                   }
                   return NodeCard(node, node.name == selectedName, groupName,
                                   testGeneration, testGroup, current->selectable,
                                   std::move(onSelect))
                       .Key(groupName + "::" + node.name);
               })
        .Columns(huxerui::GridColumns::Fixed(cols))
        .EstimatedRowExtent(72.0F)
        .RowSpacing(kNodeGridGap)
        .ColumnSpacing(kNodeGridGap)
        .With(huxerui::Grow(1.0F), huxerui::ScrollBar());
}

// 分组 chips 横向条（规则模式：订阅自带的分组，不含 GLOBAL——GLOBAL 属于
// 全局模式视图）：选中 chip primary 底。
constexpr float kChipHeight = 32.0F;
constexpr float kChipGap = 8.0F;

[[huxerui::composable]] huxerui::View GroupChipBar(
    huxerui::StateList<ProxyGroup> groups, const std::string& selected,
    huxerui::State<std::vector<std::string>> navPath, huxerui::TaskScope tasks,
    bool selectorsOnly) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    std::vector<huxerui::View> chips;
    for (const auto& g : groups) {
        if (g.name == "GLOBAL" || (selectorsOnly && !g.selectable)) continue;
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

// 底部状态条：‹ 返回（根层级隐藏）+ 面包屑路径 + 当前组测速触发 + 节点数。
[[huxerui::composable]] huxerui::View BranchBar(
    const std::vector<std::string>& path, const ProxyGroup& group,
    huxerui::State<std::vector<std::string>> navPath,
    huxerui::State<int> testGeneration, huxerui::State<std::string> testGroup,
    huxerui::TaskScope tasks) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    const std::string groupName = group.name;

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
        huxerui::View{huxerui::IconButton(app::images::speed, "测速")
            .With(huxerui::Tooltip("测试当前组延迟"))
            .OnClick(
            [testGeneration, testGroup, groupName] {
                testGroup = groupName;
                testGeneration += 1;
            })},
        huxerui::Text(std::format("{} 节点", group.nodes.size()))
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
    auto groups = huxerui::UseStateList<ProxyGroup>();
    auto coreState = huxerui::UseState<core::CoreState>(core::CoreState::Stopped);
    auto mode = huxerui::UseState<std::string>("rule");
    auto testGeneration = huxerui::UseState(0);
    auto testGroup = huxerui::UseState<std::string>("");
    // 分支路径按模式独立（互不共享）：规则模式 path[0] = chips 选中的订阅组，
    // 全局模式 path[0] = GLOBAL 或平台返回的实际可选组；后续元素 = 逐级点入的嵌套子组。
    auto rulePath = huxerui::UseState<std::vector<std::string>>({});
    auto globalPath = huxerui::UseState<std::vector<std::string>>({});

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
                        const std::string body = co_await RunOnTaskThread([] {
                            return ProxyGroupsSnapshot();
                        });
                        if (!body.empty()) ReplaceStateList(groups, parseProxies(body));
                        co_await huxerui::Delay(std::chrono::duration<double>{3.0});
                    } else {
                        if (!groups.Empty()) groups.Clear();
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

    // 按出站模式取当前视图的路径与组（渲染期校验：根组消失回落，嵌套前缀
    // 逐级校验）。直连不经过节点，不展示订阅组。
    const huxerui::StateList<ProxyGroup> all = groups;
    const bool direct = mode.Get() == "direct";
    const bool global = mode.Get() == "global";
    const ProxyGroup* firstRule = nullptr;
    const ProxyGroup* firstRuleFallback = nullptr;
    const ProxyGroup* globalRoot = findGroup(all, "GLOBAL");
    bool hasRuleSelector = false;
    for (const auto& g : all) {
        if (g.name == "GLOBAL") continue;
        if (firstRuleFallback == nullptr) firstRuleFallback = &g;
        if (g.selectable) {
            hasRuleSelector = true;
            if (firstRule == nullptr) firstRule = &g;
        }
        if (globalRoot == nullptr && g.selectable) globalRoot = &g;
    }
    // URLTest groups are normally children of a selector (for example
    // “节点选择” -> “自动选择”). Showing both as root chips duplicates the same
    // branch. Prefer selector roots, but keep a URLTest-only subscription usable.
    if (firstRule == nullptr) firstRule = firstRuleFallback;
    if (firstRule == nullptr && !all.Empty()) firstRule = &all[0];
    if (globalRoot == nullptr) {
        for (const auto& g : all) {
            if (g.name != "GLOBAL" && g.selectable) {
                globalRoot = &g;
                break;
            }
        }
    }
    if (globalRoot == nullptr && !all.Empty()) {
        for (const auto& g : all) {
            if (g.name != "GLOBAL") {
                globalRoot = &g;
                break;
            }
        }
    }

    std::vector<std::string> path;
    huxerui::State<std::vector<std::string>> activePath = rulePath;
    if (global) {
        activePath = globalPath;
        path = resolvePath(all, globalPath.Get());
        if (path.empty() && globalRoot != nullptr) path = {globalRoot->name};
    } else {
        path = resolvePath(all, rulePath.Get());
        if (path.empty() && firstRule != nullptr) path = {firstRule->name};
    }
    const ProxyGroup* current = path.empty() ? nullptr : findGroup(all, path.back());
    bool hasRuleChips = false;
    for (const auto& g : all) {
        if (g.name != "GLOBAL" && (!hasRuleSelector || g.selectable)) {
            hasRuleChips = true;
            break;
        }
    }
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    const std::size_t cols = compact ? 2
                             : huxerui::UseViewportClass() ==
                                     huxerui::ViewportClass::Medium
                                 ? 3
                                 : 4;

    // 网格区：直连 → 提示不展示订阅；有组 → 选中组的统一节点网格（滚动）。
    huxerui::View gridArea;
    if (direct) {
        gridArea = huxerui::Column {
            huxerui::Text("直连模式 —— 流量不经过任何代理节点")
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kBody),
                    theme.colors.on_surface_variant}),
        }
            .With(huxerui::Padding(32.0F),
                  huxerui::Grow(1.0F),
                  huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    } else if (current != nullptr) {
        gridArea = NodeGrid(*current, cols, testGeneration, testGroup, tasks,
                            groups, activePath);
    } else {
        gridArea = huxerui::Column {
            huxerui::Text(coreState.Get() == core::CoreState::Running
                              ? (global ? "全局模式暂无可用策略组"
                                        : "暂无策略组（检查订阅配置）")
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
        (!direct && !global && hasRuleChips)
            ? huxerui::View{GroupChipBar(all, path.front(), activePath, tasks,
                                         hasRuleSelector)}
            : huxerui::View{huxerui::Row{}},
        std::move(gridArea),
        (!direct && current != nullptr)
            ? huxerui::View{BranchBar(path, *current, activePath, testGeneration,
                                      testGroup, tasks)}
            : huxerui::View{huxerui::Row{}},
    }
        .With(huxerui::Spacing(10.0F),
              huxerui::Grow(1.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    return PageScaffold("代理", huxerui::Row{}, std::move(content));
}

} // namespace clashflux::ui
