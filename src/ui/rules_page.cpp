// rules_page.cpp — 规则页：GET /rules 快照列表（类型 | 载荷 | 走向策略组）。
// 内核 Running 时每 5s 轮询刷新；表头行 + VirtualList 行。
#include <huxerui/huxerui.h>

#include <chrono>
#include <string>
#include <vector>

#include "ui.h"
#include "task_bridge.h"

import nlohmann.json;
import clashflux.core;
import clashflux.store.core;

namespace clashflux::ui {
namespace {

struct RuleRow {
    std::string type;     // DOMAIN-SUFFIX / GEOIP / MATCH ...
    std::string payload;  // google.com / CN / "" ...
    std::string proxy;    // 走向（策略组名 / DIRECT / REJECT）

    bool operator==(const RuleRow&) const = default;
};

std::vector<RuleRow> parseRules(const std::string& body) {
    std::vector<RuleRow> out;
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (!j.is_object() || !j.contains("rules") || !j["rules"].is_array()) return out;
    for (const auto& r : j["rules"]) {
        if (!r.is_object()) continue;
        out.push_back({.type = r.value("type", ""),
                       .payload = r.value("payload", ""),
                       .proxy = r.value("proxy", "")});
    }
    return out;
}

} // namespace

[[huxerui::composable]] huxerui::View RulesPage() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto rules = huxerui::UseState<std::vector<RuleRow>>({});
    auto running = huxerui::UseState(false);
    auto refreshTick = huxerui::UseState(0);  // 手动刷新触发器

    huxerui::Lifecycle(
        [tasks, rules, running, refreshTick] {
            tasks.Launch([=]() -> huxerui::Task<void> {
                int lastTick = -1;
                for (;;) {
                    const auto snap = store::coreStore().snapshot();
                    running = snap.state == core::CoreState::Running;
                    if (running.Get()) {
                        const auto r = co_await RunOnTaskThread([] {
                            return store::coreStore().api().rules();
                        });
                        if (r.ok) rules = parseRules(r.body);
                        // 手动刷新信号到达前每 5s 一拍。
                        for (int i = 0; i < 25; ++i) {
                            co_await huxerui::Delay(std::chrono::duration<double>{0.2});
                            if (refreshTick.Get() != lastTick) {
                                lastTick = refreshTick.Get();
                                break;
                            }
                        }
                    } else {
                        if (!rules.Get().empty()) rules = {};
                        co_await huxerui::Delay(std::chrono::duration<double>{0.5});
                    }
                }
            });
            return [] {};
        },
        0);

    auto mono = [&theme](const std::string& text, huxerui::Color color) {
        return huxerui::Text(text).Style(huxerui::TextStyle{
            huxerui::Font::Monospace(font_size::kMonoBody), color});
    };

    huxerui::View body = huxerui::Column {
        huxerui::Text(running.Get() ? "暂无规则" : "内核未运行")
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                      theme.colors.on_surface_variant}),
    }.With(huxerui::Padding(32.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));

    if (!rules.Get().empty()) {
        body = huxerui::Column {
            huxerui::Row {
                mono("类型", theme.colors.on_surface_variant)
                    .With(huxerui::Frame{.width = 150.0F}),
                mono("载荷", theme.colors.on_surface_variant)
                    .With(huxerui::Grow(1.0F)),
                mono("走向", theme.colors.on_surface_variant)
                    .With(huxerui::Frame{.width = 180.0F}),
            }.With(huxerui::Spacing(8.0F),
                   huxerui::Padding(huxerui::EdgeInsets::Symmetric(4.0F, 2.0F))),
            huxerui::Divider(),
            huxerui::VirtualList(rules.Get(),
                                 [mono, theme](const RuleRow& rule) {
                                     return huxerui::Row {
                                         mono(rule.type, theme.colors.primary)
                                             .With(huxerui::Frame{.width = 150.0F}),
                                         mono(rule.payload, theme.colors.on_surface)
                                             .With(huxerui::Grow(1.0F)),
                                         mono(rule.proxy,
                                              theme.colors.on_surface_variant)
                                             .With(huxerui::Frame{.width = 180.0F}),
                                     }
                                         .With(huxerui::Spacing(8.0F),
                                               huxerui::Padding(
                                                   huxerui::EdgeInsets::Symmetric(
                                                       4.0F, 5.0F)))
                                         .Key(rule.type + "|" + rule.payload + "|" +
                                              rule.proxy);
                                 })
                .EstimatedItemExtent(28.0F)
                .With(huxerui::Grow(1.0F), huxerui::ScrollBar()),
        }.With(huxerui::Spacing(4.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
               huxerui::Grow(1.0F));
    }

    return PageScaffold(
        "规则",
        huxerui::Button("刷新").OnClick([refreshTick] {
            refreshTick = refreshTick.Get() + 1;
        }),
        std::move(body));
}

} // namespace clashflux::ui
