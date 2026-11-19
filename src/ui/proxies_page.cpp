// proxies_page.cpp — 代理页：分组标签栏 + 选中分组的节点卡网格。出站模式
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
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "proxies_model.h"
#include "ui.h"
#include "page_layout.h"
#include "page_loading.h"
#include "empty_state.h"
#include "search_text.h"
#include "task_bridge.h"
#include "proxy_node_tile.h"
#include "proxy_speed_button.h"

#include "wire_codec.h"
import clashflux.config;
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

    bool operator==(const ProxyGroup&) const = default;
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
        for (const auto& member : *value.members) {
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

// Windows' bundled emoji font displays regional-indicator pairs as country
// letters. Draw only the leading flag from the packaged atlas; keep the
// original UTF-8 name for proxy identity, search and accessibility.
huxerui::View NodeTitle(const std::string& name, huxerui::Color foreground) {
    const huxerui::TextStyle style{huxerui::Font::System(font_size::kBodySmall), foreground};
    const auto plain = [&] {
        return SingleLineEllipsisText(name, style);
    };
#ifdef _WIN32
    if (name.size() < 8) return plain();
    const auto* bytes = reinterpret_cast<const unsigned char*>(name.data());
    if (bytes[0] != 0xF0 || bytes[1] != 0x9F || bytes[2] != 0x87 ||
        bytes[4] != 0xF0 || bytes[5] != 0x9F || bytes[6] != 0x87 ||
        bytes[3] < 0xA6 || bytes[3] > 0xBF ||
        bytes[7] < 0xA6 || bytes[7] > 0xBF) return plain();
    struct Atlas {
        huxerui::ImageAsset image;
        std::string codes;
    };
    static const std::optional<Atlas> atlas = []() -> std::optional<Atlas> {
        try {
            const auto directory = cfg::executableDir() / "assets";
            std::ifstream codesFile(directory / "flags.codes", std::ios::binary);
            if (!codesFile) return std::nullopt;
            std::string codes(std::istreambuf_iterator<char>{codesFile}, {});
            return Atlas{huxerui::ImageAsset::FromFile(
                             huxerui::File((directory / "flags.png").string())),
                         std::move(codes)};
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }();
    if (!atlas) return plain();
    const std::string code{
        static_cast<char>('a' + bytes[3] - 0xA6),
        static_cast<char>('a' + bytes[7] - 0xA6)};
    const auto position = atlas->codes.find(code);
    if (position == std::string::npos || position % 3 != 0) return plain();
    const std::size_t index = position / 3;
    constexpr float cellWidth = 48.0F, cellHeight = 36.0F;
    constexpr std::size_t columns = 16;
    auto flag = huxerui::Canvas([image = atlas->image, index](
            huxerui::PaintContext& paint, huxerui::Size size) {
        paint.DrawImageRect(image,
            {static_cast<float>(index % columns) * cellWidth,
             static_cast<float>(index / columns) * cellHeight,
             cellWidth, cellHeight},
            {0.0F, 0.0F, size.width, size.height});
    }).With(huxerui::Frame{.width = 20.0F, .height = 15.0F});
    return huxerui::Row {
      flag,
      SingleLineEllipsisText(std::string_view{name}.substr(8), style).With(huxerui::Grow(1.0F)),
    }.With(huxerui::Spacing(4.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
#else
    return plain();
#endif
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
// 196 是"满屏 7 列"的阈值：1560 逻辑宽的满屏窗口去掉侧栏与岛内边距后约
// 1428 逻辑 px，1428 / (196 + 8) ≈ 7.0 → 7 列（每张约 197 px）；更宽的窗口
// 列数继续增加，更窄时逐级回落（卡片文本截断省略，窄列不溢出）。
constexpr float kProxyNodeWidth = 196.0F;

// 虚拟网格的固定缓存高度。宽屏列数多时，框架默认的 0.5 视口高度会让每个
// 滚动帧反复回收/重建边界卡片（15 列 × 96 帧约 7.3 万次工厂调用，见
// docs/evidence/2026-11-16-proxy-grid-scroll.md）；200pt 保留约 3 行预热，
// 消除该抖动并把同时挂载的卡片数减少约四成。
constexpr float kProxyGridCacheExtent = 200.0F;

// 桌面右下角悬浮测速按钮的滚动尾部净空：56pt 按钮 + 24pt 底部外边距。
constexpr float kSpeedFabFooterHeight = 80.0F;

// sing-box constant.TCPTimeout；单节点和组测速都使用内核同一上限。
constexpr int kProxyDelayTimeoutMs = 15000;

void RunProxyTestWhenReady(CoreControl control, std::shared_ptr<ProxyTestModel> tests,
                           FluxToastHandle toast, std::function<huxerui::Task<void>()> measure) {
#if defined(__ANDROID__) || defined(CLASHFLUX_IOS)
    if (control.model->view.Get().core.state == core::CoreState::Running)
        control.tasks.Launch(std::move(measure));
    else toast.Show(Localized("测速需要内核就绪，请检查内核服务"));
#else
    if (control.model->closing || tests->preparing.Get()) return;
    if (!control.Busy() && control.model->view.Get().core.state == core::CoreState::Running) {
        control.tasks.Launch(std::move(measure));
        return;
    }
    tests->preparing = true;
    control.tasks.Launch([control, tests, toast, measure = std::move(measure)]() -> huxerui::Task<void> {
        while (!control.model->closing &&
               (control.model->operations.Busy() || control.model->serviceChecking.Get()))
            co_await huxerui::Delay(std::chrono::duration<double>{0.1});
        if (control.model->closing) {
            tests->preparing = false;
            co_return;
        }
        if (control.model->view.Get().core.state == core::CoreState::Running) {
            tests->preparing = false;
            co_await measure();
            co_return;
        }
        if (!control.Run({.action = CoreAction::Start, .origin = CoreCommandOrigin::ProxyTest},
                [tests, tasks = control.tasks, measure = std::move(measure)](bool ready) mutable {
                    tests->preparing = false;
                    if (ready) tasks.Launch(std::move(measure));
                })) {
            tests->preparing = false;
            toast.Show(Localized("测速需要内核就绪，请检查内核服务"));
        }
    });
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

huxerui::Task<int> MeasureProxyDelay(const std::string& tag) {
    co_return co_await RunOnTaskThread([tag] {
        // Empty URL lets sing-box use its own default test URL (HTTPS
        // www.gstatic.com/generate_204), matching CommandClient.urlTest and
        // /proxies/{name}/delay without an explicit url parameter.
        const auto result = store::coreStore().api().proxyDelay(tag, "", kProxyDelayTimeoutMs);
        if (!result.ok) return 0;
        const auto delay = wire::DecodeDelay(result.body);
        return delay ? delay.value : 0;
    });
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

// 节点名占满首行，末行左侧显示协议或组信息，右侧显示延迟或测速动画。
[[huxerui::composable]] huxerui::View NodeCard(
    const ProxyNode& node, std::string groupName, std::string groupNow, float dotsWidth,
    huxerui::State<SelectionIntent> selectionIntent, bool selectable,
    huxerui::TaskScope appTasks, CoreControl control, FluxToastHandle toast,
    std::shared_ptr<ProxyTestModel> tests, std::function<void()> onActivate) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const SelectionIntent pending = selectionIntent.Get();
    const bool selected = node.name == (pending.group == groupName ? pending.node : groupNow);
    static_cast<void>(tests->generation.Get()); // Rebind after a runtime-plan change.
    const auto entry = tests->Get(node.name, node.delay, node.timeout, node.urlTestTime);
    const auto probe = entry->value;
    huxerui::Lifecycle([entry, nodeDelay = node.delay, nodeTestTime = node.urlTestTime] {
        if (!entry->value.Get().testing && nodeTestTime > 0 && nodeTestTime != entry->baselineTime.Get()) {
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
    const auto startProbe = [appTasks, control, tests, tag = node.name, delay = node.delay,
                             timeout = node.timeout, testTime = node.urlTestTime, toast] {
        if (tests->progress.Get().busy) return;
        RunProxyTestWhenReady(control, tests, toast,
                [appTasks, model = control.model, tests, tag, delay, timeout, testTime]() -> huxerui::Task<void> {
            const auto valid = BindProxyTest(model, *tests);
            if (!valid()) co_return;
            const auto probe = tests->Get(tag, delay, timeout, testTime);
            LaunchSingleProxyProbe(appTasks, tests, tag, probe, MeasureProxyDelay, valid);
            co_return;
        });
    };
    return ProxyNodeTile(NodeTitle(node.displayName, tile.fg),
        huxerui::Text(node.detail)
            .LineLimit(1).Overflow(huxerui::TextOverflow::Ellipsis)
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption), detailColor}),
        delayView, dotsWidth, selected, node.displayName, std::move(onActivate),
        selectable ? SelectableTileInteraction::Selectable : SelectableTileInteraction::ReadOnly,
        !node.isGroup && !ProxyTestUsesNativeGroup(), !node.isGroup && currentProbe.testing,
        tests->preparing.Get() || tests->progress.Get().busy, startProbe);
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
    huxerui::StateList<ProxyGroup> groups,
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

[[huxerui::composable]] huxerui::View ProxySpeedFloatingAction(
    std::string groupName, bool compact,
    std::shared_ptr<ProxyTestModel> testModel,
    std::function<void(const std::string&)> triggerGroupTest) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const bool testing = testModel->progress.Get().busy || testModel->preparing.Get();
    const auto start = [triggerGroupTest, groupName] { triggerGroupTest(groupName); };
    return huxerui::Column {
      WithoutIconButtonOutlines(ProxySpeedButton(testing, testModel->singlePending.Get() == 0, start)),
    }.With(huxerui::Padding(huxerui::EdgeInsets{
               .right = theme.spacing.medium + (compact ? 0.0F : kScrollBarGutter),
               .bottom = compact ? kCompactFloatingNavigationInset
                                 : theme.spacing.large,
               .left = theme.spacing.medium,
           }),
           huxerui::MainAlign(huxerui::MainAxisAlignment::End),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::End));
}

} // namespace

[[huxerui::composable]] huxerui::View ProxiesPage(bool active) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = UseFluxToast();
    // 策略组快照 / 内核状态的全局唯一来源（见 *_model.h）。
    const auto proxiesModel = huxerui::UseService<ProxiesModel>();
    const auto coreModel = huxerui::UseService<CoreModel>();
    const auto control = UseCoreControl();
    auto groups = huxerui::UseStateList<ProxyGroup>();
    auto loaded = huxerui::UseState(proxiesModel->snapshot.Get().loaded);
    auto mode = huxerui::UseState<std::string>("rule");
    auto degradedGroups = huxerui::UseState<std::set<std::string>>({});
    auto proxySearch = huxerui::UseState(huxerui::TextEditingValue{});
    auto sectionMotion = UseSectionTabMotion();
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

    // 组测速触发：桌面统一走内核 /group/{name}/delay（URLTest 组保留订阅的
    // url-test.url，内核内部并发 10）；Android 调用 libbox urlTest，UI 只消费
    // 内核回写的结果。
    const std::function<void(const std::string&)> triggerGroupTest =
        [testTasks, toast, testModel, control, proxiesModel, groups](std::string groupName) {
            if (testModel->progress.Get().busy || testModel->singlePending.Get() != 0) return;
            std::vector<ProxyNode> nodes;
            for (const auto& group : groups) {
                if (group.name == groupName) nodes = group.nodes;
            }
            RunProxyTestWhenReady(control, testModel, toast,
                [testTasks, toast, testModel, coreModel = control.model, proxiesModel,
                 nodes = std::move(nodes), groupName = std::move(groupName)]() -> huxerui::Task<void> {
                const auto valid = BindProxyTest(coreModel, *testModel);
                if (!valid()) co_return;
                const int generation = testModel->Begin();
                if (!generation) co_return;
                bool ok = false;
                try {
                    ok = co_await RunOnTaskThread([groupName] { return StartProxyGroupTest(groupName); });
                } catch (const std::exception&) {}
                if (!testModel->Current(generation)) co_return;
                if (!ok || !valid()) {
                    testModel->End(generation);
                    toast.Show(Localized("测速失败：内核未运行或启动失败"));
                    co_return;
                }
                std::vector<ProxyProbeTarget> targets;
                for (const auto& node : nodes) {
                    if (node.isGroup) continue;
                    const auto probe = testModel->Get(node.name, node.delay, node.timeout, node.urlTestTime);
                    if (testModel->Start(probe, generation, node.urlTestTime)) targets.push_back({node.name, probe});
                }
                const bool native = ProxyTestUsesNativeGroup();
                const auto pollProbe = [proxiesModel](const std::string& tag)
                    -> std::optional<std::pair<int, std::int64_t>> {
                    const auto snapshot = proxiesModel->snapshot.Get().proxies;
                    if (!snapshot) return std::nullopt;
                    const auto node = snapshot->entries.find(tag);
                    if (node == snapshot->entries.end()) return std::nullopt;
                    return std::pair{node->second.delay, node->second.testTime};
                };
                if (native) {
                    LaunchNativeProxyProbeBatch(testTasks, testModel, std::move(targets), generation, valid,
                        pollProbe);
                } else {
                    // 所有组都由内核批量测速；不要逐节点 fan-out，否则会
                    // 丢失组 URL / 组参数，也无法得到与 sing-box 相同的
                    // 并发和历史写入顺序。内核批处理期间轮询 /proxies，
                    // 让每个节点在自己的结果写入 history 后立即显示。
                    LaunchProxyGroupProbeBatch(testTasks, testModel, groupName,
                        std::move(targets), generation, MeasureProxyGroupDelay, valid,
                        pollProbe);
                }
                testModel->Broadcast(generation, groupName);
                co_await WaitForProxyTestBatch(testModel, generation, native);
            });
        };

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

    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    // 不可见时只保留本页 State/Lifecycle，不构建重子树：huxerui 的 Pager 会把
    // 四个一级页同时挂载，隐藏页即使不重组，其已挂载子树仍随每一帧被重新测量。
    // 真机实测（代理页大分组）：四页同挂时每帧 1443 次测量请求 / ~20ms，
    // 只留当前页后降到 28 次 / ~0ms；因此不可见页必须返回空占位。
    if (!active) return huxerui::Row { sync }.Key("proxies-idle");

    if (!loaded.Get()) {
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

    const float dotsWidth = probeDotsMetrics->Width();
    // 一个根分组一页。VirtualGrid 直接用索引读取组节点，不再为所有组预先
    // 分配完整的 ProxyItem 与 span 数组；只有视口附近的节点会被构造为卡片。
    // 每个根组仅声明轻量虚拟网格；Pager 提供有界视口并只测量参与显示的页。
    // 不给各页叠加单独入场动画，避免与 Pager 的完整出入场轨道冲突。
    // 桌面悬浮测速按钮存在时，滚动尾部要留出它的净空，否则最后一行节点
    // 会被按钮遮住。
    const bool showSpeedFab = !direct && current != nullptr;
    auto gridInsets = ScrollAreaInsets::Symmetric(kSectionCardSpacing,
        compact ? kCompactFloatingNavigationInset : (showSpeedFab ? kSpeedFabFooterHeight : 0.0F));
    gridInsets.rail_bottom_avoidance = 0.0F;
    const std::function<void()> onSelectFailure = [toast] {
        toast.Show(Localized("线路切换失败，请查看日志"));
    };
    const std::function<void()> onSelectReadOnly = [toast] {
        toast.Show(Localized("此分组不支持手动切换节点，仅作展示"));
    };
    std::vector<huxerui::View> groupPages;
    groupPages.reserve(rootGroups.size());
    for (std::size_t page = 0; page < rootGroups.size(); ++page) {
        const ProxyGroup& rootGroup = *rootGroups[page];
        // 只有当前页可能是嵌套子组（path 更深）；其余页就是各自的根分组。
        const bool selectedPage = page == selectedTab;
        const ProxyGroup* contentGroup =
            (selectedPage && current != nullptr) ? current : &rootGroup;

        const auto groupSnapshot = std::make_shared<const ProxyGroup>(*contentGroup);
        const std::string query = proxySearch.Get().text;
        std::vector<std::size_t> visibleNodes;
        visibleNodes.reserve(groupSnapshot->nodes.size());
        for (std::size_t index = 0; index < groupSnapshot->nodes.size(); ++index) {
            const ProxyNode& node = groupSnapshot->nodes[index];
            if (SearchTextMatches(node.name, query) ||
                SearchTextMatches(node.displayName, query) ||
                SearchTextMatches(node.type, query) ||
                SearchTextMatches(node.detail, query)) {
                visibleNodes.push_back(index);
            }
        }
        const std::size_t nodeCount = visibleNodes.size();
        if (!query.empty() && nodeCount == 0) {
            groupPages.push_back(huxerui::Column{
                EmptyState(Localized("没有匹配的节点"), app::images::search),
            }.With(huxerui::Grow(1.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
                 .Key("group-page-" + rootGroup.name));
            continue;
        }
        huxerui::View grid = ScrollArea(PageVirtualGrid(
                                 nodeCount,
                                 [groupSnapshot, visibleNodes = std::move(visibleNodes),
                                  tasks, testTasks, control, toast, testModel,
                                  onSelectFailure, onSelectReadOnly,
                                  nodeCount, proxiesModel, dotsWidth,
                                  selectionIntent](std::size_t index)
                                     -> huxerui::View {
                                     if (index >= nodeCount ||
                                         visibleNodes[index] >= groupSnapshot->nodes.size()) {
                                         return huxerui::View{};
                                     }
                                     const ProxyNode& node =
                                         groupSnapshot->nodes[visibleNodes[index]];
                                     const std::string& nodeName = node.name;
                                     return NodeCard(
                                                node, groupSnapshot->name, groupSnapshot->now, dotsWidth,
                                                selectionIntent, groupSnapshot->selectable,
                                                testTasks, control, toast, testModel,
                                                NodeSelectAction(
                                                    selectionIntent, tasks,
                                                    proxiesModel,
                                                    onSelectFailure,
                                                    onSelectReadOnly,
                                                    *groupSnapshot, node))
                                         .Key(groupSnapshot->name + "::" + nodeName);
                                 }, kSectionCardSpacing)
                                 .Columns(compact
                                              ? huxerui::GridColumns::Fixed(2)
                                              : huxerui::GridColumns::Adaptive(
                                                    kProxyNodeWidth))
                                 // 行高**只给估算值**，真实高度由虚拟布局测量卡片内容
                                 // 得到：卡片是「两行内容 + 上下 8pt 内边距」的
                                 // 自适应高度（字体缩放、系统字号变化都会改高度），
                                 // 写死精确行高会在这些情况下裁切/错位。
                                 .EstimatedRowExtent(52.0F)
                                 .CacheExtent(kProxyGridCacheExtent)
                                 .ColumnSpacing(kSectionCardSpacing), {}, {}, {},
                                 gridInsets)
                                 .With(huxerui::Grow(1.0F))
                                 .Key("group-grid-" + rootGroup.name);
        groupPages.push_back(huxerui::Column {grid}.With(
            huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
            .Key("group-page-" + rootGroup.name));
    }

    huxerui::View body;
    if (direct) {
        body = EmptyState(Localized("直连模式 —— 流量不经过任何代理节点"), app::images::proxies);
    } else if (rootGroups.empty()) {
        body = EmptyState(Localized(global ? "全局模式暂无可用策略组"
                                           : "暂无策略组（检查订阅配置）"), app::images::proxies);
    } else {
        body = SectionTabPages(groupPages, selectedTab, [selectGroup, tabNames](std::size_t index) {
            if (index < tabNames.size()) selectGroup(tabNames[index]);
        }, sectionMotion);
    }

    // 标签栏固定在页面顶部（不随节点列表滚动），只有选中分组的节点参与滚动。
    std::vector<huxerui::View> columnChildren;
    if (!tabNames.empty()) {
        std::vector<SectionTab> groupTabs;
        groupTabs.reserve(tabNames.size());
        // 保真度角标：这份订阅编译时被降级 / 跳过的组，在标签上带一个 "!"。这里只
        // 做「哪个组有问题」的定位，明细在设置页「配置保真度」
        // （见 docs/singbox-layers-and-fidelity.md §2）。
        const std::set<std::string> degraded = degradedGroups.Get();
        for (const std::string& name : tabNames) {
            groupTabs.push_back(SectionTab{
                name, findGroup(all, name) ? findGroup(all, name)->displayName : name,
                degraded.contains(name) ? std::string{"!"} : std::string{}});
        }
        columnChildren.push_back(
            SectionTabBar(groupTabs, selectedRoot, selectGroup, sectionMotion,
                          SectionTabPickerMode::ResponsiveGroups));
    }
    huxerui::View content = std::move(body);
    if (!columnChildren.empty()) {
        columnChildren.push_back(std::move(content).With(huxerui::Grow(1.0F)));
        content =
            huxerui::Column(std::move(columnChildren))
                .With(huxerui::Spacing(0.0F),
                      huxerui::Grow(1.0F),
                      huxerui::CrossAlign(
                          huxerui::CrossAxisAlignment::Stretch));
    }

    // 出站模式切换入口在首页卡片；代理页标题行只保留节点搜索胶囊。
    huxerui::View searchField = PillSearchField(
        proxySearch, Localized("搜索节点"));
    huxerui::View page = PageScaffold(Localized("代理"), huxerui::View{},
                                      std::move(content), false, true, true,
                                      std::nullopt, {}, std::move(searchField));
    std::vector<huxerui::View> layers;
    layers.reserve(3);
    layers.push_back(sync);
    layers.push_back(std::move(page));
    // 延迟测试按钮统一收在页面右下角：测试当前选中的分组。Compact 下要避开
    // 悬浮底部导航，桌面只留常规外边距。
    if (!direct && current != nullptr) {
        layers.push_back(ProxySpeedFloatingAction(
            current->name, compact, testModel, triggerGroupTest));
    }
    return huxerui::Stack(std::move(layers)).With(
        huxerui::Grow(1.0F),
        huxerui::Align(huxerui::HorizontalAlignment::Stretch,
                       huxerui::VerticalAlignment::Stretch));
}

} // namespace clashflux::ui
