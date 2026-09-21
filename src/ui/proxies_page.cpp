// proxies_page.cpp — 代理页：出站模式（规则/全局/直连）按钮挂在「代理」标题
// 行右侧，与标题左右对齐；下方是分组竖向卡片列表，组头整行可点、下拉展开该组
// 的统一矩形节点卡网格，同一时刻只展开一个组。
// 规则 → 订阅自带分组卡；全局 → GLOBAL 兼容组卡（或平台回落组）；直连 →
// 不展示订阅内容，只给提示。
//
// 规则/全局两套分支路径 State 独立（rulePath/globalPath），切换模式互不
// 覆盖对方的选择。GLOBAL 组只在全局模式出现，规则列表不含它；Android
// 等仅返回真实策略组的平台，则全局模式回落到第一个可选策略组。
//
// 节点网格：Compact(<600) 两列；桌面按窗口宽度排 1/2/3/4 列，轨道均分铺满
// 页面宽度。组头与嵌套面包屑占满整行，末行用空占位补齐，保证同一组内所有
// 节点卡同宽。
//
// 分支语义：点组类型节点 = 选中该分支到当前组（selectProxy）并进入浏览；
// 叶子节点 = 常规切换。展开组内出现嵌套时，面包屑行提供返回上级。路径 State
// 只存用户走出的链，渲染期经 resolvePath 对最新 groups 校验裁剪，订阅刷新
// 导致组消失时自动回落，无需泵写状态。
//
// 数据流：PollWhile 每 3s 拉 GET /proxies 解析成组模型写 State。测速 / 切节点
// 都是阻塞 REST，全部走 RunOnTaskThread；点击事件处理器内不直接写 State
// （约定 6），只 Launch 协程。
#include <huxerui/huxerui.h>

#include "app_resources.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
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

// 代理页扁平列表项：组头（可展开）+ 展开组内的嵌套面包屑 + 节点网格项。
// 组头与面包屑占满整行，节点占网格单元；Footer 为 Compact 悬浮导航留位。
enum class ProxyItemKind { GroupHeader, Breadcrumb, Node, Footer };

struct ProxyItem {
    ProxyItemKind kind = ProxyItemKind::Node;
    std::string group;      // GroupHeader：组名；Node：所属组名
    std::size_t nodeIndex = 0;
    bool expanded = false;
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
    const ProxyGroup& group, const ProxyNode& node) {
    const std::string groupName = group.name;
    const std::string nodeName = node.name;
    if (!group.selectable) return [] {};
    if (node.isGroup) {
        return [groups, activePath, tasks, groupName, nodeName] {
            tasks.Launch([=]() -> huxerui::Task<void> {
                co_await RunOnTaskThread(
                    [=] { SelectProxyLine(groupName, nodeName); });
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
    return [tasks, groupName, nodeName] {
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await RunOnTaskThread(
                [=] { SelectProxyLine(groupName, nodeName); });
        });
    };
}

// 分组卡头部：展开箭头 + 组名 + 类型/当前选中/节点数，整行可点切换展开；
// 展开时右侧显示本组测速按钮（结果由组内节点卡广播接收）。
[[huxerui::composable]] huxerui::View GroupCardHeader(
    const ProxyGroup& group, bool expanded, std::function<void()> onTest,
    std::function<void()> onToggle) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);

    std::string detail = group.type.empty() ? "策略组" : group.type;
    if (!group.now.empty()) detail += " · " + group.now;
    detail += std::format(" · {} 节点", group.nodes.size());

    huxerui::View speed;
    if (expanded) {
        speed = huxerui::IconButton(app::images::speed, "测速")
                    .With(huxerui::Tooltip("测试该组延迟"))
                    .OnClick(std::move(onTest));
    }

    return huxerui::Row {
        // 折角箭头：收起为右向「›」，展开为下向「∨」（旋转的小于号样式）。
        huxerui::Text(expanded ? "∨" : "›")
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                      theme.colors.on_surface_variant}),
        huxerui::Column {
            huxerui::Text(group.name).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody)
                    .WithWeight(huxerui::FontWeight::SemiBold),
                theme.colors.on_surface}),
            huxerui::Text(detail).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption),
                theme.colors.on_surface_variant}),
        }.With(huxerui::Spacing(2.0F), huxerui::Grow(1.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Start)),
        std::move(speed),
    }
        .With(huxerui::Spacing(10.0F),
              huxerui::Padding(huxerui::EdgeInsets::Symmetric(12.0F, 10.0F)),
              huxerui::Background(expanded ? islands.active : islands.raised),
              huxerui::CornerRadius(kProxyCardRadius),
              huxerui::Border(islands.outline_soft, 1.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
              huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                 .label = group.name},
              huxerui::Focusable(true))
        .OnClick(std::move(onToggle));
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
    auto testGeneration = huxerui::UseState(0);
    auto testGroup = huxerui::UseState<std::string>("");
    // 分支路径按模式独立（互不共享）：规则模式 path[0] = chips 选中的订阅组，
    // 全局模式 path[0] = GLOBAL 或平台返回的实际可选组；后续元素 = 逐级点入的嵌套子组。
    auto rulePath = huxerui::UseState<std::vector<std::string>>({});
    auto globalPath = huxerui::UseState<std::vector<std::string>>({});
    // 用户是否手动操作过展开状态：手动收起后不再自动展开首组。
    auto pathTouched = huxerui::UseState(false);

    // 数据泵：运行时刷新 /proxies；停止时从当前订阅编译预览快照，
    // 这样用户仍能预先选择节点，下一次内核启动后再由内核正式应用。
    huxerui::Lifecycle(
        [tasks, groups, coreState, mode] {
            tasks.Launch([=]() -> huxerui::Task<void> {
                for (;;) {
                    const auto snap = store::coreStore().snapshot();
                    coreState = snap.state;
                    if (!snap.mode.empty()) mode = snap.mode;
                    const std::string body = co_await RunOnTaskThread([] {
                        return ProxyGroupsSnapshot();
                    });
                    if (!body.empty()) ReplaceStateList(groups, parseProxies(body));
                    co_await huxerui::Delay(std::chrono::duration<double>{
                        snap.state == core::CoreState::Running ? 3.0 : 0.5});
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
                    toast.Show("正在启动内核以进行测速…");
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

    // 切换出站模式时重置“用户已收起”标记，让新模式的视图重新自动展开首组。
    auto lastMode = huxerui::UseState<std::string>("");
    huxerui::Lifecycle(
        [mode, lastMode, pathTouched] {
            if (lastMode.Get() != mode.Get()) {
                lastMode = mode.Get();
                pathTouched = false;
            }
            return [] {};
        },
        mode);

    // 顶部收束区：出站模式切换（同首页 SegmentedButton），挂到标题行右侧。
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

    // 活动路径：按模式取独立 State，渲染期校验（组消失回落，嵌套前缀逐级
    // 校验）。首次进入自动展开首组；用户手动收起（pathTouched）后保持全部
    // 收起；已存路径因订阅刷新失效时同样回落到首组。
    huxerui::State<std::vector<std::string>> activePath =
        global ? globalPath : rulePath;
    const std::vector<std::string> stored = activePath.Get();
    std::vector<std::string> path = resolvePath(all, stored);
    if (path.empty() && (!pathTouched.Get() || !stored.empty())) {
        if (global && globalRoot != nullptr) {
            path = {globalRoot->name};
        } else if (!global && firstRule != nullptr) {
            path = {firstRule->name};
        }
    }
    const ProxyGroup* current =
        path.empty() ? nullptr : findGroup(all, path.back());

    // 组卡列表的根集合：规则模式列出订阅自带分组（有可选组时跳过纯 URLTest
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

    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    // 节点名单行预算：定宽卡下留出右侧延迟与内边距后的可用字符数。
    const std::size_t nodeNameLimit = compact ? 9 : 16;

    // 分组 → 竖向可展开卡片列表：组头与嵌套面包屑占满整行，展开组内的节点
    // 复用多列网格；VirtualGrid 只创建视口附近的项。
    constexpr std::size_t kFullRowSpan = std::numeric_limits<std::size_t>::max();
    const std::string expandedRoot = path.empty() ? std::string{} : path.front();
    std::vector<ProxyItem> items;
    std::vector<std::size_t> spans;
    for (const ProxyGroup* g : rootGroups) {
        const bool expanded = g->name == expandedRoot;
        items.push_back(ProxyItem{.kind = ProxyItemKind::GroupHeader,
                                  .group = g->name,
                                  .expanded = expanded});
        spans.push_back(kFullRowSpan);
        if (!expanded || current == nullptr) continue;
        if (path.size() > 1) {
            items.push_back(ProxyItem{.kind = ProxyItemKind::Breadcrumb,
                                      .group = current->name});
            spans.push_back(kFullRowSpan);
        }
        for (std::size_t i = 0; i < current->nodes.size(); ++i) {
            items.push_back(ProxyItem{.kind = ProxyItemKind::Node,
                                      .group = current->name,
                                      .nodeIndex = i});
            spans.push_back(1);
        }
    }
    if (compact) {
        items.push_back(ProxyItem{.kind = ProxyItemKind::Footer});
        spans.push_back(kFullRowSpan);
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
        huxerui::View groupList = huxerui::VirtualGrid(
            items.size(),
            [items, path, groups, activePath, testGeneration, testGroup, tasks,
             pathTouched, nodeNameLimit,
             triggerGroupTest](std::size_t index) -> huxerui::View {
                const ProxyItem& item = items[index];
                if (item.kind == ProxyItemKind::Footer) {
                    return CompactFloatingNavigationFooter()
                        .Key("compact-floating-footer");
                }
                if (item.kind == ProxyItemKind::Breadcrumb) {
                    return GroupBreadcrumb(path, activePath, tasks)
                        .Key("crumb-" + item.group);
                }
                const ProxyGroup* group = findGroup(groups, item.group);
                if (group == nullptr) return huxerui::View{};
                if (item.kind == ProxyItemKind::GroupHeader) {
                    const std::string name = item.group;
                    const bool expanded = item.expanded;
                    return GroupCardHeader(
                               *group, expanded,
                               [triggerGroupTest, name] {
                                   triggerGroupTest(name);
                               },
                               [tasks, activePath, pathTouched, name,
                                expanded] {
                                   tasks.Launch([=]() -> huxerui::Task<void> {
                                       // 展开/收起会卸载被点击的组内节点：
                                       // 先让出一拍再写 State。
                                       co_await huxerui::Delay(
                                           std::chrono::duration<double>{0});
                                       pathTouched = true;
                                       activePath = expanded
                                           ? std::vector<std::string>{}
                                           : std::vector<std::string>{name};
                                   });
                               })
                        .Key("group-" + name);
                }
                if (item.nodeIndex >= group->nodes.size()) {
                    return huxerui::View{};
                }
                const ProxyNode node = group->nodes[item.nodeIndex];
                const std::string nodeName = node.name;
                return NodeCard(node, nodeName == group->now, group->name,
                                testGeneration, testGroup, group->selectable,
                                NodeSelectAction(groups, activePath, tasks,
                                                 *group, node),
                                nodeNameLimit)
                    .Key(group->name + "::" + nodeName);
            })
            .Columns(compact ? huxerui::GridColumns::Fixed(2)
                             : huxerui::GridColumns::Adaptive(kProxyNodeWidth))
            .EstimatedRowExtent(52.0F)
            .ItemSpans(std::move(spans))
            .RowSpacing(kNodeGridGap)
            .ColumnSpacing(kNodeGridGap)
            .With(huxerui::Grow(1.0F), huxerui::ScrollBar());
        body = std::move(groupList);
    }

    // 出站模式按钮与「代理」标题同处标题行、左右对齐。
    huxerui::View page = PageScaffold("代理", std::move(modeSwitch),
                                      std::move(body), true);
    if (compact && !direct && current != nullptr) {
        const std::string groupName = current->name;
        huxerui::View floatingSpeed =
            huxerui::IconButton(app::images::speed, "测速")
                .With(huxerui::Tooltip("测试当前组延迟"),
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
        // 与 Android 底部悬浮导航栏相同：按钮放在独立的全屏覆盖层中，
        // 由覆盖层的 Column 在主轴末端、交叉轴末端定位，不依赖页面内容容器。
        huxerui::View speedDock = huxerui::Column {
            std::move(floatingSpeed),
        }.With(huxerui::Padding(huxerui::EdgeInsets{
                   .right = theme.spacing.medium,
                   .bottom = kCompactFloatingNavigationInset,
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
