// proxies_page.cpp — 代理页：出站模式（规则/全局/直连）按钮挂在「代理」标题
// 行右侧，与标题左右对齐；下面是分组标签栏 + 选中分组的节点卡网格。
//
// 分组切换（标签栏）：一个根分组一个横向标签，纯文字、无边框无填充；选中项
// 高亮文字并在底部画一条主题色加粗指示线。标签过多时标签栏横向滚动；内容区
// 支持左右滑动手势切换相邻分组（移动端）。同一时刻只展示选中分组的节点。
//
// 规则 → 订阅自带分组标签；全局 → GLOBAL 兼容组（或平台回落组）；直连 →
// 不展示订阅内容，只给提示。
//
// 规则/全局两套分支路径 State 独立（rulePath/globalPath），切换模式互不
// 覆盖对方的选择。GLOBAL 组只在全局模式出现，规则列表不含它；Android
// 等仅返回真实策略组的平台，则全局模式回落到第一个可选策略组。
//
// 节点网格：Compact(<600) 两列；桌面按窗口宽度排 1/2/3/4 列，轨道均分铺满
// 页面宽度。嵌套面包屑固定在网格上方，不参与节点滚动。
//
// 分支语义：点组类型节点 = 选中该分支到当前组（selectProxy）并进入浏览；
// 叶子节点 = 常规切换。展开组内出现嵌套时，面包屑行提供返回上级。路径 State
// 只存用户走出的链，渲染期经 resolvePath 对最新 groups 校验裁剪，订阅刷新
// 导致组消失时自动回落，无需泵写状态。
//
// 延迟测试：页面右下角统一一个悬浮测速按钮（测试当前分组），组头不再各自
// 挂按钮；Compact 悬浮导航之上留出内缩量。
//
// 数据流：PollWhile 拉取并在线程池解析 /proxies，只同步有变化的组。测速 / 切节点
// 都是阻塞 REST，全部走 RunOnTaskThread；点击事件处理器内不直接写 State
// （约定 6），只 Launch 协程。
#include <huxerui/huxerui.h>

#include "app_resources.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
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
    std::int64_t urlTestTime = 0;
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
                node.delay = nit->value("urlTestDelay", node.delay);
                node.urlTestTime = nit->value("urlTestTime", std::int64_t{0});
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

void SyncProxyGroups(huxerui::StateList<ProxyGroup> groups,
                     std::vector<ProxyGroup> next) {
    if (groups.Size() != next.size()) {
        ReplaceStateList(groups, std::move(next));
        return;
    }
    // StateList::Set skips equal values. Preserve unchanged rows and their mounted
    // card state instead of invalidating the whole page on each polling tick.
    for (std::size_t i = 0; i < next.size(); ++i) {
        groups.Set(i, std::move(next[i]));
    }
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

// 单行截断（UTF-8 代码点安全）：超限截断加省略号。HuxerUI Text 默认按词
// 换行且没有省略号能力，节点名撑成两行会让网格行高参差，这里统一单行化。
std::string truncateOneLine(const std::string& s, std::size_t maxCodePoints) {
    std::size_t count = 0;
    std::size_t i = 0;
    while (i < s.size()) {
        if (count == maxCodePoints) return s.substr(0, i) + "…";
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const std::size_t len =
            c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        i += std::min(len, s.size() - i);
        ++count;
    }
    return s;
}

// 名称行固定高度：配合外层 ClipChildren，截断后仍超宽时也不会折成第二行。
constexpr float kNodeLineHeight = 20.0F;

// 代理卡（组头卡与节点卡）圆角：8px，比默认二级岛的 14px 更方。
constexpr float kProxyCardRadius = 8.0F;

// 节点卡自适应列的最小宽度：值越小同宽窗口下卡片越窄（列数更多）。
constexpr float kProxyNodeWidth = 300.0F;

// 分组标签的底部指示线高度（对齐 apitab 请求页分区条：选中态主色下划线）；
// 未选中标签画同高透明线，让整条标签栏高度稳定。
constexpr float kTabIndicatorHeight = 2.0F;

// 左右滑动切换分组：先按水平位移认领指针会话（超过认领阈值且横向分量占优），
// 松手时位移超过提交阈值才真的换组——纵向滚动因此不会被误判成滑动。
constexpr float kGroupSwipeClaimDistance = 20.0F;
constexpr float kGroupSwipeCommitDistance = 56.0F;

// 节点矩形卡：内部左右对齐——左侧名称单行（溢出省略号），右侧写延迟
// （组类型节点写「组·分支当前选中」），延迟按区间着色；选中态 primary 底。
// 宽度由 VirtualGrid 均分，高度由 EstimatedRowExtent 提供估计。
[[huxerui::composable]] huxerui::View NodeCard(
    const ProxyNode& node, bool selected, const std::string& groupName,
    huxerui::State<int> testGeneration, huxerui::State<std::string> testGroup,
    bool interactive, std::function<void()> onSelect, std::size_t nameLimit) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    auto tasks = huxerui::UseTaskScope();
    auto probe = huxerui::UseState(ProbeState{
        .delay = node.delay, .timeout = node.timeout, .testing = false});
    auto lastGeneration = huxerui::UseState(testGeneration.Get());
    auto testBaselineTime =
        huxerui::UseState<std::int64_t>(std::int64_t{node.urlTestTime});
    const std::string nodeName = node.name;
    const bool nativeGroupTest = ProxyTestUsesNativeGroup();
    // 测速请求只作为广播事件；真正的内核节点请求、状态和完成时机都归卡片自己。
    // 卡片被 VirtualGrid 回收时，TaskScope 会取消自己的请求，不影响其他卡片。
    huxerui::Lifecycle(
        [tasks, probe, lastGeneration, testBaselineTime, testGeneration,
         testGroup, groupName, nativeGroupTest, nodeDelay = node.delay,
         nodeTestTime = node.urlTestTime, nodeName] {
            const bool newRequest =
                testGeneration.Get() != 0 && testGroup.Get() == groupName &&
                testGeneration.Get() != lastGeneration.Get() &&
                !probe.Get().testing;
            if (newRequest) {
                ProbeState started = probe.Get();
                started.delay = 0;
                started.timeout = false;
                started.testing = true;
                probe = started;
                lastGeneration = testGeneration.Get();
                testBaselineTime = nodeTestTime;
                if (!nativeGroupTest) {
                    tasks.Launch([probe, nodeName]() -> huxerui::Task<void> {
                        try {
                            const int measured = co_await RunOnTaskThread([nodeName] {
                                const auto result = store::coreStore().api().proxyDelay(
                                    nodeName,
                                    "http://connectivitycheck.gstatic.com/generate_204",
                                    3000);
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
                } else {
                    // libbox 失败时可能不会更新时间戳；本地结束 UI 状态，
                    // 避免某个节点永久停留在“测速中”。
                    tasks.Launch([probe]() -> huxerui::Task<void> {
                        for (int attempt = 0; attempt < 120; ++attempt) {
                            co_await huxerui::Delay(
                                std::chrono::duration<double>{0.25});
                            if (!probe.Get().testing) co_return;
                        }
                        ProbeState timedOut = probe.Get();
                        timedOut.delay = 0;
                        timedOut.timeout = true;
                        timedOut.testing = false;
                        probe = timedOut;
                    });
                }
            }

            if (nativeGroupTest) {
                const ProbeState current = probe.Get();
                if (current.testing && nodeTestTime != testBaselineTime.Get()) {
                    ProbeState completed = current;
                    completed.delay = nodeDelay > 0 ? nodeDelay : 0;
                    completed.timeout = nodeDelay <= 0;
                    completed.testing = false;
                    probe = completed;
                } else if (!current.testing &&
                           (current.delay != nodeDelay ||
                            current.timeout != (nodeDelay <= 0))) {
                    ProbeState snapshot = current;
                    snapshot.delay = nodeDelay;
                    snapshot.timeout = nodeDelay <= 0;
                    probe = snapshot;
                }
                return;
            }
        },
        testGeneration, testGroup, node.delay, node.urlTestTime);

    const ProbeState currentProbe = probe.Get();
    const int delay = currentProbe.delay;
    const bool timeout = currentProbe.timeout;

    std::string meta;
    if (node.isGroup) {
        const std::string inner =
            node.groupNow.empty() ? node.type : node.groupNow;
        meta = "组·" + truncateOneLine(inner, 8);
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
    // 延迟/组信息统一放右侧并着色；名称占满剩余宽度，单行省略号截断。
    const huxerui::Color metaColor =
        selected ? fg
                 : (node.isGroup ? theme.colors.on_surface_variant
                                 : delayColor(theme, delay, timeout));
    return huxerui::Row {
        huxerui::Column {
            huxerui::Text(truncateOneLine(node.name, nameLimit))
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kBody), fg}),
        }.With(huxerui::Frame{.height = kNodeLineHeight},
               huxerui::ClipChildren(),
               huxerui::Grow(1.0F)),
        huxerui::Text(meta).Style(
            huxerui::TextStyle{huxerui::Font::System(font_size::kCaption),
                               metaColor}),
    }
        .With(huxerui::Spacing(8.0F),
              huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 8.0F)),
              huxerui::Background(bg),
              huxerui::CornerRadius(kProxyCardRadius),
              huxerui::Grow(1.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
              huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                 .label = node.name},
              huxerui::Focusable(interactive),
              huxerui::Enabled(interactive))
        .OnClick([onSelect = std::move(onSelect)] { onSelect(); });
}

// 节点网格间隙：组卡内部节点按列排布，取比岛屿缝隙更紧的间距。
constexpr float kNodeGridGap = 8.0F;

// 节点点击动作：分支节点先选中到当前组（selectProxy）再进入浏览（路径入栈）；
// 叶子节点常规切换；URLTest/Fallback 组只允许查看与测速，不允许手动选择。
// 路径写在任务协程里（点击节点会随列表换组卸载）。
std::function<void()> NodeSelectAction(
    huxerui::StateList<ProxyGroup> groups,
    huxerui::State<std::vector<std::string>> activePath, huxerui::TaskScope tasks,
    std::function<void()> onFailure, const ProxyGroup& group,
    const ProxyNode& node) {
    const std::string groupName = group.name;
    const std::string nodeName = node.name;
    if (!group.selectable) return [] {};
    if (node.isGroup) {
        return [groups, activePath, tasks, onFailure, groupName, nodeName] {
            tasks.Launch([=]() -> huxerui::Task<void> {
                const bool selected = co_await RunOnTaskThread(
                    [=] { return SelectProxyLine(groupName, nodeName); });
                if (!selected) {
                    if (onFailure) onFailure();
                    co_return;
                }
                std::vector<std::string> p =
                    resolvePath(groups, activePath.Get());
                // 根组由渲染期回落时，State 可能仍为空或保留了已消失的
                // GLOBAL；以当前已校验的组重新建立路径。
                if (p.empty() || p.back() != groupName) p = {groupName};
                p.push_back(nodeName);
                activePath = p;
            });
        };
    }
    return [tasks, onFailure, groupName, nodeName] {
        tasks.Launch([=]() -> huxerui::Task<void> {
            const bool selected = co_await RunOnTaskThread(
                [=] { return SelectProxyLine(groupName, nodeName); });
            if (!selected && onFailure) onFailure();
        });
    };
}

// 分组标签栏：横向纯文本标签，无外框、无填充——直接沿用 apitab 请求页分区条
// （Auth/Params/Headers/Cookies/Body/设置，FlatSelectRow 的 Underline 样式）：
// 非选中为次级文字色，选中为主色文字 + 底部 2pt 主色短线；未选中画同高透明线，
// 切换时布局零跳动。"选择中"（hover/press）只叠普通按钮那层填充，与选中态
// 互不混淆。标签过多时整条横向滚动（移动端可直接滑动标签条）。
[[huxerui::composable]] huxerui::View GroupTabBar(
    const std::vector<std::string>& names, const std::string& selected,
    std::function<void(const std::string&)> onSelect) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    huxerui::Color hoverFill = theme.colors.on_surface;
    hoverFill.alpha = 0.08F;
    huxerui::Color pressFill = theme.colors.on_surface;
    pressFill.alpha = 0.14F;
    const huxerui::Indication indication{
        .hover = huxerui::IndicationLayer{
            .fill = huxerui::VisualFill{huxerui::Brush{hoverFill}}},
        .press = huxerui::IndicationLayer{
            .fill = huxerui::VisualFill{huxerui::Brush{pressFill}}},
    };

    std::vector<huxerui::View> tabs;
    tabs.reserve(names.size());
    for (const std::string& name : names) {
        const bool active = name == selected;
        const huxerui::Color labelColor =
            active ? theme.colors.primary : theme.colors.on_surface_variant;
        tabs.push_back(
            huxerui::Column {
                huxerui::Text(name, huxerui::TextRole::Label)
                    .With(huxerui::Foreground(labelColor)),
                // 两态同高的短线：选中显示主色，未选中透明占位，无布局跳动。
                huxerui::Row{}.With(
                    huxerui::Frame{.height = kTabIndicatorHeight},
                    active ? huxerui::Background(theme.colors.primary)
                           : huxerui::Background(huxerui::Color::Transparent()),
                    huxerui::CornerRadius(theme.shapes.full)),
            }
                .With(huxerui::Spacing(3.0F),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
                .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(8.0F, 4.0F)),
                      // 未选中用透明描边（宽度恒为 1pt）保住几何，避免切换时
                      // 整项尺寸跳动；本样式不画框，描边始终透明。
                      huxerui::Border(huxerui::Color::Transparent(), 1.0F),
                      huxerui::CornerRadius(theme.shapes.small),
                      indication,
                      huxerui::Focusable(true),
                      huxerui::Semantics{.role = huxerui::SemanticRole::Tab,
                                         .label = name,
                                         .selected = active})
                .OnClick([onSelect, name] { onSelect(name); })
                .Key("group-tab-" + name));
    }
    return huxerui::ScrollView(
               huxerui::Row(std::move(tabs))
                   .With(huxerui::Spacing(theme.spacing.extra_small),
                         huxerui::CrossAlign(
                             huxerui::CrossAxisAlignment::Center)))
        .ScrollAxis(huxerui::Axis::Horizontal)
        .With(huxerui::ClipChildren());
}

// 展开组内的嵌套面包屑：路径 + 返回上级（与旧底部状态条同一套弹栈语义）。
[[huxerui::composable]] huxerui::View GroupBreadcrumb(
    const std::vector<std::string>& path,
    huxerui::State<std::vector<std::string>> activePath, huxerui::TaskScope tasks) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);

    std::string breadcrumb;
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (i > 0) breadcrumb += " / ";
        breadcrumb += path[i];
    }

    return huxerui::Row {
        huxerui::Row {
            huxerui::Text("‹").Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody),
                theme.colors.on_surface}),
            huxerui::Text("返回上级").Style(huxerui::TextStyle{
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
            .OnClick([tasks, activePath] {
                tasks.Launch([=]() -> huxerui::Task<void> {
                    // 返回后本按钮可能随层级收起被卸载：先让出一拍再写 State。
                    co_await huxerui::Delay(std::chrono::duration<double>{0});
                    std::vector<std::string> p = activePath.Get();
                    if (p.size() <= 1) co_return;
                    p.pop_back();
                    activePath = p;
                });
            }),
        huxerui::Text(breadcrumb).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kCaption),
            theme.colors.on_surface_variant}),
        huxerui::Spacer(),
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
    auto modePending = huxerui::UseState(false);
    auto testGeneration = huxerui::UseState(0);
    auto testGroup = huxerui::UseState<std::string>("");
    // 分支路径按模式独立（互不共享）：规则模式 path[0] = 标签栏选中的订阅组，
    // 全局模式 path[0] = GLOBAL 或平台返回的实际可选组；后续元素 = 逐级点入的嵌套子组。
    auto rulePath = huxerui::UseState<std::vector<std::string>>({});
    auto globalPath = huxerui::UseState<std::vector<std::string>>({});
    // 横向滑动：按下点与"是否已认领本次指针会话"。
    auto swipeOrigin = huxerui::UseState<huxerui::Point>(huxerui::Point{0.0F, 0.0F});
    auto swipeOwned = huxerui::UseState(false);

    // 数据泵：运行时刷新 /proxies；停止时从当前订阅编译预览快照，
    // 这样用户仍能预先选择节点，下一次内核启动后再由内核正式应用。
    huxerui::Lifecycle(
        [tasks, groups, coreState, mode, modePending] {
            tasks.Launch([=]() -> huxerui::Task<void> {
                for (;;) {
                    const auto snap = store::coreStore().snapshot();
                    coreState = snap.state;
                    if (!modePending.Get() && !snap.mode.empty()) mode = snap.mode;
                    const auto nextGroups = co_await RunOnTaskThread([]()
                        -> std::optional<std::vector<ProxyGroup>> {
                        const std::string body = ProxyGroupsSnapshot();
                        if (body.empty()) return std::nullopt;
                        return parseProxies(body);
                    });
                    if (nextGroups) {
                        SyncProxyGroups(groups, std::move(*nextGroups));
                    }
                    co_await huxerui::Delay(std::chrono::duration<double>{3.0});
                }
            });
            return [] {};
        },
        0);

    // 组测速触发：桌面逐节点调用内核 delay API；Android 调用 libbox
    // urlTest，UI 只消费内核回写的结果。
    const std::function<void(const std::string&)> triggerGroupTest =
        [tasks, toast, testGeneration, testGroup, coreState](
            std::string groupName) {
            tasks.Launch([toast, testGeneration, testGroup, coreState,
                          groupName = std::move(groupName)]() -> huxerui::Task<void> {
                if (coreState.Get() != core::CoreState::Running) {
                    toast.Show("测速需要内核：请先在首页右下角启动内核");
                    co_return;
                }
                const bool ok = co_await RunOnTaskThread([groupName] {
                    return StartProxyGroupTest(groupName);
                });
                if (!ok) {
                    toast.Show("测速失败：内核未运行或启动失败");
                    co_return;
                }
                testGroup = groupName;
                testGeneration += 1;
                co_return;
            });
        };

    // 顶部收束区：出站模式切换（同首页 SegmentedButton），挂到标题行右侧。
    std::size_t modeIndex = 0;
    for (std::size_t i = 0; i < kModes.size(); ++i) {
        if (mode.Get() == kModes[i]) modeIndex = i;
    }
    huxerui::View modeSwitch =
        huxerui::SegmentedButton(kModeNames, modeIndex)
            .OnChanged([tasks, toast, mode, modePending](std::size_t idx) {
                if (idx >= kModes.size() || modePending.Get()) return;
                const std::string previous = mode.Get();
                mode = kModes[idx];
                modePending = true;
                tasks.Launch([=]() -> huxerui::Task<void> {
                    bool ok = false;
                    std::string error;
                    try {
                        ok = co_await RunOnTaskThread(
                            [idx] { return store::coreStore().applyMode(kModes[idx]); });
                    } catch (const std::exception& exception) {
                        error = exception.what();
                    }
                    modePending = false;
                    if (!ok) {
                        mode = previous;
                        toast.Show(error.empty() ? "切换失败（内核未运行？）" : error);
                    }
                });
            });

    // 按出站模式解析根分组与当前组（渲染期校验：组消失回落，嵌套前缀逐级
    // 校验）。直连不经过节点，不展示订阅组。
    const huxerui::StateList<ProxyGroup> all = groups;
    const bool direct = mode.Get() == "direct";
    const bool global = mode.Get() == "global";
    const ProxyGroup* globalRoot = findGroup(all, "GLOBAL");
    bool hasRuleSelector = false;
    for (const auto& g : all) {
        if (g.name == "GLOBAL") continue;
        if (g.selectable) {
            hasRuleSelector = true;
            if (globalRoot == nullptr) globalRoot = &g;
        }
    }
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

    // 标签栏的根分组集合：规则模式列出订阅自带分组（有可选组时跳过纯 URLTest
    // 分支，避免与作为其父级的 selector 重复）；全局模式只保留 GLOBAL（或
    // 平台回落组）。直连不走任何组。
    std::vector<const ProxyGroup*> rootGroups;
    if (global) {
        if (globalRoot != nullptr) rootGroups.push_back(globalRoot);
    } else if (!direct) {
        for (const auto& g : all) {
            if (g.name == "GLOBAL") continue;
            if (hasRuleSelector && !g.selectable) continue;
            rootGroups.push_back(&g);
        }
    }
    std::vector<std::string> tabNames;
    tabNames.reserve(rootGroups.size());
    for (const ProxyGroup* g : rootGroups) tabNames.push_back(g->name);

    // 活动路径：按模式取独立 State，渲染期校验（组消失回落，嵌套前缀逐级
    // 校验）。标签栏是分组入口：路径必须落在某个标签上，否则回落到首个标签；
    // 嵌套层级（path.size() > 1）由面包屑返回。
    huxerui::State<std::vector<std::string>> activePath =
        global ? globalPath : rulePath;
    std::vector<std::string> path = resolvePath(all, activePath.Get());
    const bool rootSelected =
        !path.empty() && std::ranges::any_of(rootGroups, [&](const ProxyGroup* g) {
            return g->name == path.front();
        });
    if (!rootSelected) {
        path = tabNames.empty() ? std::vector<std::string>{}
                                : std::vector<std::string>{tabNames.front()};
    }
    const ProxyGroup* current =
        path.empty() ? nullptr : findGroup(all, path.back());
    const std::string selectedRoot = path.empty() ? std::string{} : path.front();
    std::size_t selectedTab = tabNames.size();
    for (std::size_t i = 0; i < tabNames.size(); ++i) {
        if (tabNames[i] == selectedRoot) selectedTab = i;
    }

    // 分组标签与左右滑动直接更新路径，让选中态和内容在同一帧切换。
    const std::function<void(const std::string&)> selectGroup =
        [activePath](const std::string& name) {
            activePath = std::vector<std::string>{name};
        };

    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    // 节点名单行预算：定宽卡下留出右侧延迟与内边距后的可用字符数。
    const std::size_t nodeNameLimit = compact ? 9 : 16;

    // 一个根分组一页。VirtualGrid 直接用索引读取组节点，不再为所有组预先
    // 分配完整的 ProxyItem 与 span 数组；只有视口附近的节点会被构造为卡片。
    // IndexedPages 保留各组页面和滚动位置，只测量当前组，避免切组时并行布局
    // 离屏网格；滑动手势在内容区自行识别。
    constexpr std::size_t kCompactFooterItems = 2;
    std::vector<huxerui::View> groupPages;
    groupPages.reserve(rootGroups.size());
    for (std::size_t page = 0; page < rootGroups.size(); ++page) {
        const ProxyGroup& rootGroup = *rootGroups[page];
        // 只有当前页可能是嵌套子组（path 更深）；其余页就是各自的根分组。
        const bool selectedPage = page == selectedTab;
        const ProxyGroup* contentGroup =
            (selectedPage && current != nullptr) ? current : &rootGroup;
        const std::vector<std::string> pagePath =
            (selectedPage && current != nullptr) ? path
                                                 : std::vector<std::string>{rootGroup.name};

        const std::string contentGroupName = contentGroup->name;
        const std::size_t nodeCount = contentGroup->nodes.size();
        const std::size_t footerCount = compact ? kCompactFooterItems : 0;
        huxerui::View grid = huxerui::VirtualGrid(
                                 nodeCount + footerCount,
                                 [groups, activePath, testGeneration, testGroup,
                                  tasks, toast, nodeNameLimit, contentGroupName,
                                  nodeCount, compact](std::size_t index)
                                     -> huxerui::View {
                                     if (index >= nodeCount) {
                                         if (!compact) return huxerui::View{};
                                         return CompactFloatingNavigationFooter()
                                             .Key("compact-floating-footer-" +
                                                  std::to_string(index - nodeCount));
                                     }
                                     const ProxyGroup* group =
                                         findGroup(groups, contentGroupName);
                                     if (group == nullptr ||
                                         index >= group->nodes.size()) {
                                         return huxerui::View{};
                                     }
                                     const ProxyNode& node = group->nodes[index];
                                     const std::string nodeName = node.name;
                                     return NodeCard(
                                                node, nodeName == group->now,
                                                group->name, testGeneration,
                                                testGroup, group->selectable,
                                                NodeSelectAction(groups, activePath,
                                                                 tasks,
                                                                 [toast] {
                                                                     toast.Show(
                                                                         "线路切换失败，请查看应用日志");
                                                                 },
                                                                 *group, node),
                                                nodeNameLimit)
                                         .Key(group->name + "::" + nodeName);
                                 })
                                 .Columns(compact
                                              ? huxerui::GridColumns::Fixed(2)
                                              : huxerui::GridColumns::Adaptive(
                                                    kProxyNodeWidth))
                                 .EstimatedRowExtent(52.0F)
                                 .RowSpacing(kNodeGridGap)
                                 .ColumnSpacing(kNodeGridGap)
                                 .With(huxerui::Grow(1.0F),
                                       huxerui::ScrollBar())
                                 .Key("group-grid-" + rootGroup.name)
                                 // Intercept lives on the scroll node itself.
                                 // PointerIntercept is resolved deepest-first;
                                 // on this node it runs before the grid's own
                                 // vertical-scroll recognizer, so horizontal
                                 // swipes can claim the sequence while vertical
                                 // movement remains available to the list.
                                 .On<huxerui::ViewEvents::PointerIntercept>(
                                     [swipeOrigin, swipeOwned, selectGroup,
                                      tabNames, selectedTab](
                                         const huxerui::PointerEvent& event) {
                                         const float dx = event.position.x -
                                                          swipeOrigin.Get().x;
                                         const float dy = event.position.y -
                                                          swipeOrigin.Get().y;
                                         switch (event.type) {
                                         case huxerui::PointerEventType::Down:
                                             swipeOrigin = event.position;
                                             swipeOwned = false;
                                             return false;
                                         case huxerui::PointerEventType::Move:
                                             if (swipeOwned.Get()) return true;
                                             if (std::abs(dx) >
                                                     kGroupSwipeClaimDistance &&
                                                 std::abs(dx) > std::abs(dy)) {
                                                 swipeOwned = true;
                                                 return true;
                                             }
                                             return false;
                                         case huxerui::PointerEventType::Up:
                                             if (!swipeOwned.Get()) return false;
                                             swipeOwned = false;
                                             if (dx <= -kGroupSwipeCommitDistance &&
                                                 selectedTab + 1 < tabNames.size()) {
                                                 selectGroup(tabNames[selectedTab + 1]);
                                             } else if (
                                                 dx >= kGroupSwipeCommitDistance &&
                                                 selectedTab > 0) {
                                                 selectGroup(tabNames[selectedTab - 1]);
                                             }
                                             return false;
                                         case huxerui::PointerEventType::Cancel:
                                             swipeOwned = false;
                                             return false;
                                         }
                                         return false;
                                     });

        std::vector<huxerui::View> pageContent;
        if (pagePath.size() > 1) {
            pageContent.push_back(GroupBreadcrumb(pagePath, activePath, tasks));
        }
        pageContent.push_back(std::move(grid));
        groupPages.push_back(
            huxerui::Column(std::move(pageContent))
                .With(huxerui::Spacing(pagePath.size() > 1
                                           ? theme.spacing.small
                                           : 0.0F),
                      huxerui::Grow(1.0F),
                      huxerui::CrossAlign(
                          huxerui::CrossAxisAlignment::Stretch),
                      huxerui::Transition{
                          huxerui::AnimateTo(
                              selectedPage ? 1.0F : 0.0F,
                              huxerui::TweenSpec{
                                  .duration = theme.motion.reduced_motion
                                                  ? 0.0
                                                  : theme.motion.normal,
                                  .easing = huxerui::Easing::EaseOut})}
                          .Opacity(0.82F, 1.0F)
                          .Offset({12.0F, 0.0F}, {}))
                .Key("group-page-" + rootGroup.name));
    }

    huxerui::View body;
    if (direct) {
        body = huxerui::Column {
            huxerui::Text("直连模式 —— 流量不经过任何代理节点")
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kBody),
                    theme.colors.on_surface_variant}),
        }
            .With(huxerui::Padding(32.0F),
                  huxerui::Grow(1.0F),
                  huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    } else if (rootGroups.empty()) {
        body = huxerui::Column {
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
    } else {
        body = huxerui::IndexedPages(std::move(groupPages), selectedTab)
                   .With(huxerui::Grow(1.0F));
    }

    // 标签栏固定在页面顶部（不随节点列表滚动），只有选中分组的节点参与滚动。
    huxerui::View content = std::move(body);
    if (!tabNames.empty()) {
        content =
            huxerui::Column {
                GroupTabBar(tabNames, selectedRoot, selectGroup),
                std::move(content).With(huxerui::Grow(1.0F)),
            }
                .With(huxerui::Spacing(theme.spacing.small),
                      huxerui::Grow(1.0F),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }

    // 出站模式按钮与「代理」标题同处标题行、左右对齐。
    huxerui::View page = PageScaffold("代理", std::move(modeSwitch),
                                      std::move(content), true);
    // 延迟测试按钮统一收在页面右下角：测试当前选中的分组。Compact 下要避开
    // 悬浮底部导航，桌面只留常规外边距。
    if (!direct && current != nullptr) {
        const std::string groupName = current->name;
        huxerui::View floatingSpeed =
            huxerui::IconButton(app::images::speed, "测速")
                .With(huxerui::Tooltip("测试当前分组延迟"),
                      huxerui::Frame{.width = 56.0F, .height = 56.0F},
                      huxerui::Background(theme.colors.primary),
                      huxerui::Foreground(theme.colors.on_primary),
                      huxerui::CornerRadius(28.0F),
                      huxerui::Shadow{huxerui::Color::Rgb(0, 0, 0, 0.28F),
                                      {}, 14.0F, 2.0F},
                      huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                         .label = "测速"})
                .OnClick([triggerGroupTest, groupName] {
                    triggerGroupTest(groupName);
                });
        floatingSpeed = WithoutIconButtonOutlines(floatingSpeed);
        // 与 Android 底部悬浮导航栏相同：按钮放在独立的全屏覆盖层中，
        // 由覆盖层的 Column 在主轴末端、交叉轴末端定位，不依赖页面内容容器。
        huxerui::View speedDock = huxerui::Column {
            std::move(floatingSpeed),
        }.With(huxerui::Padding(huxerui::EdgeInsets{
                   .right = theme.spacing.medium,
                   .bottom = compact ? kCompactFloatingNavigationInset
                                     : theme.spacing.large,
                   .left = theme.spacing.medium,
               }),
               huxerui::MainAlign(huxerui::MainAxisAlignment::End),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::End));
        return huxerui::Stack {
            std::move(page),
            std::move(speedDock),
        }.With(huxerui::Grow(1.0F),
               huxerui::Align(huxerui::HorizontalAlignment::Stretch,
                              huxerui::VerticalAlignment::Stretch));
    }
    return page;
}

} // namespace clashflux::ui
