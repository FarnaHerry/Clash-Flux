// connections_page.cpp — 连接页：WS /connections 推送快照（约 1Hz 全量），
// 表头（总量 + 关闭全部）+ VirtualList 行（链 | 目标 | 上/下行 | 规则 | 关闭）。
//
// 数据流：IX 线程把每帧原文推进 CoreStreams 槽位；UI 泵每 500ms takeConnections
// 取最新帧解析写 State（全量快照语义，直接整表替换，无需差分）。
#include <huxerui/huxerui.h>

#include <chrono>
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

struct ConnectionRow {
    std::string id;
    std::string host;       // metadata.host 或 destinationIP:port
    std::string network;    // tcp / udp
    std::string chains;     // 节点链 "DIRECT" / "节点 ← 策略组"
    std::string rule;       // 命中规则
    std::int64_t up = 0;
    std::int64_t down = 0;
    std::string start;      // HH:MM:SS

    bool operator==(const ConnectionRow&) const = default;
};

struct ConnectionsSnapshot {
    std::vector<ConnectionRow> rows;
    std::int64_t totalUp = 0;
    std::int64_t totalDown = 0;

    bool operator==(const ConnectionsSnapshot&) const = default;
};

ConnectionsSnapshot parseConnections(const std::string& body) {
    ConnectionsSnapshot snap;
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (!j.is_object()) return snap;
    snap.totalUp = j.value("uploadTotal", std::int64_t{0});
    snap.totalDown = j.value("downloadTotal", std::int64_t{0});
    if (!j.contains("connections") || !j["connections"].is_array()) return snap;
    for (const auto& c : j["connections"]) {
        if (!c.is_object()) continue;
        ConnectionRow row;
        row.id = c.value("id", "");
        row.up = c.value("upload", std::int64_t{0});
        row.down = c.value("download", std::int64_t{0});
        if (c.contains("metadata") && c["metadata"].is_object()) {
            const auto& m = c["metadata"];
            row.host = m.value("host", "");
            if (row.host.empty()) {
                row.host = m.value("destinationIP", "");
            }
            const std::string port = m.value("destinationPort", "");
            if (!port.empty()) row.host += ":" + port;
            row.network = m.value("network", "");
        }
        if (c.contains("chains") && c["chains"].is_array()) {
            std::string chains;
            for (const auto& hop : c["chains"]) {
                if (!hop.is_string()) continue;
                if (!chains.empty()) chains += " ← ";
                chains += hop.get<std::string>();
            }
            row.chains = std::move(chains);
        }
        row.rule = c.value("rule", "");
        const std::string payload = c.value("rulePayload", "");
        if (!payload.empty()) row.rule += "(" + payload + ")";
        snap.rows.push_back(std::move(row));
    }
    return snap;
}

} // namespace

[[huxerui::composable]] huxerui::View ConnectionsPage() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto snap = huxerui::UseState<ConnectionsSnapshot>({});
    auto streamOpen = huxerui::UseState(false);

    huxerui::Lifecycle(
        [tasks, snap, streamOpen] {
            tasks.Launch([=]() -> huxerui::Task<void> {
                co_await PollWhile(std::chrono::duration<double>{0.5}, [=] {
                    auto& streams = store::coreStore().streams();
                    streamOpen = streams.connectionsOpen();
                    std::string frame;
                    if (streams.takeConnections(frame)) {
                        snap = parseConnections(frame);
                    }
                    return true;
                });
            });
            return [] {};
        },
        0);

    auto mono = [&theme](const std::string& text, huxerui::Color color, float width) {
        huxerui::View t = huxerui::Text(text).Style(huxerui::TextStyle{
            huxerui::Font::Monospace(font_size::kMonoBody), color});
        if (width > 0.0F) return std::move(t).With(huxerui::Frame{.width = width});
        return std::move(t).With(huxerui::Grow(1.0F));
    };

    const ConnectionsSnapshot s = snap.Get();

    huxerui::View body = huxerui::Column {
        huxerui::Text(streamOpen.Get() ? "暂无活动连接"
                                       : "连接流未就绪（内核未运行？）")
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                      theme.colors.on_surface_variant}),
    }.With(huxerui::Padding(32.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));

    if (!s.rows.empty()) {
        std::vector<huxerui::View> rows;
        for (const auto& row : s.rows) {
            const std::string id = row.id;
            rows.push_back(
                huxerui::Row {
                    mono(row.host, theme.colors.on_surface, 0.0F),
                    mono(row.network, theme.colors.on_surface_variant, 50.0F),
                    mono(row.chains, theme.colors.on_surface_variant, 220.0F),
                    mono(std::format("↑{} ↓{}", formatBytes(row.up),
                                     formatBytes(row.down)),
                         theme.colors.on_surface_variant, 160.0F),
                    mono(row.rule, theme.colors.on_surface_variant, 140.0F),
                    huxerui::Text("✕").Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kCaption),
                        theme.colors.on_surface_variant})
                        .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(
                                  8.0F, 2.0F)),
                              huxerui::Semantics{.role =
                                                     huxerui::SemanticRole::Button,
                                                 .label = "关闭连接"},
                              huxerui::Focusable(true),
                              huxerui::Tooltip("关闭该连接"))
                        .OnClick([tasks, id] {
                            tasks.Launch([=]() -> huxerui::Task<void> {
                                co_await RunOnTaskThread([=] {
                                    store::coreStore().api().closeConnection(id);
                                });
                            });
                        }),
                }
                    .With(huxerui::Spacing(8.0F),
                          huxerui::Padding(huxerui::EdgeInsets::Symmetric(4.0F, 5.0F)),
                          huxerui::CrossAlign(
                              huxerui::CrossAxisAlignment::Center))
                    .Key(row.id));
        }
        body = huxerui::Column(std::move(rows))
            .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
        body = huxerui::ScrollView(std::move(body)).With(huxerui::Grow(1.0F));
    }

    return PageScaffold(
        std::format("连接（{} 条 · ↑{} ↓{}）", s.rows.size(),
                    formatBytes(s.totalUp), formatBytes(s.totalDown)),
        huxerui::Button("关闭全部")
            .OnClick([tasks] {
                tasks.Launch([=]() -> huxerui::Task<void> {
                    co_await RunOnTaskThread([] {
                        store::coreStore().api().closeAllConnections();
                    });
                });
            }),
        std::move(body));
}

} // namespace clashflux::ui
