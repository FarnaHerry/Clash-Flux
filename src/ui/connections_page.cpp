// connections_page.cpp — 连接页：WS /connections 推送快照（约 1Hz 全量），
// 表头（总量 + 关闭全部）+ VirtualList 行（链 | 目标 | 上/下行 | 规则 | 关闭）。
//
// 数据流：IX 线程把每帧原文推进 CoreStreams 槽位并通知一次；UI 线程收到通知
// 才解析写 StateList（全量快照语义，直接整表替换，无需差分）。
#include <huxerui/huxerui.h>

#include <chrono>
#include <algorithm>
#include <string>
#include <vector>

#include "app_resources.h"
#include "ui.h"
#include "task_bridge.h"

import nlohmann.json;
import clashflux.core;
import clashflux.store.core;
import clashflux.stream;
import clashflux.utils;

// 用到 stream::StreamKind，必须在模块导入之后。
#include "stream_updates.h"

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
            if (row.host.empty() && m.contains("destination") &&
                m["destination"].is_string()) {
                row.host = m["destination"].get<std::string>();
            }
            const std::string port = m.value("destinationPort", "");
            if (!port.empty()) row.host += ":" + port;
            row.network = m.value("network", "");
        }
        if (row.host.empty() && c.contains("destination") &&
            c["destination"].is_string()) {
            row.host = c["destination"].get<std::string>();
        }
        if (row.network.empty() && c.contains("network") &&
            c["network"].is_string()) {
            row.network = c["network"].get<std::string>();
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
        if (row.chains.empty() && c.contains("chain") &&
            c["chain"].is_string()) {
            row.chains = c["chain"].get<std::string>();
        }
        row.rule = c.value("rule", "");
        const std::string payload = c.value("rulePayload", "");
        if (!payload.empty()) row.rule += "(" + payload + ")";
        if (row.host.empty()) row.host = "未知目标";
        if (row.network.empty()) row.network = "—";
        if (row.chains.empty()) row.chains = "DIRECT";
        if (row.rule.empty()) row.rule = "未匹配规则";
        snap.rows.push_back(std::move(row));
    }
    return snap;
}

struct ConnectionFrame {
    bool open = false;
    std::string body;
};

ConnectionFrame readConnectionFrame() {
    ConnectionFrame frame;
#if defined(__ANDROID__)
    frame.open = AndroidVpnState() == 2;
    if (const char* body = clashflux_android_connections();
        body != nullptr && *body != '\0') {
        frame.body = body;
    }
#else
    auto& streams = store::coreStore().streams();
    frame.open = streams.connectionsOpen();
    streams.readConnections(frame.body);
#endif
    return frame;
}

void closeConnectionForPlatform(const std::string& id) {
#if defined(__ANDROID__)
    clashflux_android_close_connection(id.c_str());
#else
    store::coreStore().api().closeConnection(id);
#endif
}

void closeAllConnectionsForPlatform() {
#if defined(__ANDROID__)
    clashflux_android_close_all_connections();
#else
    store::coreStore().api().closeAllConnections();
#endif
}

} // namespace

[[huxerui::composable]] huxerui::View ConnectionsPage(
    std::function<void()> onBack, bool active) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    auto tasks = huxerui::UseTaskScope();
    auto rows = huxerui::UseStateList<ConnectionRow>();
    auto totalUp = huxerui::UseState<std::int64_t>(0);
    auto totalDown = huxerui::UseState<std::int64_t>(0);
    auto streamOpen = huxerui::UseState(false);
    auto searching = huxerui::UseState(false);
    auto searchValue = huxerui::UseState(huxerui::TextEditingValue{""});
    const auto scroll = huxerui::UseScrollController();

    huxerui::Lifecycle(
        [tasks, rows, totalUp, totalDown, streamOpen] {
            // 帧原文比较用共享对象保存：页面重进的首次读取与后续推送共用同一份。
            auto lastFrame = std::make_shared<std::string>();
            const auto applyFrame = [rows, totalUp, totalDown, streamOpen,
                                     lastFrame] {
                const ConnectionFrame frame = readConnectionFrame();
                streamOpen = frame.open;
                if (!frame.body.empty() && frame.body != *lastFrame) {
                    *lastFrame = frame.body;
                    ConnectionsSnapshot snapshot = parseConnections(frame.body);
                    totalUp = snapshot.totalUp;
                    totalDown = snapshot.totalDown;
                    ReplaceStateList(rows, std::move(snapshot.rows));
                }
            };
            // 先消费最近一次快照：切页/重挂载不必等下一帧推送。
            applyFrame();
            const std::uint64_t subscription = SubscribeStreamUpdates(
                tasks, [applyFrame](stream::StreamKind kind) {
                    if (kind != stream::StreamKind::Connections) return;
                    applyFrame();
                });
            return [subscription] { UnsubscribeStreamUpdates(subscription); };
        },
        0);

    // 不可见时只保留本页 State/Lifecycle，不构建内容：桌面 IndexedPages 让七个
    // 一级页同帧参与测量，隐藏页（日志/连接有推送流更新）的重子树会拖慢每一次渲染。
    if (!active) return huxerui::View{huxerui::Row{}}.Key("connections-idle");

    auto mono = [](const std::string& text, huxerui::Color color, float width) {
        huxerui::View t = huxerui::Text(text).Style(huxerui::TextStyle{
            huxerui::Font::Monospace(font_size::kMonoBody), color});
        if (width > 0.0F) return std::move(t).With(huxerui::Frame{.width = width});
        return std::move(t).With(huxerui::Grow(1.0F));
    };

    const std::string query = searchValue.Get().text;
    huxerui::View body = huxerui::Column {
        huxerui::Text(Localized(!query.empty() ? "没有匹配的连接"
                                   : streamOpen.Get() ? "暂无活动连接"
                                                      : "连接流未就绪（内核未运行？）"))
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                      theme.colors.on_surface_variant}),
    }.With(huxerui::Padding(32.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));

    std::vector<std::size_t> visibleRows;
    for (std::size_t index = 0; index < rows.Size(); ++index) {
        if (query.empty() || rows[index].host.find(query) != std::string::npos) {
            visibleRows.push_back(index);
        }
    }
    if (!visibleRows.empty()) {
        const std::size_t rowCount = visibleRows.size();
        body = huxerui::VirtualList(
                   rowCount + (compact ? 1U : 0U),
                   [rows, visibleRows, tasks, mono, theme, compact, rowCount](
                       std::size_t index) -> huxerui::View {
                       if (compact && index == rowCount) {
                           return CompactFloatingNavigationFooter()
                               .Key("compact-floating-footer");
                       }
                       const ConnectionRow& row = rows[visibleRows[index]];
                       const std::string host =
                           row.host == "未知目标"
                               ? huxerui::UseString(Localized("未知目标"))
                               : row.host;
                       const std::string rule =
                           row.rule == "未匹配规则"
                               ? huxerui::UseString(Localized("未匹配规则"))
                               : row.rule;
                       const std::string id = row.id;
                       const std::string key = id.empty()
                                                   ? std::format("connection-{}", index)
                                                   : id;
                       const auto closeButton = [tasks, id] {
                           return huxerui::IconButton(app::images::close,
                                                      Localized("关闭连接"))
                               .With(huxerui::Tooltip(Localized("关闭该连接")))
                               .OnClick([tasks, id] {
                                   if (id.empty()) return;
                                   tasks.Launch([=]() -> huxerui::Task<void> {
                                       co_await RunOnTaskThread([=] {
                                           closeConnectionForPlatform(id);
                                       });
                                   });
                               });
                       };
                       if (compact) {
                           return UnifiedListRow(
                               huxerui::Column{
                                   huxerui::Row{
                                       mono(host, theme.colors.on_surface, 0.0F),
                                       mono(row.network,
                                            theme.colors.on_surface_variant, 55.0F),
                                       closeButton(),
                                   }
                                       .With(huxerui::Spacing(6.0F),
                                             huxerui::CrossAlign(
                                                 huxerui::CrossAxisAlignment::Center)),
                                   mono(huxerui::UseString(LocalizedFormat("链路：{}", row.chains)),
                                        theme.colors.on_surface_variant, 0.0F),
                                   huxerui::Row{
                                       mono(std::format("↑{}/s ↓{}/s", formatBytes(row.up),
                                                        formatBytes(row.down)),
                                            theme.colors.on_surface_variant, 0.0F),
                                   mono(huxerui::UseString(LocalizedFormat("规则：{}", rule)),
                                            theme.colors.on_surface_variant, 0.0F),
                                   }
                                       .With(huxerui::Spacing(8.0F)),
                               },
                               key, true, index + 1 < rowCount);
                       }
                       return UnifiedListRow(
                           huxerui::Row{
                               mono(host, theme.colors.on_surface, 0.0F),
                               mono(row.network, theme.colors.on_surface_variant, 50.0F),
                               mono(row.chains, theme.colors.on_surface_variant, 220.0F),
                               mono(std::format("↑{}/s ↓{}/s", formatBytes(row.up),
                                                formatBytes(row.down)),
                                    theme.colors.on_surface_variant, 160.0F),
                               mono(rule, theme.colors.on_surface_variant, 140.0F),
                               closeButton(),
                           },
                           key, false, index + 1 < rowCount);
                   })
                   .EstimatedItemExtent(compact ? 92.0F : 44.0F)
                   .Controller(scroll)
                   .With(huxerui::Grow(1.0F), huxerui::ScrollBar());
    }

    huxerui::View title = searching.Get()
        ? PillSearchField(searchValue, Localized("搜索连接"), [searching, searchValue] {
              searchValue = huxerui::TextEditingValue{""};
              searching = false;
          })
        : huxerui::View{huxerui::Text(Localized("连接"), huxerui::TextRole::Title)};
    huxerui::View actions = searching.Get()
        ? huxerui::View{huxerui::Row{}}
        : huxerui::View{huxerui::Row {
              huxerui::IconButton(app::images::search, Localized("搜索连接"))
                  .With(huxerui::Tooltip(Localized("搜索连接")))
                  .OnClick([searching] { searching = true; }),
              huxerui::IconButton(app::images::clear_all, Localized("关闭全部"))
                  .With(huxerui::Tooltip(Localized("关闭全部连接")))
                  .OnClick([tasks] {
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        co_await RunOnTaskThread([] {
                            closeAllConnectionsForPlatform();
                        });
                    });
                  }),
          }.With(huxerui::Spacing(6.0F),
                 huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))};
    return onBack
        ? SecondaryPageScaffold(std::move(title), std::move(actions),
                                std::move(body), onBack, searching.Get())
        : PageScaffold(Localized("连接"), std::move(actions), std::move(body));
}

} // namespace clashflux::ui
