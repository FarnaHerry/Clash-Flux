// proxies_page.cpp — 代理页：横滑标签或纵向分组，共用节点卡和代理状态。出站模式
// （规则/全局/直连）的切换入口只在首页「出站模式」卡片；本页只跟随内核当前
// 模式渲染对应内容，不重复提供切换按钮。
//
// 分组切换（标签栏）：一个根分组一个横向标签，纯文字、无边框无填充；选中项
// 高亮文字并在底部画一条主题色加粗指示线。标签过多时标签栏横向滚动；内容区
// 支持左右滑动手势切换相邻分组，两端继续滑动交给手机端 Pager 切一级页。
// 同一时刻只展示选中分组的节点。
//
// 规则 → 订阅自带分组标签；全局 → 内核默认出站（`route.final`）所在的那个真实
// 分组，sing-box 合成的只读 GLOBAL 只作兜底；直连 → 不展示订阅内容，只给提示。
//
// 规则/全局两套分支路径 State 独立（rulePath/globalPath），切换模式互不
// 覆盖对方的选择。GLOBAL 组只在全局模式出现，规则列表不含它；Android
// 等仅返回真实策略组的平台，则全局模式回落到第一个可选策略组。
//
// 节点网格：Compact 两列；桌面按窗口宽度排 1/2/3/4 列，轨道均分铺满
// 页面宽度。嵌套面包屑固定在网格上方，不参与节点滚动。
//
// 分支语义：点组类型节点 = 选中该分支到当前组（selectProxy）并进入浏览；
// 叶子节点 = 常规切换。展开组内出现嵌套时，面包屑行提供返回上级。路径 State
// 只存用户走出的链，渲染期经 resolvePath 对最新 groups 校验裁剪，订阅刷新
// 导致组消失时自动回落，无需泵写状态。
//
// 延迟测试：横滑顶栏测试当前分组，分组视图顶栏测试当前模式下的所有分组。
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
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "country_flag.h"
#include "proxies_model.h"
#include "settings_model.h"
#include "ui.h"
#include "page_layout.h"
#include "page_loading.h"
#include "empty_state.h"
#include "search_text.h"
#include "task_bridge.h"
#include "proxy_group_layout.h"
#include "proxy_node_tile.h"
#include "proxy_speed_button.h"

#include "wire_codec.h"
import clashflux.core;
import clashflux.singbox;
import clashflux.store.core;
import clashflux.utils;

// CoreView 含 store::CoreSnapshot，必须在模块导入之后（同 profiles_cache.h）。
#include "core_model.h"
#include "core_control.h"

namespace clashflux::ui {
namespace {

struct ProxyNode {
    std::string name;
    std::string displayName;
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
    std::string displayName;
    std::string type;   // Selector / URLTest / Fallback / LoadBalance / Relay
    std::string now;    // 当前选中节点
    bool selectable = false;
    std::vector<ProxyNode> nodes;
    std::optional<std::size_t> selectedIndex;

    bool operator==(const ProxyGroup&) const = default;
};

using ProxyGroupPtr = std::shared_ptr<const ProxyGroup>;

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

std::vector<ProxyGroup> parseProxies(const std::shared_ptr<const wire::Proxies>& snapshot,
    const std::map<std::string, std::string>& labels) {
    std::vector<ProxyGroup> groups;
    if (!snapshot) return groups;
    const auto& all = snapshot->entries;
    for (const auto& [name, value] : all) {
        if (!isGroupType(value.type) || !value.members) continue;
        ProxyGroup group;
        group.name = name;
        group.displayName = labels.contains(name) ? labels.at(name) : name;
        group.type = value.type;
        group.now = value.now;
        group.selectable = value.selectable.value_or(isSelectorType(value.type));
        group.nodes.reserve(value.members->size());
        for (const auto& member : *value.members) {
            if (!group.selectedIndex && member == group.now) group.selectedIndex = group.nodes.size();
            ProxyNode node;
            node.name = member;
            node.displayName = labels.contains(member) ? labels.at(member) : member;
            if (const auto found = all.find(member); found != all.end()) {
                node.type = found->second.type;
                node.udp = found->second.udp;
                node.isGroup = isGroupType(node.type);
                node.delay = found->second.delay;
                node.urlTestTime = found->second.testTime;
            }
            node.detail = BuildNodeDetail(node);
            group.nodes.push_back(std::move(node));
        }
        groups.push_back(std::move(group));
    }
    // GLOBAL 组太长且无意义时沉底（clash_api 兼容组，列出全部节点）。
    std::ranges::stable_sort(groups, [](const ProxyGroup& a, const ProxyGroup& b) {
        return (a.name == "GLOBAL") < (b.name == "GLOBAL");
    });
    return groups;
}

void SyncProxyGroups(huxerui::StateList<ProxyGroupPtr> groups,
                     std::vector<ProxyGroup> next) {
    if (groups.Size() != next.size()) {
        std::vector<ProxyGroupPtr> snapshots;
        snapshots.reserve(next.size());
        for (auto& group : next) snapshots.push_back(std::make_shared<ProxyGroup>(std::move(group)));
        ReplaceStateList(groups, std::move(snapshots));
        return;
    }
    // Keep the same owning snapshot for unchanged groups. Virtual factories
    // can then retain it across composition and never duplicate its nodes.
    for (std::size_t i = 0; i < next.size(); ++i) {
        if (*groups[i] != next[i]) groups.Set(i, std::make_shared<ProxyGroup>(std::move(next[i])));
    }
}

ProxyGroupPtr findGroup(const huxerui::StateList<ProxyGroupPtr>& groups,
                        const std::string& name) {
    for (const auto& g : groups) {
        if (g->name == name) return g;
    }
    return {};
}

// 渲染期校验：只保留 navPath 中仍然存在且逐级可达（组类型子节点）的前缀；
// 空输入或根组消失返回空（调用方回落第一个组）。
std::vector<std::string> resolvePath(
    const huxerui::StateList<ProxyGroupPtr>& groups,
    const std::vector<std::string>& raw) {
    std::vector<std::string> out;
    if (raw.empty()) return out;
    auto g = findGroup(groups, raw.front());
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

// 虚拟网格的固定缓存高度。宽屏列数多时，框架默认的 0.5 视口高度会让每个
// 滚动帧反复回收/重建边界卡片（15 列 × 96 帧约 7.3 万次工厂调用，见
// docs/evidence/2026-11-16-proxy-grid-scroll.md）；200pt 保留约 3 行预热，
// 消除该抖动并把同时挂载的卡片数减少约四成。
constexpr float kProxyGridCacheExtent = 200.0F;

// sing-box constant.TCPTimeout；单节点和组测速都使用内核同一上限。
constexpr int kProxyDelayTimeoutMs = 15000;

huxerui::Task<bool> WaitForProxyTestCore(CoreControl control, std::shared_ptr<ProxyTestModel> tests,
    FluxToastHandle toast) {
    if (control.model->closing) co_return false;
    if (!control.Busy() && control.model->view.Get().core.state == core::CoreState::Running) co_return true;
#if defined(CLASHFLUX_IOS)
    toast.Show(Localized("测速需要内核就绪，请检查内核服务"));
    co_return false;
#else
    auto readiness = tests->readiness;
    if (!readiness) {
        readiness = tests->readiness = std::make_shared<std::optional<bool>>();
        const auto finish = [tests, readiness](bool ready) {
            *readiness = ready;
            tests->readiness.reset();
        };
        control.tasks.Launch([control, toast, finish]() -> huxerui::Task<void> {
            while (!control.model->closing && control.Busy()) {
                co_await huxerui::Delay(std::chrono::milliseconds{100});
            }
            if (control.model->closing) {
                finish(false);
            } else if (control.model->view.Get().core.state == core::CoreState::Running) {
                finish(true);
            } else if (!control.Run({.action = CoreAction::Start, .origin = CoreCommandOrigin::ProxyTest}, finish)) {
                finish(false);
                toast.Show(Localized("测速需要内核就绪，请检查内核服务"));
            }
        });
    }
    while (!readiness->has_value() && !control.model->closing) {
        co_await huxerui::Delay(std::chrono::milliseconds{100});
    }
    co_return !control.model->closing && readiness->value_or(false);
#endif
}

auto BindProxyTest(const std::shared_ptr<CoreModel>& coreModel, ProxyTestModel& tests) {
    const auto& core = coreModel->view.Get().core;
    tests.ObserveContext(core.state == core::CoreState::Running, core.planRevision, core.runtimeEpoch);
    return [coreModel, revision = core.planRevision, epoch = core.runtimeEpoch] {
        const auto& current = coreModel->view.Get().core;
        return current.state == core::CoreState::Running && current.planRevision == revision &&
            current.runtimeEpoch == epoch;
    };
}

std::optional<std::pair<int, std::int64_t>> ReadProxyTestResult(
    const ProxiesModel& model, const std::string& tag) {
    const auto& snapshot = model.snapshot.Get();
    if (snapshot.source != ProxiesSource::Live || !snapshot.proxies) return std::nullopt;
    const auto node = snapshot.proxies->entries.find(tag);
    if (node == snapshot.proxies->entries.end()) return std::nullopt;
    return std::pair{node->second.delay, node->second.testTime};
}

template<class Valid>
void RunSingleProxyProbe(huxerui::TaskScope tasks, const std::shared_ptr<ProxiesModel>& model,
    const std::string& tag, const std::shared_ptr<ProxyProbe>& probe, Valid valid,
    const FluxToastHandle& toast) {
#if defined(__ANDROID__)
    const auto measure = [model, probe, valid, toast](const std::string& target) -> huxerui::Task<std::optional<int>> {
        std::optional<int> result;
        try {
            result = co_await MeasureNativeProxyDelay(target, probe, [model](const std::string& node)
            -> huxerui::Task<bool> {
                const bool accepted = co_await RunOnTaskThread([node] { return AndroidProxyUrlTest(node); });
                if (accepted) model->RequestRefresh();
                co_return accepted;
            }, [model](const std::string& node) {
                return ReadProxyTestResult(*model, node);
            }, valid);
        } catch (...) {}
        if (!result && valid()) toast.Show(Localized("测速失败：内核未运行或启动失败"));
        co_return result;
    };
#else
    static_cast<void>(toast);
    const auto measure = [](const std::string& target) {
        return RunOnTaskThread([target] {
            // Let sing-box choose its default URL, as it does on Android.
            const auto result = store::coreStore().api().proxyDelay(target, "", kProxyDelayTimeoutMs);
            if (!result.ok) return 0;
            const auto delay = wire::DecodeDelay(result.body);
            return delay ? delay.value : 0;
        });
    };
#endif
    LaunchSingleProxyProbe(tasks, model->tests, tag, probe, measure, valid);
}

huxerui::Task<std::map<std::string, int>> MeasureProxyGroupDelay(const std::string& group) {
    co_return co_await RunOnTaskThread([group] {
        // Native group test: URLTest groups use their own url-test.url;
        // other groups use the kernel's default test URL. The kernel runs
        // the batch itself with its native concurrency and history updates.
        const auto result = store::coreStore().api().groupDelay(group, kProxyDelayTimeoutMs);
        if (!result.ok) return std::map<std::string, int>{};
        auto decoded = wire::DecodeGroupDelays(result.body);
        return decoded ? std::move(decoded.value) : std::map<std::string, int>{};
    });
}

huxerui::Task<void> RunGroupProxyProbe(huxerui::TaskScope tasks, CoreControl control,
    std::shared_ptr<ProxiesModel> model, std::string name,
    std::shared_ptr<ProxyGroupTest> batch, int ticket, FluxToastHandle toast) {
    if (!(co_await WaitForProxyTestCore(control, model->tests, toast))) {
        batch->End(ticket, false);
        co_return;
    }

    const auto valid = BindProxyTest(control.model, *model->tests);
    if (!batch->Current(ticket)) co_return;
    std::vector<ProxyProbeTarget> targets;
    if (const auto snapshot = model->snapshot.Get().proxies; valid() && snapshot) {
        targets = CollectProxyGroupTargets(*model->tests, *snapshot, name);
    }
    if (targets.empty()) {
        batch->End(ticket, false);
        co_return;
    }

    batch->bound = true;
    const auto poll = [model](const std::string& tag) { return ReadProxyTestResult(*model, tag); };
    const bool native = ProxyTestUsesNativeGroup();
    if (!(co_await ClaimProxyGroupTargets(*batch, ticket, targets, native, valid, poll))) {
        batch->End(ticket, false);
        co_return;
    }
    if (native) {
        const auto latest = std::ranges::max_element(targets, {}, [](const auto& target) {
            return target.nativeHistory.baselineTime;
        });
        co_await WaitForNativeProxyTimestamp(latest->nativeHistory.baselineTime);
    }
    if (!batch->Current(ticket)) co_return;
    if (!valid()) {
        batch->End(ticket, false);
        co_return;
    }

    bool accepted = false;
    try {
        accepted = co_await RunOnTaskThread([name] { return StartProxyGroupTest(name); });
    } catch (const std::exception&) {}
    if (!batch->Current(ticket)) co_return;
    if (!valid() || !accepted) {
        batch->End(ticket, false);
        if (valid()) toast.Show(Localized("测速失败：内核未运行或启动失败"));
        co_return;
    }
    if (native) {
        model->RequestRefresh();
        LaunchNativeProxyProbeBatch(tasks, batch, std::move(targets), ticket, valid, poll);
    } else {
        LaunchProxyGroupProbeBatch(tasks, batch, name, std::move(targets), ticket, MeasureProxyGroupDelay, valid, poll);
    }
}

// 页面级「乐观选中意图」：点击后渲染立刻读到目标节点。以前直接改 `groups`
// （StateList）——那会让整张 VirtualGrid 失效并重排，大订阅下点一下卡一下；
// 现在只写这个小 State，只有可见的卡片重组。模型快照追平后清掉意图。
struct SelectionIntent {
    std::string group;
    std::string node;

    bool operator==(const SelectionIntent&) const = default;
};

struct ProxySyncCache {
    std::map<std::string, std::string> labels;
    std::string body;
    std::uint64_t ticket = 0;
};

// 节点名占满首行，末行左侧显示协议或组信息，右侧显示空心闪电、结果或圆点动画。
[[huxerui::composable]] huxerui::View NodeCard(
    ProxyGroupPtr group, std::size_t index, float dotsWidth,
    huxerui::State<SelectionIntent> selectionIntent,
    huxerui::TaskScope appTasks, CoreControl control, FluxToastHandle toast,
    std::shared_ptr<ProxiesModel> proxiesModel, std::function<void()> onActivate, bool insideGroup) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const auto& node = group->nodes[index];
    const auto& pending = selectionIntent.Get();
    const bool selected = node.name == (pending.group == group->name ? pending.node : group->now);
    const auto& tests = proxiesModel->tests;
    static_cast<void>(tests->generation.Get()); // Rebind after a runtime-plan change.
    const auto entry = tests->Get(node.name, node.delay, node.timeout, node.urlTestTime);
    const auto probe = entry->value;
    huxerui::Lifecycle([entry, nodeDelay = node.delay, nodeTestTime = node.urlTestTime] {
        if (!entry->value.Get().testing && nodeTestTime > entry->baselineTime.Get()) {
            entry->value = ProbeState{nodeDelay > 0 ? nodeDelay : 0, nodeDelay <= 0, false};
            entry->baselineTime = nodeTestTime;
        }
        return [] {};
    }, node.delay, node.urlTestTime);

    const ProbeState currentProbe = probe.Get();
    const int delay = currentProbe.delay;
    const bool timeout = currentProbe.timeout;

    const auto tile = ResolveSelectableTileColors(theme, selected);
    const auto detailColor = selected ? tile.fg : tile.muted;
    huxerui::View delayView;
    if (!node.isGroup && !currentProbe.testing && (delay > 0 || timeout)) {
        const auto text = delay > 0 ? huxerui::StringVariant{std::to_string(delay)}
                                    : huxerui::StringVariant{Localized("超时")};
        const auto color = selected ? tile.fg
            : delay > 0 ? DelayLevelColor(theme, delay, false) : theme.colors.error;
        delayView = huxerui::Text(text)
            .LineLimit(1).Overflow(huxerui::TextOverflow::Ellipsis)
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption), color});
    }
    const auto startProbe = [appTasks, control, proxiesModel, entry, tag = node.name, delay = node.delay,
                             timeout = node.timeout, testTime = node.urlTestTime, toast] {
        if (entry->value.Get().testing) return;
        if (!proxiesModel->tests->singlePreparing.insert(tag).second) return;
        appTasks.Launch([appTasks, control, proxiesModel, tag, delay, timeout, testTime, toast]() -> huxerui::Task<void> {
            const auto& tests = proxiesModel->tests;
            if (co_await WaitForProxyTestCore(control, tests, toast)) {
                const auto valid = BindProxyTest(control.model, *tests);
                const auto probe = tests->Get(tag, delay, timeout, testTime);
                while (valid() && probe->owner != 0) {
                    co_await huxerui::Delay(std::chrono::milliseconds{100});
                }
                if (valid()) RunSingleProxyProbe(appTasks, proxiesModel, tag, probe, valid, toast);
            }
            tests->singlePreparing.erase(tag);
        });
    };
    const huxerui::TextStyle titleStyle{huxerui::Font::System(font_size::kBodySmall), tile.fg};
    const auto title = UseProxyNameTitle(node.displayName, [&titleStyle](std::string_view name) {
        return SingleLineEllipsisText(name, titleStyle);
    });
    return ProxyNodeTile(title,
        huxerui::Text(node.detail)
            .LineLimit(1).Overflow(huxerui::TextOverflow::Ellipsis)
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption), detailColor}),
        delayView, dotsWidth, selected, node.displayName, std::move(onActivate),
        group->selectable ? SelectableTileInteraction::Selectable : SelectableTileInteraction::ReadOnly,
        !node.isGroup, !node.isGroup && currentProbe.testing, startProbe, insideGroup);
}

// 节点点击动作：**所有成员都只是"选中当前线路"**。成员本身是策略组时（订阅里
// 的 url-test 组，如「自动选择」）表示"这条线路交给它自动挑最快"，与 Clash
// Verge / metacubexd 一致：没有"点进去看子组"这一层，子组是标签栏里的独立分组。
// 切换写在任务协程里（点击节点可能随列表换组卸载）。
std::function<void()> NodeSelectAction(
    huxerui::State<SelectionIntent> intent, huxerui::TaskScope tasks,
    std::shared_ptr<ProxiesModel> proxiesModel, std::function<void()> onFailure,
    std::function<void()> onReadOnly,
    const ProxyGroup& group, const ProxyNode& node) {
    const std::string groupName = group.name;
    const std::string nodeName = node.name;
    if (!group.selectable) return onReadOnly;
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

std::set<std::string> ResolveDegradedProxyGroups(const store::CoreSnapshot& core) {
    std::set<std::string> degraded;
    if (core.fidelity.empty()) return degraded;
    const auto isDegraded = [&](const std::string& source, const std::string& object) {
        return std::ranges::any_of(core.fidelity, [&](const singbox::FidelityNote& note) {
            return note.scope == singbox::FidelityScope::Group &&
                   note.subject == object && note.sourceId == source;
        });
    };
    if (core.sourceObjects) {
        for (const auto& entry : *core.sourceObjects) {
            if (isDegraded(entry.sourceId, entry.objectId)) {
                degraded.insert(entry.tag);
            }
        }
    }
    for (const auto& note : core.fidelity) {
        if (note.scope == singbox::FidelityScope::Group && note.sourceId.empty()) {
            degraded.insert(note.subject);
        }
    }
    return degraded;
}

// 隔离高频 CoreModel 1Hz 流量/健康轮询：仅在加载态、出站模式、保真度角标或
// 代理快照内容真正改变时更新页面 State，避免每秒重建 VirtualGrid 并清空卡片缓存。
[[huxerui::composable]] huxerui::View ProxyPageSync(
    huxerui::TaskScope tasks,
    std::shared_ptr<ProxiesModel> proxiesModel,
    std::shared_ptr<CoreModel> coreModel,
    std::shared_ptr<ProxySyncCache> syncCache,
    huxerui::StateList<ProxyGroupPtr> groups,
    huxerui::State<bool> loaded,
    huxerui::State<std::string> mode,
    huxerui::State<std::set<std::string>> degradedGroups,
    huxerui::State<SelectionIntent> selectionIntent) {
    const auto testModel = proxiesModel->tests;
    huxerui::Lifecycle(
        [tasks, proxiesModel, coreModel, testModel, syncCache, groups,
         loaded, mode, degradedGroups, selectionIntent] {
            const CoreView& coreView = coreModel->view.Get();
            const store::CoreSnapshot& core = coreView.core;
            testModel->ObserveContext(core.state == core::CoreState::Running,
                                      core.planRevision, core.runtimeEpoch);
            const std::string nextMode = coreView.Mode();
            if (mode.Get() != nextMode) mode = nextMode;
            auto nextDegraded = ResolveDegradedProxyGroups(core);
            if (degradedGroups.Get() != nextDegraded) {
                degradedGroups = std::move(nextDegraded);
            }
            const ProxiesSnapshot& current = proxiesModel->snapshot.Get();
            if (loaded.Get() != current.loaded) loaded = current.loaded;
            if (current.body != syncCache->body || current.labels != syncCache->labels) {
                syncCache->labels = current.labels;
                syncCache->body = current.body;
                const auto ticket = ++syncCache->ticket;
                tasks.Launch([groups, selectionIntent, syncCache, ticket,
                              snapshot = current.proxies, labels = current.labels]() -> huxerui::Task<void> {
                    auto parsed = co_await RunOnTaskThread(
                        [snapshot = std::move(snapshot), labels = std::move(labels)] {
                            return parseProxies(snapshot, labels);
                        });
                    if (syncCache->ticket != ticket) co_return;
                    const SelectionIntent pending = selectionIntent.Get();
                    if (!pending.node.empty()) {
                        for (ProxyGroup& parsedGroup : parsed) {
                            if (parsedGroup.name != pending.group) continue;
                            if (parsedGroup.now == pending.node) {
                                selectionIntent = SelectionIntent{};
                            } else {
                                parsedGroup.now = pending.node;
                                const auto selected = std::ranges::find(parsedGroup.nodes, pending.node, &ProxyNode::name);
                                parsedGroup.selectedIndex = selected == parsedGroup.nodes.end() ? std::nullopt
                                    : std::optional<std::size_t>(selected - parsedGroup.nodes.begin());
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
    return huxerui::Row{};
}

[[huxerui::composable]] huxerui::View ProxySpeedSearchAction(
    std::string groupName,
    std::shared_ptr<ProxyTestModel> testModel,
    std::function<void(const std::string&)> triggerGroupTest) {
    const auto start = [triggerGroupTest, groupName] { triggerGroupTest(groupName); };
    return WithoutIconButtonOutlines(ProxySpeedButton(*testModel, groupName, start));
}

struct ProxyNodeActions {
    huxerui::State<SelectionIntent> selection;
    huxerui::TaskScope pageTasks;
    huxerui::TaskScope testTasks;
    CoreControl control;
    FluxToastHandle toast;
    std::shared_ptr<ProxiesModel> model;
    float dotsWidth;
    std::function<void()> onFailure;
    std::function<void()> onReadOnly;

    huxerui::View Card(const ProxyGroupPtr& group, std::size_t index, bool insideGroup = false) const {
        const auto& node = group->nodes[index];
        return NodeCard(group, index, dotsWidth, selection, testTasks, control, toast, model,
            NodeSelectAction(selection, pageTasks, model, onFailure, onReadOnly, *group, node), insideGroup)
            .Key(group->name + "::" + node.name);
    }
};

struct ProxyNodeMatches {
    std::size_t count = 0;
    std::vector<std::size_t> filtered;

    std::size_t NodeIndex(std::size_t index) const { return filtered.empty() ? index : filtered[index]; }
};

ProxyNodeMatches MatchingProxyNodes(const ProxyGroup& group, std::string_view query) {
    if (query.empty()) return {.count = group.nodes.size(), .filtered = {}};
    ProxyNodeMatches matches;
    for (std::size_t i = 0; i < group.nodes.size(); ++i) {
        const auto& node = group.nodes[i];
        if (SearchTextMatches(node.name, query) ||
            (node.displayName != node.name && SearchTextMatches(node.displayName, query)) ||
            SearchTextMatches(node.type, query) || SearchTextMatches(node.detail, query)) {
            matches.filtered.push_back(i);
        }
    }
    matches.count = matches.filtered.size();
    return matches;
}

huxerui::View ProxyGrid(huxerui::VirtualGrid grid, bool compact, ScrollAreaInsets insets,
    const std::shared_ptr<PageScrollOffset>& offset) {
    return ScrollArea(std::move(grid)
        .Columns(compact ? huxerui::GridColumns::Fixed(2) : huxerui::GridColumns::Adaptive(kProxyNodeWidth))
        // Estimate only. Content determines the actual height under font scaling.
        .EstimatedRowExtent(52.0F)
        .CacheExtent(kProxyGridCacheExtent)
        .ColumnSpacing(kSectionCardSpacing)
        .With(RememberPageScroll{offset}), {}, offset, {}, insets)
        .With(huxerui::Grow(1.0F));
}

struct ProxyGroupSection {
    ProxyGroupPtr group;
    ProxyNodeMatches matches;
    std::size_t firstItem;
    bool showOverview;
    bool expandable;
    bool degraded;
};

struct GroupedProxyLayout {
    std::vector<ProxyGroupSection> sections;
    std::size_t itemCount = 0;
};

GroupedProxyLayout BuildGroupedProxyLayout(const std::vector<ProxyGroupPtr>& groups, std::string_view query,
    const std::set<std::string>& degraded, const std::set<std::string>& openGroups, std::size_t columns) {
    GroupedProxyLayout layout;
    layout.sections.reserve(groups.size());
    const bool searching = !query.empty();
    for (const auto& group : groups) {
        const bool open = searching || openGroups.contains(group->name);
        auto matches = open ? MatchingProxyNodes(*group, query) : ProxyNodeMatches{};
        if (searching && matches.count == 0) continue;
        const auto rows = (matches.count + columns - 1) / columns;
        const bool showOverview = !open && !group->nodes.empty();
        layout.sections.push_back({group, std::move(matches), layout.itemCount, showOverview,
            !searching && !group->nodes.empty(), degraded.contains(group->name)});
        layout.itemCount += 1 + rows + static_cast<std::size_t>(showOverview);
    }
    return layout;
}

huxerui::View ProxyGroupSectionHeader(const ProxyGroupSection& group,
    huxerui::State<std::set<std::string>> expanded, std::shared_ptr<ProxyTestModel> testModel,
    std::function<void(const std::string&)> triggerGroupTest,
    const std::shared_ptr<PinnedProxyGroup>& pinned) {
    std::function<void()> toggle;
    if (group.expandable) {
        toggle = [expanded, name = group.group->name, item = group.firstItem + 1, pinned] {
            pinned->AnchorBeforeToggle(item, name);
            expanded.Update([&name](std::set<std::string>& groups) {
                if (groups.erase(name) == 0) groups.insert(name);
            });
        };
    }
    const auto& snapshot = *group.group;
    const auto& selected = snapshot.selectedIndex ? snapshot.nodes[*snapshot.selectedIndex].displayName : snapshot.now;
    const bool open = group.matches.count != 0;
    const auto count = group.expandable || group.showOverview ? snapshot.nodes.size() : group.matches.count;
    return ProxyGroupHeader(snapshot.displayName, snapshot.type, count, group.degraded, open, selected,
        ProxySpeedSearchAction(snapshot.name, testModel, triggerGroupTest), std::move(toggle));
}

[[huxerui::composable]] huxerui::View ProxyGroupOverview(ProxyGroupPtr group,
    std::shared_ptr<ProxyTestModel> tests) {
    const auto& theme = huxerui::UseTheme();
    const auto islands = ResolveIslandTheme(theme);
    static_cast<void>(tests->generation.Get());
    auto testing = theme.colors.on_surface_variant;
    testing.alpha *= 0.5F;
    std::vector<huxerui::View> dots;
    const auto shown = std::min<std::size_t>(20, group->nodes.size());
    dots.reserve(shown);
    for (std::size_t i = 0; i < shown; ++i) {
        const auto selected = group->selectedIndex.value_or(i);
        const auto index = i + 1 == shown && selected >= shown ? selected : i;
        const auto& node = group->nodes[index];
        const auto entry = tests->Get(node.name, node.delay, node.timeout, node.urlTestTime);
        const auto cached = entry->value.Get();
        const auto probe = !cached.testing && node.urlTestTime > entry->baselineTime.Get()
            ? ProbeState{node.delay, node.delay <= 0, false} : cached;
        auto color = islands.overlay;
        if (node.name == group->now) color = theme.colors.primary;
        else if (probe.testing) color = testing;
        else if (probe.timeout) color = theme.colors.error;
        else if (probe.delay > 0) color = DelayLevelColor(theme, probe.delay, false);
        dots.push_back(huxerui::Row{}.With(
            huxerui::Frame{.width = 7.0F, .height = 7.0F},
            huxerui::Background(color), huxerui::CornerRadius(3.5F)));
    }
    return huxerui::Flow(std::move(dots)).With(
        huxerui::Spacing(4.0F),
        huxerui::Padding(huxerui::EdgeInsets{
            .right = kProxyGroupInset, .bottom = kProxyGroupInset, .left = kProxyGroupInset}),
        huxerui::Semantics{.hidden = true})
        .Key("proxy-group-overview-" + group->name);
}

huxerui::View GroupedProxyView(const std::vector<ProxyGroupPtr>& groups, std::string_view query,
    const std::set<std::string>& degraded, huxerui::State<std::set<std::string>> expanded,
    const ProxyNodeActions& actions, const std::function<void(const std::string&)>& triggerGroupTest,
    bool compact, ScrollAreaInsets insets, const std::shared_ptr<PageScrollOffset>& offset,
    huxerui::State<std::size_t> columnCount, huxerui::TaskScope tasks) {
    const std::size_t columns = compact ? 2 : std::max<std::size_t>(1, columnCount.Get());
    auto layout = std::make_shared<const GroupedProxyLayout>(
        BuildGroupedProxyLayout(groups, query, degraded, expanded.Get(), columns));
    if (layout->itemCount == 0) return EmptyState(Localized("没有匹配的节点"), app::images::search);
    // The leading list item uses the same theme radius as the group cards.
    insets.top.reset();
    return huxerui::Scope([=] {
        const float groupGap = ResolveIslandTheme(huxerui::UseTheme()).nested_radius;
        const auto controller = huxerui::UseScrollController(offset->value);
        const auto pinned = huxerui::UseState(
            std::make_shared<PinnedProxyGroup>(PinnedProxyGroup{.remembered = offset})).Get();
        auto list = MakeStickyProxyGroupList(layout->itemCount, [=](std::size_t index) -> huxerui::View {
            const auto next = std::ranges::upper_bound(layout->sections, index, {}, &ProxyGroupSection::firstItem);
            const auto& section = *std::prev(next);
            const auto& group = section.group;
            const bool hasNextGroup = next != layout->sections.end();
            ProxyGroupRow::Value row{.headerIndex = section.firstItem + 1, .expanded = section.matches.count != 0};
            if (index == section.firstItem) {
                const bool last = !row.expanded && !section.showOverview;
                const auto header = ProxyGroupSectionHeader(
                    section, expanded, actions.model->tests, triggerGroupTest, pinned);
                return ProxyGroupPanelPart(header, true, last, last && hasNextGroup)
                    .LayoutValue<ProxyGroupRow>(row)
                    .LayoutValue<ProxyGroupHeaderName>(group->name)
                    .Key("proxy-group-header-" + group->name);
            }
            if (section.showOverview) {
                return ProxyGroupPanelPart(ProxyGroupOverview(group, actions.model->tests), false, true, hasNextGroup)
                    .LayoutValue<ProxyGroupRow>(row)
                    .Key("proxy-group-overview-row-" + group->name);
            }
            const auto start = (index - section.firstItem - 1) * columns;
            const auto end = std::min(start + columns, section.matches.count);
            std::vector<huxerui::View> cards;
            cards.reserve(columns);
            for (std::size_t i = start; i < end; ++i) {
                cards.push_back(actions.Card(group, section.matches.NodeIndex(i), true));
            }
            const bool last = end == section.matches.count;
            if (last) row.trailingGap = hasNextGroup ? groupGap : 0.0F;
            return ProxyGroupPanelPart(ProxyGroupNodeRow(std::move(cards), columns, last),
                false, last, last && hasNextGroup)
                .LayoutValue<ProxyGroupRow>(row)
                .Key("proxy-group-row-" + group->name + "::" + group->nodes[section.matches.NodeIndex(start)].name);
        }, pinned).With(RememberPageScroll{offset}, MeasureProxyGroupColumns{columnCount, tasks, compact});
        return ProxyGroupViewport{ScrollArea(list, controller, offset, {}, insets)};
    })
        .With(huxerui::Grow(1.0F)).Key("proxy-grouped-grid");
}

} // namespace

[[huxerui::composable]] huxerui::View ProxiesPage(bool active) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = UseFluxToast();
    // 策略组快照 / 内核状态的全局唯一来源（见 *_model.h）。
    const auto proxiesModel = huxerui::UseService<ProxiesModel>();
    const auto coreModel = huxerui::UseService<CoreModel>();
    const auto settingsModel = huxerui::UseService<SettingsModel>();
    const auto control = UseCoreControl();
    auto groups = huxerui::UseStateList<ProxyGroupPtr>();
    auto loaded = huxerui::UseState(proxiesModel->snapshot.Get().loaded);
    auto mode = huxerui::UseState<std::string>("rule");
    auto degradedGroups = huxerui::UseState<std::set<std::string>>({});
    const auto [proxySearch, searchExpanded] = UsePageSearch(active);
    auto sectionMotion = UseSectionTabMotion();
    auto expandedGroups = huxerui::UseState<std::set<std::string>>({});
    auto groupedColumns = huxerui::UseState<std::size_t>(2);
    const auto groupedScroll = huxerui::UseState(std::make_shared<PageScrollOffset>()).Get();
    const auto tabScroll = huxerui::UseState(
        std::make_shared<std::map<std::string, std::shared_ptr<PageScrollOffset>>>()).Get();
    const auto testTasks = huxerui::UseApplicationTaskScope();
    const auto testModel = proxiesModel->tests;
    // 分支路径按模式独立（互不共享）：规则模式 path[0] = 标签栏选中的订阅组，
    // 全局模式 path[0] = GLOBAL 或平台返回的实际可选组；后续元素 = 逐级点入的嵌套子组。
    auto rulePath = huxerui::UseState<std::vector<std::string>>({});
    auto globalPath = huxerui::UseState<std::vector<std::string>>({});
    const auto syncCache = huxerui::UseState(std::make_shared<ProxySyncCache>()).Get();
    const auto probeDotsMetrics = huxerui::UseState(ProxyProbeDotsMetrics{});
    huxerui::Lifecycle([syncCache] { return [syncCache] { ++syncCache->ticket; }; }, 0);
    // 乐观选中意图（见 SelectionIntent）：只写这个小组 State，不动 groups。
    auto selectionIntent = huxerui::UseState<SelectionIntent>(SelectionIntent{});
    const huxerui::View sync = ProxyPageSync(
        tasks, proxiesModel, coreModel, syncCache, groups,
        loaded, mode, degradedGroups, selectionIntent).Key("proxies-sync");

    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    // 不可见时只保留本页 State/Lifecycle，不构建重子树：huxerui 的 Pager 会把
    // 四个一级页同时挂载，隐藏页即使不重组，其已挂载子树仍随每一帧被重新测量。
    // 真机实测（代理页大分组）：四页同挂时每帧 1443 次测量请求 / ~20ms，
    // 只留当前页后降到 28 次 / ~0ms；因此不可见页必须返回空占位。
    if (!active) return huxerui::Row { sync }.Key("proxies-idle");

    // 组测速触发：桌面统一走内核 /group/{name}/delay（URLTest 组保留订阅的
    // url-test.url，内核内部并发 10）；Android 调用 libbox urlTest，UI 只消费
    // 内核回写的结果。
    const std::function<void(const std::string&)> triggerGroupTest =
        [testTasks, toast, testModel, control, proxiesModel](const std::string& name) {
            const auto batch = testModel->GetGroup(name);
            const int ticket = testModel->Begin(*batch);
            if (!ticket) return;
            testTasks.Launch([testTasks, toast, control, proxiesModel, name, batch, ticket] {
                return RunGroupProxyProbe(testTasks, control, proxiesModel, name, batch, ticket, toast);
            });
        };

    // 按出站模式解析根分组与当前组（渲染期校验：组消失回落，嵌套前缀逐级
    // 校验）。直连不经过节点，不展示订阅组。
    const huxerui::StateList<ProxyGroupPtr> all = groups;
    const bool direct = mode.Get() == "direct";
    const bool global = mode.Get() == "global";
    const auto syntheticGlobal = findGroup(all, "GLOBAL");
    // 全局模式的根组 = 内核**实际**走哪个出站。sing-box 的 GLOBAL 是 clash_api
    // 合成的只读组（type Fallback，`PUT /proxies/GLOBAL` 会 404），它的 `now`
    // 等于内核的默认出站（route.final = 订阅 MATCH 目标，缺省回落首个 selector 组）。
    // 直接拿 GLOBAL 当根组会让整屏卡片都点不动；随便挑一个组又会出现「改了不影响
    // 全局流量」的假象。所以按 `now` 找回那个真实组，只有找不回时才退回 GLOBAL。
    ProxyGroupPtr globalRoot;
    if (global) {
        const std::string globalTarget =
            syntheticGlobal != nullptr ? syntheticGlobal->now : std::string{};
        if (!globalTarget.empty()) {
            const auto target = findGroup(all, globalTarget);
            if (target != nullptr && target != syntheticGlobal) globalRoot = target;
        }
        if (globalRoot == nullptr) globalRoot = syntheticGlobal;
        if (globalRoot == nullptr) {
            for (const auto& g : all) {
                if (g->name != "GLOBAL" && g->selectable) {
                    globalRoot = g;
                    break;
                }
            }
        }
        if (globalRoot == nullptr && !all.Empty()) {
            for (const auto& g : all) {
                if (g->name != "GLOBAL") {
                    globalRoot = g;
                    break;
                }
            }
        }
    }

    // 标签栏列出订阅里的**每个**策略组（与 Clash Verge / metacubexd 一致）：
    // url-test 组（如订阅里的「自动选择」）也是独立分组——既能在父组里被选为
    // 线路（＝交给它自动挑最快），也能直接切到它自己的标签查看/测速，因此不再
    // 需要"点进子组"那一层。全局模式只保留 GLOBAL（或平台回落组），直连不走组。
    std::vector<ProxyGroupPtr> rootGroups;
    if (global) {
        if (globalRoot != nullptr) rootGroups.push_back(globalRoot);
    } else if (!direct) {
        for (const auto& g : all) {
            if (g->name == "GLOBAL") continue;
            rootGroups.push_back(g);
        }
    }
    std::vector<std::string> tabNames;
    tabNames.reserve(rootGroups.size());
    for (const auto& g : rootGroups) tabNames.push_back(g->name);

    // 活动路径：按模式取独立 State，渲染期校验（组消失回落，嵌套前缀逐级
    // 校验）。标签栏是分组入口：路径必须落在某个标签上，否则回落到首个标签；
    // 嵌套层级（path.size() > 1）由面包屑返回。
    huxerui::State<std::vector<std::string>> activePath =
        global ? globalPath : rulePath;
    std::vector<std::string> path = resolvePath(all, activePath.Get());
    const bool rootSelected =
        !path.empty() && std::ranges::any_of(rootGroups, [&](const ProxyGroupPtr& g) {
            return g->name == path.front();
        });
    if (!rootSelected) {
        path = tabNames.empty() ? std::vector<std::string>{}
                                : std::vector<std::string>{tabNames.front()};
    }
    const auto current =
        path.empty() ? nullptr : findGroup(all, path.back());
    const std::string selectedRoot = path.empty() ? std::string{} : path.front();
    std::size_t selectedTab = tabNames.size();
    for (std::size_t i = 0; i < tabNames.size(); ++i) {
        if (tabNames[i] == selectedRoot) selectedTab = i;
    }

    // 分组标签与左右滑动直接更新路径，让选中态和内容在同一帧切换。
    const std::function<void(const std::string&)> selectGroup =
        [activePath, tabNames, selectedRoot](const std::string& name) {
            const auto& latestPath = activePath.Get();
            const std::string from = !latestPath.empty() &&
                                             std::ranges::find(tabNames, latestPath.front()) != tabNames.end()
                                         ? latestPath.front() : selectedRoot;
            if (from == name) return;
            const auto next = std::ranges::find(tabNames, name);
            if (next == tabNames.end()) return;
            activePath = std::vector<std::string>{name};
        };


    const auto& settings = settingsModel->view.Get();
    if (!loaded.Get() || !settings.ready) {
        huxerui::View loading = (kPageTitlesInWindow && !compact)
            ? PageLoadingScaffold(Localized("代理"), theme)
            : PageScaffold(Localized("代理"), huxerui::View{}, ProxyLoadingSkeleton(theme),
                           false, true, true);
        return huxerui::Stack {
            sync,
            std::move(loading),
        }.With(huxerui::Grow(1.0F),
               huxerui::Align(huxerui::HorizontalAlignment::Stretch,
                              huxerui::VerticalAlignment::Stretch));
    }

    auto gridInsets = ScrollAreaInsets::Symmetric(kSectionCardSpacing,
        compact ? kCompactFloatingNavigationInset : 0.0F);
    gridInsets.rail_bottom_avoidance = 0.0F;
    const ProxyNodeActions actions{selectionIntent, tasks, testTasks, control, toast, proxiesModel,
        probeDotsMetrics->Width(),
        [toast] { toast.Show(Localized("线路切换失败，请查看日志")); },
        [toast] { toast.Show(Localized("此分组不支持手动切换节点，仅作展示")); },
    };
    const std::string query = proxySearch.Get().text;
    const auto& degraded = degradedGroups.Get();
    const bool groupedMode = settings.proxyGroupedView;

    huxerui::View content;
    if (direct) {
        content = EmptyState(Localized("直连模式 —— 流量不经过任何代理节点"), app::images::proxies);
    } else if (rootGroups.empty()) {
        content = EmptyState(Localized(global ? "全局模式暂无可用策略组"
                                           : "暂无策略组（检查订阅配置）"), app::images::proxies);
    } else if (groupedMode) {
        content = GroupedProxyView(rootGroups, query, degraded, expandedGroups, actions,
            triggerGroupTest, compact, gridInsets, groupedScroll, groupedColumns, tasks);
    } else {
        // The Pager measures just the participating pages; each group retains its own scroll position.
        std::vector<huxerui::View> groupPages;
        std::vector<SectionTab> groupTabs;
        groupPages.reserve(rootGroups.size());
        groupTabs.reserve(rootGroups.size());
        for (std::size_t page = 0; page < rootGroups.size(); ++page) {
            const ProxyGroup& root = *rootGroups[page];
            groupTabs.push_back({root.name, root.displayName, degraded.contains(root.name) ? "!" : ""});
            const auto& group = page == selectedTab && current != nullptr ? current : rootGroups[page];
            auto visibleNodes = MatchingProxyNodes(*group, query);
            huxerui::View grid;
            if (visibleNodes.count == 0 && !query.empty()) {
                grid = EmptyState(Localized("没有匹配的节点"), app::images::search);
            } else {
                auto& offset = (*tabScroll)[group->name];
                if (!offset) offset = std::make_shared<PageScrollOffset>();
                const auto count = visibleNodes.count;
                auto matches = std::make_shared<const ProxyNodeMatches>(std::move(visibleNodes));
                auto items = PageVirtualGrid(count,
                    [group, matches, actions](std::size_t index) {
                        return actions.Card(group, matches->NodeIndex(index));
                    }, kSectionCardSpacing);
                grid = ProxyGrid(std::move(items), compact, gridInsets, offset).Key("group-grid-" + group->name);
            }
            groupPages.push_back(huxerui::Column {grid}.With(
                huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
                .Key("group-page-" + root.name));
        }
        auto pages = SectionTabPages(groupPages, selectedTab, [selectGroup, tabNames](std::size_t index) {
            if (index < tabNames.size()) selectGroup(tabNames[index]);
        }, sectionMotion);
        content = huxerui::Column {
          SectionTabBar(groupTabs, selectedRoot, selectGroup, sectionMotion, SectionTabPickerMode::ResponsiveGroups),
          std::move(pages).With(huxerui::Grow(1.0F)),
        }.With(huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }

    huxerui::View toolbarActions;
    if (!rootGroups.empty()) {
        const auto actionLabel = Localized(groupedMode ? "切换到横滑视图" : "切换到分组视图");
        const auto icon = groupedMode ? app::images::proxy_view_tabs : app::images::proxy_view_groups;
        huxerui::View speed;
        if (groupedMode) {
            speed = WithoutIconButtonOutlines(ProxyAllSpeedButton(testModel, tabNames, triggerGroupTest));
        } else if (current != nullptr) {
            speed = ProxySpeedSearchAction(current->name, testModel, triggerGroupTest);
        }
        toolbarActions = huxerui::Row {
          speed,
          huxerui::IconButton(icon, actionLabel).OnClick([settingsModel] {
              settingsModel->SetProxyGroupedView(!settingsModel->view.Get().proxyGroupedView);
          })
              .With(huxerui::Tooltip(actionLabel)).Key("proxy-view-toggle"),
        }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    }
    const auto title = Localized("代理");
    huxerui::View searchToolbar = PageSearchToolbar(
        proxySearch, searchExpanded, title, Localized("搜索节点"), toolbarActions);
    huxerui::View page = PageScaffold(title, {}, std::move(content),
                                    false, true, true, std::nullopt, {}, std::move(searchToolbar));
    return huxerui::Stack {sync, page}.With(
        huxerui::Grow(1.0F),
        huxerui::Align(huxerui::HorizontalAlignment::Stretch,
                       huxerui::VerticalAlignment::Stretch));
}

} // namespace clashflux::ui
