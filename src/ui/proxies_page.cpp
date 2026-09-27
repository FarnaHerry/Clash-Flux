// proxies_page.cpp — 代理页：出站模式（规则/全局/直连）按钮挂在「代理」标题
// 行右侧，与标题左右对齐；下面是分组标签栏 + 选中分组的节点卡网格。
//
// 分组切换（标签栏）：一个根分组一个横向标签，纯文字、无边框无填充；选中项
// 高亮文字并在底部画一条主题色加粗指示线。标签过多时标签栏横向滚动；内容区
// 支持左右滑动手势切换相邻分组（桌面；手机端「代理」在 Pager 里，横向拖动由
// Pager 切一级页，分组切换用标签点击）。同一时刻只展示选中分组的节点。
//
// 规则 → 订阅自带分组标签；全局 → 内核默认出站（`route.final`）所在的那个真实
// 分组，sing-box 合成的只读 GLOBAL 只作兜底；直连 → 不展示订阅内容，只给提示。
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
// 数据流：只读 ProxiesModel 的 State（AppRoot 驱动的唯一来源），原文变化时才
// 到任务线程解析，只同步有变化的组。测速 / 切节点
// 都是阻塞 REST，全部走 RunOnTaskThread；点击事件处理器内不直接写 State
// （约定 6），只 Launch 协程。
#include <huxerui/huxerui.h>

#include "app_resources.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "proxies_model.h"
#include "ui.h"
#include "task_bridge.h"

import nlohmann.json;
import clashflux.core;
import clashflux.singbox;
import clashflux.store.core;
import clashflux.utils;

// CoreView 含 store::CoreSnapshot，必须在模块导入之后（同 profiles_cache.h）。
#include "core_model.h"

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
    bool isGroup = false;      // 该节点本身是策略组（订阅里的 url-test 分支）
    // 卡片第二行的元数据（协议大写 [+ " · UDP" 仅当内核真给了 udp]）：
    // 在解析期算好，item 工厂里零字符串分配——虚拟列表滚动时每帧都建卡片。
    std::string detail;

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

// 卡片第二行元数据（定义在 NodeCard 之前，声明放这里供解析期调用）。
std::string BuildNodeDetail(const ProxyNode& node);

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
                // history 最新一条延迟（unified-delay 下含完整耗时）。
                if (nit->contains("history") && (*nit)["history"].is_array() &&
                    !(*nit)["history"].empty()) {
                    const auto& last = (*nit)["history"].back();
                    if (last.is_object()) node.delay = last.value("delay", 0);
                }
                node.delay = nit->value("urlTestDelay", node.delay);
                node.urlTestTime = nit->value("urlTestTime", std::int64_t{0});
            }
            node.detail = BuildNodeDetail(node);
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

// 卡片第二行元数据：协议（大写）+ UDP 标记；组节点写「组 · 当前分支」。
// 只由 parseProxies 调用一次（结果存进 ProxyNode::detail）。
std::string BuildNodeDetail(const ProxyNode& node) {
    std::string type = node.type;
    for (char& c : type) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    if (type.empty()) return node.udp ? std::string{"UDP"} : std::string{};
    // 成员本身是策略组时只显示组类型（URLTEST/SELECTOR…）：组没有"协议 + UDP"
    // 语义，也不该带「组 · 当前分支」这种旧分支文案。
    if (!node.isGroup && node.udp) type += " · UDP";
    return type;
}

// 节点卡自适应列的最小宽度：值越小同宽窗口下卡片越窄（列数更多）。
// 225 是"满屏 6 列"的阈值：1560 逻辑宽的满屏窗口去掉侧栏与岛内边距后约
// 1428 逻辑 px，1428 / (225 + 8) ≈ 6.1 → 6 列（每张约 231 px）。
constexpr float kProxyNodeWidth = 225.0F;

// 延迟槽能放下的字符数（"测速中…"/"99999 ms"），超出截断成省略号。
constexpr std::size_t kNodeMetaChars = 8;

// 左右滑动切换分组的处理器与阈值由 SectionTabSwipeHandler（common.cpp）统一提供，
// 代理/订阅/规则页一致；「手机端在 Pager 里也能局部切换」的关键在于它 6pt 就抢先
// 认领指针会话（早于 Pager 的整页拖动，见 section_swipe.h），并且在第一个/最后一个
// 分组时主动不认领，把手势让回 Pager 整页翻。这里给页面挂的是内容网格，所以标签栏
// 与页面留白上的横向拖动仍然是整页翻。

// 节点矩形卡：内部左右对齐——左侧名称单行（溢出省略号），右侧写延迟
// （组类型节点写「组·分支当前选中」），延迟按区间着色；选中态 primary 底。
// 宽度由 VirtualGrid 均分，高度由 EstimatedRowExtent 提供估计。
[[huxerui::composable]] huxerui::View NodeCard(
    const ProxyNode& node, bool selected, const std::string& groupName,
    huxerui::State<int> testGeneration, huxerui::State<std::string> testGroup,
    bool interactive, std::function<void()> onSelect, std::size_t nameLimit,
    std::size_t detailLimit) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
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
         nodeTestTime = node.urlTestTime, nodeName, isGroupNode = node.isGroup] {
            // 成员本身是策略组（订阅里的 url-test 分支）时不测速：它的延迟没有
            // 意义（url-test 自己会挑最快），卡片也不显示延迟槽内容。
            if (isGroupNode) return;
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

    // 右侧槽位只放"测速结果"：
    //   测速中… / N ms / 超时（**仅真正测过且失败**）/ 空占位（从未测过）。
    // 以前把 delay==0 一律画成"超时"，未测速的节点满屏红字，现在留空但保留等宽
    // 占位，卡片之间仍然对齐。
    std::string meta;
    if (!node.isGroup) {
        if (currentProbe.testing) {
            meta = "测速中…";
        } else if (delay > 0) {
            meta = std::format("{} ms", delay);
        } else if (timeout) {
            meta = "超时";
        }
    }
    // 第二行与延迟都在卡片宽度内截断成省略号：huxerui 的 Text 没有省略号能力，
    // 与名称用同一套字符预算（延迟槽更窄，预算更小）。短字符串走 SSO，不额外
    // 分配。第二行元数据本身在解析期算好（见 ProxyNode::detail）。
    const std::string detail =
        node.detail.size() > detailLimit ? truncateOneLine(node.detail, detailLimit)
                                         : node.detail;
    const std::string metaText =
        meta.size() > kNodeMetaChars ? truncateOneLine(meta, kNodeMetaChars) : meta;

    // 卡片表面与文字色来自统一原语：未选中 islands.active、选中 primary 实心底。
    const SelectableTileColors tile =
        ResolveSelectableTileColors(theme, selected);
    const huxerui::Color detailColor = selected ? tile.fg : tile.muted;
    const huxerui::Color metaColor =
        selected ? tile.fg
                 : (meta == "超时"
                        ? theme.colors.error
                        : (meta.empty() || meta == "测速中…"
                               ? tile.muted
                               : delayColor(theme, delay, false)));
    // 延迟槽固定宽度（含"测速中…"），内容**右对齐**贴住卡片右边距：以前 Text
    // 在槽内左对齐，短文本（"128 ms"）右侧会多出一截空隙，看着像没对齐；未测速
    // 时留空占位，卡片之间仍然对齐。
    constexpr float kNodeMetaSlotWidth = 62.0F;
    huxerui::View metaView =
        metaText.empty()
            ? huxerui::View{huxerui::Row{}.With(
                  huxerui::Frame{.width = kNodeMetaSlotWidth})}
            : huxerui::View{
                  huxerui::Row{huxerui::Text(metaText).Style(
                      huxerui::TextStyle{
                          huxerui::Font::System(font_size::kCaption),
                          metaColor})}
                      .With(huxerui::Frame{.width = kNodeMetaSlotWidth},
                            huxerui::MainAlign(
                                huxerui::MainAxisAlignment::End))};
    // 两行：第一行名称，第二行是「元数据（左）＋ 测速（右）」同一基线左右对齐。
    // 表面 / 内边距 / 圆角 / 交互全部交给 SelectableTile——代理页节点卡就是这套
    // 形状的标准（订阅卡等其它列表项同样复用它）。
    return SelectableTile(
        huxerui::Column {
            huxerui::Text(truncateOneLine(node.name, nameLimit))
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kBodySmall), tile.fg}),
            huxerui::Row {
                huxerui::Text(detail).Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption), detailColor}),
                huxerui::Spacer(),
                std::move(metaView),
            }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::End)),
        }.With(huxerui::Spacing(2.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
        selected, node.name,
        interactive ? std::move(onSelect) : std::function<void()>{});
}

// 节点网格间隙：组卡内部节点按列排布，取比岛屿缝隙更紧的间距。
constexpr float kNodeGridGap = 8.0F;

// 页面级「乐观选中意图」：点击后渲染立刻读到目标节点。以前直接改 `groups`
// （StateList）——那会让整张 VirtualGrid 失效并重排，大订阅下点一下卡一下；
// 现在只写这个小 State，只有可见的卡片重组。模型快照追平后清掉意图。
struct SelectionIntent {
    std::string group;
    std::string node;

    bool operator==(const SelectionIntent&) const = default;
};

// 节点点击动作：**所有成员都只是"选中当前线路"**。成员本身是策略组时（订阅里
// 的 url-test 组，如「自动选择」）表示"这条线路交给它自动挑最快"，与 Clash
// Verge / metacubexd 一致：没有"点进去看子组"这一层，子组是标签栏里的独立分组。
// 切换写在任务协程里（点击节点可能随列表换组卸载）。
std::function<void()> NodeSelectAction(
    huxerui::State<SelectionIntent> intent, huxerui::TaskScope tasks,
    std::shared_ptr<ProxiesModel> proxiesModel, std::function<void()> onFailure,
    const ProxyGroup& group, const ProxyNode& node) {
    const std::string groupName = group.name;
    const std::string nodeName = node.name;
    if (!group.selectable) return [] {};
    const SelectionIntent target{groupName, nodeName};
    return [intent, tasks, proxiesModel, onFailure, target, groupName,
            nodeName] {
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            intent = target;
            const bool selected = co_await RunOnTaskThread(
                [=] { return SelectProxyLine(groupName, nodeName); });
            if (!selected) {
                // 失败回落：只有意图仍停在本目标上才撤销（用户已点别的就不覆盖）。
                if (intent.Get() == target) intent = SelectionIntent{};
                if (onFailure) onFailure();
                co_return;
            }
            proxiesModel->RequestRefresh();
        });
    };
}

} // namespace

// 分组页的入场过渡：代理页只为当前分组构造内容（虚拟化，见页面里的循环），换组时
// 新页是**全新挂载**的节点，而 `AnimateTo` 只在目标值变化时才有动画、新挂载会直接
// 落到目标值上——照搬订阅页那套 `AnimateTo(selected ? 1 : 0)` 在代理页等于没有动画。
// 所以每页自带一条本地进度：挂载后由 Lifecycle 从 0 推到 1（页的 Key 随分组变化，
// 新挂载 = 新 scope = 进度重新从 0 开始），换组因此每次都播一遍「透明度 + 横向轻移」。
// 轨道值与订阅页/规则页保持一致（0.82→1、右侧 12pt 滑入），reduced motion 下时长归零。
[[huxerui::composable]] huxerui::View ProxyGroupPage(huxerui::View content) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto progress = huxerui::UseState(0.0F);
    // 常量依赖：只在挂载时补一次（与数据泵那类 Lifecycle 写法一致）。
    huxerui::Lifecycle(
        [progress] {
            progress = 1.0F;
            return [] {};
        },
        0);
    // composable 形参在函数体内是 const（见 CLAUDE.md 第 2 条），先取副本再装修。
    huxerui::View page = std::move(content);
    return std::move(page).With(
        huxerui::Grow(1.0F),
        huxerui::Transition{huxerui::AnimateTo(
            progress.Get(),
            huxerui::TweenSpec{
                .duration = theme.motion.reduced_motion ? 0.0 : theme.motion.normal,
                .easing = huxerui::Easing::EaseOut})}
            .Opacity(0.82F, 1.0F)
            .Offset({12.0F, 0.0F}, {}));
}

[[huxerui::composable]] huxerui::View ProxiesPage(bool active) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    // 策略组快照 / 内核状态的全局唯一来源（见 *_model.h）。
    const auto proxiesModel = huxerui::UseService<ProxiesModel>();
    const auto coreModel = huxerui::UseService<CoreModel>();
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
    // 已经解析过的快照原文：只在原文变化时重新解析（模型的 State 去重已保证
    // 通知只在内容变化时发生）。
    auto parsedBody = huxerui::UseState<std::string>("");
    // 乐观选中意图（见 SelectionIntent）：只写这个小组 State，不动 groups。
    auto selectionIntent = huxerui::UseState<SelectionIntent>(SelectionIntent{});

    // 数据流完全由模型驱动（见 *_model.h）：内核状态、策略组快照任一变化才
    // 重新解析嵌套分组，**没有定时器**。解析放任务线程——大订阅的 /proxies
    // 原文在 UI 线程解析会掉帧。
    huxerui::Lifecycle(
        [tasks, groups, coreState, mode, modePending, proxiesModel, coreModel,
         parsedBody, selectionIntent] {
            const store::CoreSnapshot core = coreModel->view.Get().core;
            coreState = core.state;
            if (!modePending.Get() && !core.mode.empty()) mode = core.mode;
            const ProxiesSnapshot& current = proxiesModel->snapshot.Get();
            if (current.body != parsedBody.Get()) {
                parsedBody = current.body;
                tasks.Launch([groups, selectionIntent, body = current.body]()
                                 -> huxerui::Task<void> {
                    auto parsed = co_await RunOnTaskThread(
                        [body] { return parseProxies(body); });
                    // 未确认的乐观选中：模型追平就清意图，否则继续用意图值
                    // 覆盖解析结果（点击立刻上屏，不等 2s 那拍）。
                    const SelectionIntent pending = selectionIntent.Get();
                    if (!pending.node.empty()) {
                        for (ProxyGroup& parsedGroup : parsed) {
                            if (parsedGroup.name != pending.group) continue;
                            if (parsedGroup.now == pending.node) {
                                selectionIntent = SelectionIntent{};
                            } else {
                                parsedGroup.now = pending.node;
                            }
                            break;
                        }
                    }
                    SyncProxyGroups(groups, std::move(parsed));
                });
            }
            return [] {};
        },
        coreModel->view, proxiesModel->snapshot);

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
    const ProxyGroup* syntheticGlobal = findGroup(all, "GLOBAL");
    // 全局模式的根组 = 内核**实际**走哪个出站。sing-box 的 GLOBAL 是 clash_api
    // 合成的只读组（type Fallback，`PUT /proxies/GLOBAL` 会 404），它的 `now`
    // 等于内核的默认出站（route.final = 订阅 MATCH 目标，缺省回落首个 selector 组）。
    // 直接拿 GLOBAL 当根组会让整屏卡片都点不动；随便挑一个组又会出现「改了不影响
    // 全局流量」的假象。所以按 `now` 找回那个真实组，只有找不回时才退回 GLOBAL。
    const ProxyGroup* globalRoot = nullptr;
    if (global) {
        const std::string globalTarget =
            syntheticGlobal != nullptr ? syntheticGlobal->now : std::string{};
        if (!globalTarget.empty()) {
            const ProxyGroup* target = findGroup(all, globalTarget);
            if (target != nullptr && target != syntheticGlobal) globalRoot = target;
        }
        if (globalRoot == nullptr) globalRoot = syntheticGlobal;
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
    }

    // 标签栏列出订阅里的**每个**策略组（与 Clash Verge / metacubexd 一致）：
    // url-test 组（如订阅里的「自动选择」）也是独立分组——既能在父组里被选为
    // 线路（＝交给它自动挑最快），也能直接切到它自己的标签查看/测速，因此不再
    // 需要"点进子组"那一层。全局模式只保留 GLOBAL（或平台回落组），直连不走组。
    std::vector<const ProxyGroup*> rootGroups;
    if (global) {
        if (globalRoot != nullptr) rootGroups.push_back(globalRoot);
    } else if (!direct) {
        for (const auto& g : all) {
            if (g.name == "GLOBAL") continue;
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
    // 第二行（协议/组类型）的字符预算：卡片宽度减去右侧固定延迟槽后的余量，
    // 比名称行字号小、能多放几个字；超出截断成省略号。
    const std::size_t detailLimit = compact ? 10 : 16;

    // 不可见时只保留本页 State/Lifecycle，不构建重子树：huxerui 的 Pager 会把
    // 四个一级页同时挂载，隐藏页即使不重组，其已挂载子树仍随每一帧被重新测量。
    // 真机实测（代理页大分组）：四页同挂时每帧 1443 次测量请求 / ~20ms，
    // 只留当前页后降到 28 次 / ~0ms；因此不可见页必须返回空占位。
    if (!active) return huxerui::View{huxerui::Row{}}.Key("proxies-idle");

    // 一个根分组一页。VirtualGrid 直接用索引读取组节点，不再为所有组预先
    // 分配完整的 ProxyItem 与 span 数组；只有视口附近的节点会被构造为卡片。
    // 只为**当前 tab**构造页面并直接挂载（不再套 IndexedPages：它给子页无界高度，
    // 内层 VirtualGrid 会因此全量构建——真机实测大组 53ms/帧、小组 6ms/帧）；
    // 滑动手势在内容区自行识别。
    constexpr std::size_t kCompactFooterItems = 2;
    std::vector<huxerui::View> groupPages;
    groupPages.reserve(rootGroups.size());
    for (std::size_t page = 0; page < rootGroups.size(); ++page) {
        const ProxyGroup& rootGroup = *rootGroups[page];
        // 只有当前页可能是嵌套子组（path 更深）；其余页就是各自的根分组。
        const bool selectedPage = page == selectedTab;
        const ProxyGroup* contentGroup =
            (selectedPage && current != nullptr) ? current : &rootGroup;

        // 只为**当前 tab**构造真正的页面：以前每个根组都建一个 VirtualGrid 并
        // 交给 IndexedPages 保持挂载，几十个组时会同帧参与组合。其余页留空占位，
        // 切组时再造（切组会重建当前页，滚动位置不再跨组保留）。
        if (!selectedPage) {
            groupPages.push_back(huxerui::View{huxerui::Row{}}.Key(
                "group-page-idle-" + rootGroup.name));
            continue;
        }

        const std::string contentGroupName = contentGroup->name;
        const std::size_t nodeCount = contentGroup->nodes.size();
        const std::size_t footerCount = compact ? kCompactFooterItems : 0;
        huxerui::View grid = huxerui::VirtualGrid(
                                 nodeCount + footerCount,
                                 [groups, testGeneration, testGroup,
                                  detailLimit,
                                  tasks, toast, nodeNameLimit, contentGroupName,
                                  nodeCount, compact, proxiesModel,
                                  selectionIntent](std::size_t index)
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
                                     // 乐观意图优先于模型快照里的 now。
                                     const SelectionIntent pending =
                                         selectionIntent.Get();
                                     const std::string currentNow =
                                         pending.group == group->name
                                             ? pending.node
                                             : group->now;
                                     return NodeCard(
                                                node, nodeName == currentNow,
                                                group->name, testGeneration,
                                                testGroup, group->selectable,
                                                NodeSelectAction(
                                                    selectionIntent, tasks,
                                                    proxiesModel,
                                                    [toast] {
                                                        toast.Show(
                                                            "线路切换失败，请查看应用日志");
                                                    },
                                                    *group, node),
                                                nodeNameLimit, detailLimit)
                                         .Key(group->name + "::" + nodeName);
                                 })
                                 .Columns(compact
                                              ? huxerui::GridColumns::Fixed(2)
                                              : huxerui::GridColumns::Adaptive(
                                                    kProxyNodeWidth))
                                 // 行高**只给估算值**，真实高度由虚拟布局测量卡片内容
                                 // 得到：卡片是「名称 + 元数据行 + 上下 8pt 内边距」的
                                 // 自适应高度（字体缩放、系统字号变化都会改高度），
                                 // 写死精确行高会在这些情况下裁切/错位。
                                 .EstimatedRowExtent(52.0F)
                                 .RowSpacing(kNodeGridGap)
                                 .ColumnSpacing(kNodeGridGap)
                                 .With(huxerui::Grow(1.0F),
                                       huxerui::ScrollBar())
                                 .Key("group-grid-" + rootGroup.name);
        // 页内分区滑动：两端没有相邻分组时 onPrev/onNext 留空，处理器据此不认领，
        // 手势交给外层 Pager 整页翻（见 section_swipe.h）。处理器/阈值与订阅页、
        // 规则页共用。
        if (kSectionTabsSwipeDefault) {
            std::function<void()> swipePrev;
            if (selectedTab > 0) {
                swipePrev = [selectGroup, tabNames, selectedTab] {
                    selectGroup(tabNames[selectedTab - 1]);
                };
            }
            std::function<void()> swipeNext;
            if (selectedTab + 1 < tabNames.size()) {
                swipeNext = [selectGroup, tabNames, selectedTab] {
                    selectGroup(tabNames[selectedTab + 1]);
                };
            }
            grid = std::move(grid).On<huxerui::ViewEvents::PointerIntercept>(
                SectionTabSwipeHandler(swipeOrigin, swipeOwned,
                                       std::move(swipePrev),
                                       std::move(swipeNext)));
        }

        groupPages.push_back(
            ProxyGroupPage(std::move(grid)).Key("group-page-" +
                                                rootGroup.name));
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
            huxerui::Text(global ? "全局模式暂无可用策略组"
                                 : "暂无策略组（检查订阅配置）")
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kBody),
                    theme.colors.on_surface_variant}),
        }
            .With(huxerui::Padding(32.0F),
                  huxerui::Grow(1.0F),
                  huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    } else {
    // 不套 IndexedPages：它给子页的约束是无界高度，内层 VirtualGrid 因此拿不到
    // 有界视口，只能**全量构建**该组所有卡片——真机实测同一个页面里
    // 大组 53ms/帧、小组 6ms/帧，就是这个原因。页面已经是"只为当前 tab 建页"，
    // 直接挂载当前页即可恢复虚拟化。
    body = groupPages.empty()
               ? huxerui::View{huxerui::Column{}.With(huxerui::Grow(1.0F))}
               : std::move(groupPages[std::min(selectedTab,
                                               groupPages.size() - 1)])
                     .With(huxerui::Grow(1.0F));
    }

    // 标签栏固定在页面顶部（不随节点列表滚动），只有选中分组的节点参与滚动。
    std::vector<huxerui::View> columnChildren;
    if (!tabNames.empty()) {
        std::vector<SectionTab> groupTabs;
        groupTabs.reserve(tabNames.size());
        // 保真度角标：这份订阅编译时被降级 / 跳过的组，在标签上带一个 "!"。这里只
        // 做「哪个组有问题」的定位，明细在设置页「配置保真度」
        // （见 docs/singbox-layers-and-fidelity.md §2）。
        const std::vector<singbox::FidelityNote> fidelity =
            coreModel->view.Get().core.fidelity;
        const auto groupHasFidelityNote = [&fidelity](const std::string& name) {
            return std::any_of(
                fidelity.begin(), fidelity.end(),
                [&name](const singbox::FidelityNote& note) {
                    return note.scope == singbox::FidelityScope::Group &&
                           note.subject == name;
                });
        };
        for (const std::string& name : tabNames) {
            groupTabs.push_back(SectionTab{
                name, name,
                groupHasFidelityNote(name) ? std::string{"!"} : std::string{}});
        }
        columnChildren.push_back(
            SectionTabBar(groupTabs, selectedRoot, selectGroup));
    }
    huxerui::View content = std::move(body);
    if (!columnChildren.empty()) {
        columnChildren.push_back(std::move(content).With(huxerui::Grow(1.0F)));
        content =
            huxerui::Column(std::move(columnChildren))
                .With(huxerui::Spacing(theme.spacing.small),
                      huxerui::Grow(1.0F),
                      huxerui::CrossAlign(
                          huxerui::CrossAxisAlignment::Stretch));
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
