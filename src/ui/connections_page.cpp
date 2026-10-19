// connections_page.cpp — 连接页：WS /connections 推送快照（约 1Hz 全量），
// 表头（总量 + 关闭全部）+ VirtualList 密集卡片（链 | 目标 | 上/下行 | 规则 | 关闭）。
//
// 数据流：IX 线程把每帧原文推进 CoreStreams 槽位并通知一次；UI 线程收到通知
// 启动任务线程读取与解析，回到 UI 线程发布 StateList；旧任务不能覆盖新快照。
#include <huxerui/huxerui.h>

#include <chrono>
#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app_resources.h"
#include "ui.h"
#include "connection_row.h"
#include "empty_state.h"
#include "page_layout.h"
#include "search_text.h"
#include "task_bridge.h"

#include "wire_codec.h"
import clashflux.core;
import clashflux.store.core;
import clashflux.stream;
import clashflux.utils;

// 用到 stream::StreamKind，必须在模块导入之后。
#include "stream_updates.h"

namespace clashflux::ui {
namespace {

struct ConnectionsSnapshot {
    std::vector<ConnectionRow> rows;
    std::int64_t totalUp = 0;
    std::int64_t totalDown = 0;

    bool operator==(const ConnectionsSnapshot&) const = default;
};

std::optional<ConnectionsSnapshot> parseConnections(const std::string& body) {
    ConnectionsSnapshot snap;
    const auto decoded = wire::DecodeConnections(body);
    if (!decoded) return std::nullopt;
    snap.totalUp = decoded.value.uploadTotal;
    snap.totalDown = decoded.value.downloadTotal;
    std::map<std::string, std::string> labels;
    const auto catalog = store::coreStore().snapshot().sourceObjects;
    if (catalog) for (const auto& object : *catalog)
        labels[object.tag] = object.objectId + (object.sourceName.empty() ? "" : " · " + object.sourceName);
    for (const auto& connection : decoded.value.entries) {
        ConnectionRow row;
        row.id = connection.id;
        row.up = connection.upload;
        row.down = connection.download;
        row.host = connection.host;
        if (row.host.empty()) row.host = connection.destinationIP;
        if (row.host.empty()) row.host = connection.destination;
        if (!connection.destinationPort.empty()) {
            const std::string portSuffix = ":" + connection.destinationPort;
            // Android libbox returns metadata.destination as host:port when no
            // domain is available, while still providing destinationPort.
            // Desktop snapshots usually keep those fields separate. Avoid
            // rendering the Android fallback as host:port:port.
            if (!row.host.ends_with(portSuffix)) row.host += portSuffix;
        }
        row.network = connection.network;
        for (const auto& hop : connection.chains) {
            if (!row.chains.empty()) row.chains += " ← ";
            row.chains += labels.contains(hop) ? labels.at(hop) : hop;
        }
        if (row.chains.empty()) row.chains = labels.contains(connection.chain) ? labels.at(connection.chain) : connection.chain;
        row.rule = connection.rule;
        if (!connection.rulePayload.empty()) row.rule += "(" + connection.rulePayload + ")";
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

// Android 没有 /connections WS 通道（libbox 状态走 Java CommandClient），也没
// 有人发布 StreamKind::Connections 通知：页面激活期间必须按固定节拍重读跨进
// 程快照文件，否则挂载之后才启动代理时，页面会永远停在挂载时刻读到的空快照。
// 桌面由 WS 推送驱动（SubscribeStreamUpdates），不轮询。条件编译留在文件作用
// 域 helper，composable 函数体内不做条件编译。
void StartSnapshotPoll(huxerui::TaskScope tasks, bool active,
                       std::shared_ptr<std::atomic<bool>> polling,
                       std::function<void()> applyFrame) {
#if defined(__ANDROID__)
    if (!active) return;
    tasks.Launch([tasks, polling, applyFrame]() -> huxerui::Task<void> {
        co_await PollWhile(std::chrono::duration<double>{1.0},
                           [polling, applyFrame] {
                               if (!polling->load()) return false;
                               applyFrame();
                               return true;
                           });
    });
#else
    static_cast<void>(tasks);
    static_cast<void>(active);
    static_cast<void>(polling);
    static_cast<void>(applyFrame);
#endif
}

} // namespace

[[huxerui::composable]] huxerui::View ConnectionsPage(
    std::function<void()> onBack, bool active) {
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    const bool stacked =
        huxerui::UseViewportClass() != huxerui::ViewportClass::Expanded;
    auto tasks = huxerui::UseTaskScope();
    auto rows = huxerui::UseStateList<ConnectionRow>();
    auto totalUp = huxerui::UseState<std::int64_t>(0);
    auto totalDown = huxerui::UseState<std::int64_t>(0);
    auto streamOpen = huxerui::UseState(false);
    auto searchValue = huxerui::UseState(huxerui::TextEditingValue{""});
    const auto scroll = huxerui::UseScrollController();
    // 水平留白与 SectionTabPages 同约定：骨架 fullWidthSections 后由页面根
    // 持有半份卡片间距，不再叠加旧的 24pt 桌面岛内边距。
    const auto sectionInsets = huxerui::UseEnvironment<SectionTabContentInsets>();

    huxerui::Lifecycle(
        [tasks, rows, totalUp, totalDown, streamOpen, active] {
            // 帧原文比较用共享对象保存：本次激活内的首次读取与后续推送共用
            // 同一份；重新激活（依赖变化）重建后强制重新解析，拿到最新快照。
            auto lastFrame = std::make_shared<std::string>();
            auto generation = std::make_shared<std::uint64_t>(0);
            // 节拍泵的在位标志：依赖变化/卸载的 cleanup 置 false，让上一轮
            // 轮询在下一个节拍退出（TaskScope 活整个页面生命周期，不会随
            // Lifecycle 依赖变化取消已 Launch 的协程）。
            auto polling = std::make_shared<std::atomic<bool>>(true);
            const auto applyFrame = [tasks, rows, totalUp, totalDown, streamOpen,
                                     lastFrame, generation] {
                const auto ticket = ++*generation;
                tasks.Launch([=]() -> huxerui::Task<void> {
                    try {
                        auto frame = co_await RunOnTaskThread(readConnectionFrame);
                        if (ticket != *generation) co_return;
                        streamOpen = frame.open;
                        if (frame.body.empty() || frame.body == *lastFrame) co_return;
                        *lastFrame = frame.body;
                        auto snapshot = co_await RunOnTaskThread(
                            [body = std::move(frame.body)] { return parseConnections(body); });
                        if (ticket != *generation) co_return;
                        if (!snapshot) {
                            stream::logApplication("warning", "连接快照格式错误，保留最近有效快照");
                            co_return;
                        }
                        totalUp = snapshot->totalUp;
                        totalDown = snapshot->totalDown;
                        ReplaceStateList(rows, std::move(snapshot->rows));
                    } catch (const std::exception& error) {
                        stream::logApplication("error", std::string("读取连接快照失败：") + error.what());
                    }
                });
            };
            // 先消费最近一次快照：切页/重挂载不必等下一帧推送。
            applyFrame();
            const std::uint64_t subscription = SubscribeStreamUpdates(
                tasks, [applyFrame](stream::StreamKind kind) {
                    if (kind != stream::StreamKind::Connections) return;
                    applyFrame();
                });
            StartSnapshotPoll(tasks, active, polling, applyFrame);
            return [subscription, polling] {
                UnsubscribeStreamUpdates(subscription);
                polling->store(false);
            };
        },
        active);

    // 不可见时只保留本页 State/Lifecycle，不构建内容：桌面 IndexedPages 让七个
    // 一级页同帧参与测量，隐藏页（日志/连接有推送流更新）的重子树会拖慢每一次渲染。
    if (!active) return huxerui::View{huxerui::Row{}}.Key("connections-idle");

    const std::string query = searchValue.Get().text;
    huxerui::View body = EmptyState(
        Localized(!query.empty() ? "没有匹配的连接"
                  : streamOpen.Get() ? "暂无活动连接" : "连接流未就绪（内核未运行？）"),
        app::images::connections);

    std::vector<std::size_t> visibleRows;
    for (std::size_t index = 0; index < rows.Size(); ++index) {
        const ConnectionRow& row = rows[index];
        if (SearchTextMatches(row.host, query) ||
            SearchTextMatches(row.network, query) ||
            SearchTextMatches(row.chains, query) ||
            SearchTextMatches(row.rule, query)) {
            visibleRows.push_back(index);
        }
    }
    if (!visibleRows.empty()) {
        const std::size_t rowCount = visibleRows.size();
        body = huxerui::VirtualList(
                   rowCount + (compact ? 1U : 0U),
                   [rows, visibleRows, tasks, compact, stacked, rowCount](
                       std::size_t index) -> huxerui::View {
                       if (compact && index == rowCount) {
                           return CompactFloatingNavigationFooter()
                               .Key("compact-floating-footer");
                       }
                       const ConnectionRow& row = rows[visibleRows[index]];
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
                       return DenseCard(ConnectionRowContent(row,
                               std::format("↑{}/s ↓{}/s", formatBytes(row.up), formatBytes(row.down)),
                               stacked, closeButton()).With(
                                   huxerui::Spacing(stacked ? 5.0F : 8.0F),
                                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
                           .With(huxerui::Padding(huxerui::EdgeInsets{.bottom = 4.0F}))
                           .Key(key);
                   })
                   // 16pt 卡内留白 + 内容 + 4pt 卡间距，与 DenseCard 的密度档一致。
                   .EstimatedItemExtent(stacked ? 104.0F : 52.0F)
                   .Controller(scroll)
                   .With(huxerui::Grow(1.0F), huxerui::ScrollBar());
    }

    huxerui::View searchField = PillSearchField(
        searchValue, Localized("搜索连接"));
    huxerui::View closeAllButton = huxerui::IconButton(
        app::images::trash, Localized("关闭全部"))
        .With(huxerui::Tooltip(Localized("关闭全部连接")))
        .OnClick([tasks] {
            tasks.Launch([=]() -> huxerui::Task<void> {
                co_await RunOnTaskThread([] {
                    closeAllConnectionsForPlatform();
                });
            });
        });
    huxerui::View actions = std::move(closeAllButton);
    body = huxerui::View{std::move(body)}.With(huxerui::Padding(
        huxerui::EdgeInsets::Symmetric(sectionInsets.horizontal * 0.5F, 0.0F)));
    return onBack
        ? SecondaryPageScaffold(std::move(searchField), std::move(actions),
                                std::move(body), onBack, false, true)
        : PageScaffold(Localized("连接"), std::move(actions), std::move(body),
                       false, true, true, std::nullopt, {},
                       std::move(searchField));
}

} // namespace clashflux::ui
