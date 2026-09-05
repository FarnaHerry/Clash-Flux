// proxies_page.cpp — 代理页：策略组卡片（类型徽标 / 当前节点 / 整体测速）+
// 节点网格（延迟着色徽标，点击切换节点）。
//
// 数据流：PollWhile 每 2s 检查内核状态；Running 时经 RunOnTaskThread 拉
// GET /proxies 解析成组模型写 State。测速 / 切节点都是阻塞 REST，全部走
// RunOnTaskThread；点击事件处理器内不直接写 State（约定 6），只 Launch 协程。
#include <huxerui/huxerui.h>

#include <chrono>
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

struct ProxyNode {
    std::string name;
    std::string type;
    int delay = 0;      // 0 = 未测；来自 history 或测速结果
    bool timeout = false;
    bool udp = false;

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

// 延迟着色：未测灰 / <300ms 绿 / <1000ms 琥珀 / 超时红。
huxerui::Color delayColor(const huxerui::ThemeSpec& theme, int delay, bool timeout) {
    if (timeout) return theme.colors.error;
    if (delay <= 0) return theme.colors.on_surface_variant;
    if (delay < 300) return huxerui::Color::Rgb(34, 197, 94);
    if (delay < 1000) return huxerui::Color::Rgb(245, 158, 11);
    return theme.colors.error;
}

[[huxerui::composable]] huxerui::View NodeChip(
    const ProxyNode& node, bool selected,
    huxerui::State<std::unordered_map<std::string, int>> delays,
    huxerui::State<std::unordered_map<std::string, bool>> timeouts,
    std::function<void()> onSelect) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
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

    huxerui::Color bg = theme.colors.surface_container_high;
    huxerui::Color fg = theme.colors.on_surface;
    if (selected) {
        bg = theme.colors.primary;
        fg = theme.colors.on_primary;
    }
    return huxerui::Column {
        huxerui::Text(node.name).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kChip), fg}),
        huxerui::Text(timeout ? "超时" : (delay > 0 ? std::format("{} ms", delay)
                                                     : node.type))
            .Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption),
                selected ? fg : delayColor(theme, delay, timeout)}),
    }
        .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 6.0F)),
              huxerui::Background(bg),
              huxerui::CornerRadius(theme.shapes.small),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Start),
              huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                 .label = node.name},
              huxerui::Focusable(true))
        .OnClick([onSelect = std::move(onSelect)] { onSelect(); });
}

[[huxerui::composable]] huxerui::View GroupCard(
    const ProxyGroup& group,
    huxerui::State<std::unordered_map<std::string, int>> delays,
    huxerui::State<std::unordered_map<std::string, bool>> timeouts,
    huxerui::State<std::string> testingGroup,
    huxerui::TaskScope tasks) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const bool testing = testingGroup.Get() == group.name;

    std::vector<huxerui::View> chips;
    for (const auto& node : group.nodes) {
        const std::string groupName = group.name;
        const std::string nodeName = node.name;
        chips.push_back(
            NodeChip(node, node.name == group.now, delays, timeouts,
                     [tasks, groupName, nodeName] {
                         tasks.Launch([=]() -> huxerui::Task<void> {
                             co_await RunOnTaskThread([=] {
                                 store::coreStore().api().selectProxy(groupName,
                                                                      nodeName);
                             });
                         });
                     })
                .Key(node.name));
    }

    return Card(huxerui::Column {
        huxerui::Row {
            huxerui::Text(group.name).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody)
                    .WithWeight(huxerui::FontWeight::SemiBold),
                theme.colors.on_surface}),
            huxerui::Text(group.type).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption),
                theme.colors.on_surface_variant}),
            huxerui::Spacer(),
            testing
                ? huxerui::View{huxerui::ProgressCircle()
                                    .With(huxerui::Frame{.width = 18.0F,
                                                         .height = 18.0F})}
                : huxerui::View{huxerui::Button("测速").OnClick(
                      [tasks, delays, timeouts, testingGroup,
                       name = group.name] {
                          tasks.Launch([=]() -> huxerui::Task<void> {
                              testingGroup = name;
                              const auto r = co_await RunOnTaskThread([=] {
                                  return store::coreStore().api().groupDelay(
                                      name, "https://www.gstatic.com/generate_204",
                                      3000);
                              });
                              if (r.ok) {
                                  const auto j = nlohmann::json::parse(
                                      r.body, nullptr, false);
                                  if (j.is_object()) {
                                      auto d = delays.Get();
                                      auto t = timeouts.Get();
                                      for (auto it = j.begin(); it != j.end(); ++it) {
                                          const auto& v = it.value();
                                          if (v.is_number_integer() &&
                                              v.get<int>() > 0) {
                                              d[it.key()] = v.get<int>();
                                              t.erase(it.key());
                                          } else {
                                              t[it.key()] = true;
                                              d.erase(it.key());
                                          }
                                      }
                                      delays = d;
                                      timeouts = t;
                                  }
                              }
                              testingGroup = "";
                          });
                      })},
        }.With(huxerui::Spacing(10.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Flow(std::move(chips))
            .With(huxerui::Spacing(8.0F)),
    }.With(huxerui::Spacing(10.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
}

} // namespace

[[huxerui::composable]] huxerui::View ProxiesPage() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto groups = huxerui::UseState<std::vector<ProxyGroup>>({});
    auto coreState = huxerui::UseState<core::CoreState>(core::CoreState::Stopped);
    auto delays = huxerui::UseState<std::unordered_map<std::string, int>>({});
    auto timeouts = huxerui::UseState<std::unordered_map<std::string, bool>>({});
    auto testingGroup = huxerui::UseState<std::string>("");

    // 数据泵：内核 Running 时每 3s 刷一次 /proxies（保持 now/history 新鲜）；
    // 非 Running 清空列表。
    huxerui::Lifecycle(
        [tasks, groups, coreState] {
            tasks.Launch([=]() -> huxerui::Task<void> {
                for (;;) {
                    const auto snap = store::coreStore().snapshot();
                    coreState = snap.state;
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

    huxerui::View body = huxerui::Column {
        huxerui::Text(coreState.Get() == core::CoreState::Running
                          ? "暂无策略组（检查订阅配置）"
                          : "内核未运行 —— 请到设置页启动内核")
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                      theme.colors.on_surface_variant}),
    }.With(huxerui::Padding(32.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));

    if (!groups.Get().empty()) {
        std::vector<huxerui::View> cards;
        for (const auto& g : groups.Get()) {
            cards.push_back(GroupCard(g, delays, timeouts, testingGroup, tasks)
                                .Key(g.name));
        }
        body = huxerui::ScrollView(
                   huxerui::Column(std::move(cards))
                       .With(huxerui::Spacing(10.0F),
                             huxerui::CrossAlign(
                                 huxerui::CrossAxisAlignment::Stretch)))
            .With(huxerui::Grow(1.0F));
    }

    return PageScaffold("代理", huxerui::Row{}, std::move(body));
}

} // namespace clashflux::ui
