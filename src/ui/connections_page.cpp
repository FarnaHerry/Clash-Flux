// connections_page.cpp — 连接页：WS /connections 推送快照（约 1Hz 全量），
// 表头（总量 + 关闭全部）+ VirtualList 密集卡片（目标、实时/累计流量、链路/规则）。
//
// 数据流：IX 线程把每帧原文推进 CoreStreams 槽位并通知一次；UI 线程收到通知
// 启动任务线程读取与解析，回到 UI 线程发布 StateList；旧任务不能覆盖新快照。
#include <huxerui/huxerui.h>
#include <huxerui/charts.h>

#include <chrono>
#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <map>
#include <string>
#include <vector>

#include "app_resources.h"
#include "ui.h"
#include "connection_row.h"
#include "action_menu.h"
#include "empty_state.h"
#include "page_layout.h"
#include "page_loading.h"
#include "search_text.h"
#include "section_tab_picker.h"
#include "task_bridge.h"
#include "theme_colors.h"

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

struct ConnectionSample {
    std::int64_t up = 0;
    std::int64_t down = 0;
    std::chrono::steady_clock::time_point at;
};

using ConnectionSamples = std::map<std::string, ConnectionSample>;

void ApplyConnectionRates(ConnectionsSnapshot& snapshot, ConnectionSamples& previous,
                          std::chrono::steady_clock::time_point now) {
    ConnectionSamples next;
    for (auto& row : snapshot.rows) {
        const auto sample = previous.find(row.id);
        if (sample != previous.end()) {
            const double seconds = std::chrono::duration<double>(now - sample->second.at).count();
            if (seconds > 0.0) {
                if (row.up >= sample->second.up)
                    row.upRate = static_cast<std::int64_t>((row.up - sample->second.up) / seconds);
                if (row.down >= sample->second.down)
                    row.downRate = static_cast<std::int64_t>((row.down - sample->second.down) / seconds);
            }
        }
        if (!row.id.empty()) next[row.id] = {row.up, row.down, now};
    }
    previous = std::move(next);
}

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
        row.start = connection.start;
        row.inboundType = connection.inboundType;
        row.inbound = connection.inbound;
        row.destinationIP = connection.destinationIP;
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
        row.direct = connection.outboundType == "direct" ||
            (!connection.chains.empty() && connection.chains.front() == "DIRECT") ||
            connection.chain == "DIRECT";
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

std::string closeConnectionForPlatform(const std::string& id) {
#if defined(__ANDROID__)
    return clashflux_android_close_connection(id.c_str()) ? "" : "后台服务未响应";
#else
    const auto result = store::coreStore().api().closeConnection(id);
    return result.ok ? "" : result.error;
#endif
}

std::string closeAllConnectionsForPlatform() {
#if defined(__ANDROID__)
    return clashflux_android_close_all_connections() ? "" : "后台服务未响应";
#else
    const auto result = store::coreStore().api().closeAllConnections();
    return result.ok ? "" : result.error;
#endif
}

void ConfirmCloseConnection(huxerui::DialogHandle dialog, huxerui::TaskScope tasks,
                            FluxToastHandle toast, std::string id,
                            std::string host, bool all = false) {
    // 菜单项先结束弹出层；下一拍再打开模态层，避免同一事件路径互相抢焦点。
    tasks.Launch([dialog, tasks, toast, id = std::move(id), host = std::move(host), all]()
                     -> huxerui::Task<void> {
        co_await huxerui::Delay(std::chrono::duration<double>{0});
        dialog.Show([tasks, toast, id, host, all](huxerui::DialogContext context) {
            return DialogCard(huxerui::Column{
                huxerui::Text(Localized(all ? "关闭全部连接" : "关闭连接"),
                              huxerui::TextRole::Title),
                huxerui::Text(all
                    ? Localized("确定关闭全部活动连接吗？应用可能会重新建立连接。")
                    : LocalizedFormat("确定关闭“{}”吗？应用可能会重新建立连接。", host)),
                huxerui::Row{
                    huxerui::Button(Localized("取消"))
                        .OnClick([context] { context.Dismiss(); }),
                    huxerui::Button(Localized("关闭"))
                        .OnClick([context, tasks, toast, id, all] {
                            context.Dismiss();
                            tasks.Launch([toast, id, all]() -> huxerui::Task<void> {
                                try {
                                    const std::string error = co_await RunOnTaskThread([id, all] {
                                        return all ? closeAllConnectionsForPlatform()
                                                   : closeConnectionForPlatform(id);
                                    });
                                    if (!error.empty()) toast.Show(LocalizedFormat("关闭连接失败：{}", error));
                                } catch (const std::exception& error) {
                                    toast.Show(LocalizedFormat("关闭连接失败：{}", error.what()));
                                }
                            });
                        }),
                }.With(huxerui::Spacing(8.0F),
                       huxerui::MainAlign(huxerui::MainAxisAlignment::End)),
            }.With(huxerui::Spacing(12.0F), huxerui::Frame{.width = 320.0F},
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
        });
    });
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

// 桌面宽屏在连接页右侧抽屉打开详情（见文件末尾实现）；Android 与窄屏推入
// 二级页面，平台能力不随视口变化。
void OpenConnectionDetail(std::shared_ptr<SectionPickerLayers> layers, bool wide,
    huxerui::NavigationController navigation,
    std::shared_ptr<DesktopSettingsNavigationModel> desktopNavigation,
    ConnectionRow row);

} // namespace

[[huxerui::composable]] huxerui::View ConnectionListCard(
    ConnectionRow row, bool stacked, std::function<void()> openDetails,
    std::function<void()> confirmClose) {
    const auto menu = UseActionMenu();
    const auto more = huxerui::IconButton(
        huxerui::ImageResource{"app", "images/more_vertical"}, Localized("更多操作"))
        .With(menu.Anchor(), huxerui::Tooltip(Localized("更多操作")))
        .OnClick([menu, openDetails, confirmClose] {
            menu.Show({
                ActionMenuItem(huxerui::ImageResource{"app", "images/info"},
                               Localized("查看详情"), openDetails),
                ActionMenuSection{},
                ActionMenuItem(app::images::close, Localized("关闭连接"), confirmClose)
                    .Danger(),
            });
        });
    return DenseCard(ConnectionRowContent(
        row, formatRate(row.upRate), formatRate(row.downRate),
        formatBytes(row.up), formatBytes(row.down), stacked, more))
        .With(huxerui::Padding(huxerui::EdgeInsets{.bottom = 4.0F}));
}

[[huxerui::composable]] huxerui::View ConnectionsPage(
    std::function<void()> onBack, bool active) {
    const auto& theme = huxerui::UseTheme();
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    const bool stacked =
        huxerui::UseViewportClass() != huxerui::ViewportClass::Expanded;
    auto tasks = huxerui::UseTaskScope();
    const auto dialog = huxerui::UseDialog();
    const auto toast = UseFluxToast();
    const auto navigation = huxerui::UseNavigation();
    const auto desktopNavigation = huxerui::UseEnvironment<DesktopSettingsNavigation>().model;
    // 窗口层一次安装的层服务：宽屏从右侧抽屉打开连接详情（详见 OpenConnectionDetail）。
    const auto drawerLayers = huxerui::UseService<SectionPickerLayers>();
    auto rows = huxerui::UseStateList<ConnectionRow>();
    auto totalUp = huxerui::UseState<std::int64_t>(0);
    auto totalDown = huxerui::UseState<std::int64_t>(0);
    auto streamOpen = huxerui::UseState(false);
    auto loaded = huxerui::UseState(false);
    auto searchValue = huxerui::UseState(huxerui::TextEditingValue{""});
    const auto scroll = huxerui::UseScrollController();
    // 水平留白与 SectionTabPages 同约定：骨架 fullWidthSections 后由页面根
    // 持有半份卡片间距，不再叠加旧的 24pt 桌面岛内边距。
    const auto sectionInsets = huxerui::UseEnvironment<SectionTabContentInsets>();

    huxerui::Lifecycle(
        [tasks, rows, totalUp, totalDown, streamOpen, active, loaded] {
            // 帧原文比较用共享对象保存：本次激活内的首次读取与后续推送共用
            // 同一份；重新激活（依赖变化）重建后强制重新解析，拿到最新快照。
            auto lastFrame = std::make_shared<std::string>();
            auto samples = std::make_shared<ConnectionSamples>();
            auto generation = std::make_shared<std::uint64_t>(0);
            // 节拍泵的在位标志：依赖变化/卸载的 cleanup 置 false，让上一轮
            // 轮询在下一个节拍退出（TaskScope 活整个页面生命周期，不会随
            // Lifecycle 依赖变化取消已 Launch 的协程）。
            auto polling = std::make_shared<std::atomic<bool>>(true);
            const auto applyFrame = [tasks, rows, totalUp, totalDown, streamOpen,
                                     lastFrame, samples, generation, loaded] {
                const auto ticket = ++*generation;
                tasks.Launch([=]() -> huxerui::Task<void> {
                    try {
                        auto frame = co_await RunOnTaskThread(readConnectionFrame);
                        if (ticket != *generation) co_return;
                        streamOpen = frame.open;
                        if (frame.body.empty()) {
                            loaded = true;
                            co_return;
                        }
                        if (frame.body == *lastFrame) {
                            std::vector<ConnectionRow> idle;
                            idle.reserve(rows.Size());
                            bool hadRate = false;
                            for (const auto& current : rows) {
                                ConnectionRow row = current;
                                hadRate |= row.upRate != 0 || row.downRate != 0;
                                row.upRate = row.downRate = 0;
                                idle.push_back(std::move(row));
                            }
                            if (hadRate) ReplaceStateList(rows, std::move(idle));
                            co_return;
                        }
                        *lastFrame = frame.body;
                        auto snapshot = co_await RunOnTaskThread(
                            [body = std::move(frame.body)] { return parseConnections(body); });
                        if (ticket != *generation) co_return;
                        if (!snapshot) {
                            stream::logApplication("warning", "连接快照格式错误，保留最近有效快照");
                            loaded = true;
                            co_return;
                        }
                        ApplyConnectionRates(*snapshot, *samples,
                                             std::chrono::steady_clock::now());
                        totalUp = snapshot->totalUp;
                        totalDown = snapshot->totalDown;
                        ReplaceStateList(rows, std::move(snapshot->rows));
                        loaded = true;
                    } catch (const std::exception& error) {
                        stream::logApplication("error", std::string("读取连接快照失败：") + error.what());
                        if (ticket == *generation) loaded = true;
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

    if (kPageTitlesInWindow && !compact && !loaded.Get())
        return PageLoadingScaffold(Localized("连接"), theme, true);

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
                   [rows, visibleRows, tasks, dialog, toast, navigation,
                    desktopNavigation, drawerLayers, compact, stacked, rowCount](
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
                       return ConnectionListCard(
                           row, stacked,
                           [navigation, desktopNavigation, drawerLayers, compact, row] {
                               OpenConnectionDetail(drawerLayers, !compact, navigation,
                                                    desktopNavigation, row);
                           },
                           [dialog, tasks, toast, id, host = row.host] {
                               if (!id.empty())
                                   ConfirmCloseConnection(dialog, tasks, toast, id, host);
                           }).Key(key);
                   })
                   // 16pt 卡内留白 + 三行内容 + 4pt 卡间距。
                   .EstimatedItemExtent(90.0F)
                   .Controller(scroll)
                   .With(huxerui::Grow(1.0F), huxerui::ScrollBar());
    }

    huxerui::View searchField = PillSearchField(
        searchValue, Localized("搜索连接"));
    huxerui::View closeAllButton = huxerui::IconButton(
        app::images::trash, Localized("关闭全部"))
        .With(huxerui::Tooltip(Localized("关闭全部连接")))
        .OnClick([dialog, tasks, toast] {
            ConfirmCloseConnection(dialog, tasks, toast, {}, {}, true);
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

namespace {

using ConnectionRatePoint = std::pair<std::int64_t, std::int64_t>;

huxerui::XYChartData ConnectionChartData(
    const std::vector<ConnectionRatePoint>& history,
    std::string downloadLabel, std::string uploadLabel) {
    std::vector<huxerui::XYDatum> down;
    std::vector<huxerui::XYDatum> up;
    down.reserve(history.size());
    up.reserve(history.size());
    for (std::size_t index = 0; index < history.size(); ++index) {
        const auto key = std::to_string(index);
        const double x = static_cast<double>(index);
        up.push_back({key, x, static_cast<double>(history[index].first)});
        down.push_back({key, x, static_cast<double>(history[index].second)});
    }
    return huxerui::XYChartData({
        huxerui::XYSeries("down", std::move(downloadLabel), std::move(down)),
        huxerui::XYSeries("up", std::move(uploadLabel), std::move(up)),
    });
}

} // namespace

// 连接详情内容：轮询当前连接并渲染状态/速率图表/元数据岛。二级页面与桌面宽屏
// 侧抽屉共用；抽屉模式把「关闭连接」按钮放进内容末尾（页面模式动作在标题栏位）。
[[huxerui::composable]] huxerui::View ConnectionDetailBody(
    huxerui::State<ConnectionRow> current, huxerui::State<bool> ended,
    huxerui::State<std::vector<ConnectionRatePoint>> history,
    ConnectionRow initial, bool withCloseAction) {
    const auto& theme = huxerui::UseTheme();
    const auto tasks = huxerui::UseTaskScope();
    const auto dialog = huxerui::UseDialog();
    const auto toast = UseFluxToast();

    huxerui::Lifecycle([tasks, current, ended, history, initial] {
        auto generation = std::make_shared<std::uint64_t>(0);
        auto polling = std::make_shared<std::atomic<bool>>(true);
        auto samples = std::make_shared<ConnectionSamples>();
        auto seenOpen = std::make_shared<bool>(false);
        const auto applyFrame = [tasks, current, ended, history, initial,
                                 generation, samples, seenOpen] {
            if (ended.Get()) return;
            const auto ticket = ++*generation;
            tasks.Launch([=]() -> huxerui::Task<void> {
                try {
                    auto frame = co_await RunOnTaskThread(readConnectionFrame);
                    if (ticket != *generation) co_return;
                    if (!frame.open) {
                        if (!*seenOpen) co_return;
                        ConnectionRow finalRow = current.Get();
                        finalRow.upRate = finalRow.downRate = 0;
                        current = std::move(finalRow);
                        ended = true;
                        co_return;
                    }
                    *seenOpen = true;
                    if (frame.body.empty()) co_return;
                    auto snapshot = co_await RunOnTaskThread(
                        [body = std::move(frame.body)] { return parseConnections(body); });
                    if (ticket != *generation || !snapshot) co_return;
                    ApplyConnectionRates(*snapshot, *samples,
                                         std::chrono::steady_clock::now());
                    const auto match = std::find_if(snapshot->rows.begin(),
                        snapshot->rows.end(), [&initial](const ConnectionRow& row) {
                            return !initial.id.empty() && row.id == initial.id;
                        });
                    if (match == snapshot->rows.end()) {
                        ConnectionRow finalRow = current.Get();
                        finalRow.upRate = finalRow.downRate = 0;
                        current = std::move(finalRow);
                        ended = true;
                        co_return;
                    }
                    current = *match;
                    auto points = history.Get();
                    points.emplace_back(match->upRate, match->downRate);
                    if (points.size() > 60) points.erase(points.begin());
                    history = std::move(points);
                } catch (const std::exception& error) {
                    stream::logApplication("error",
                        std::string("读取连接详情失败：") + error.what());
                }
            });
        };
        applyFrame();
        const auto subscription = SubscribeStreamUpdates(tasks,
            [applyFrame](stream::StreamKind kind) {
                if (kind == stream::StreamKind::Connections) applyFrame();
            });
        StartSnapshotPoll(tasks, true, polling, applyFrame);
        return [subscription, polling] {
            UnsubscribeStreamUpdates(subscription);
            polling->store(false);
        };
    }, initial.id);

    const ConnectionRow row = current.Get();
    const auto label = [&theme](huxerui::StringVariant title, std::string value) {
        return huxerui::Row{
            huxerui::Text(std::move(title)).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption), theme.colors.on_surface_variant})
                .With(huxerui::Frame{.width = 82.0F}),
            huxerui::Text(std::move(value)).With(huxerui::Grow(1.0F)),
        }.With(huxerui::Spacing(8.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Start));
    };
    const auto metric = [&theme](std::string direction, std::string rate,
                                 std::string total) {
        return huxerui::Row{
            huxerui::Text(direction).With(huxerui::Frame{.width = 24.0F}),
            huxerui::Text(std::move(rate)).Style(huxerui::TextStyle{
                huxerui::Font::Monospace(font_size::kMonoBody), theme.colors.on_surface})
                .With(huxerui::Frame{.width = 130.0F}),
            huxerui::Text(direction + " " + std::move(total)).Style(huxerui::TextStyle{
                huxerui::Font::Monospace(font_size::kMonoBody), theme.colors.on_surface_variant}),
        }.With(huxerui::Spacing(8.0F));
    };
    huxerui::Color fillDown = theme.colors.primary;
    fillDown.alpha = 0.18F;
    const auto upColor = SemanticWarningColor(theme);
    const auto downColor = theme.colors.primary;
    const auto points = history.Get();
    huxerui::View chart = huxerui::AreaChart(ConnectionChartData(
        points, huxerui::UseString(Localized("↓ 下载")),
        huxerui::UseString(Localized("↑ 上传"))))
        .Baseline(0.0)
        .Interpolation(huxerui::ChartInterpolation::Linear)
        .XAxis(huxerui::ChartAxis::Linear().Visible(false))
        .YAxis(huxerui::ChartAxis::Linear().IncludeZero().Visible(false)
            .Formatter([](double value) {
                return formatRate(static_cast<std::int64_t>(value));
            }))
        .Legend(huxerui::ChartLegendOptions{.visible = false})
        .Accessibility(huxerui::ChartAccessibilityOptions{
            .summary = huxerui::UseString(Localized("上/下行实时速率曲线"))})
        .SeriesStyle("down", huxerui::AreaChartStyle{
            .fill = huxerui::Brush(fillDown),
            .stroke = huxerui::Brush(downColor),
            .stroke_style = huxerui::StrokeStyle{.width = 2.0F}})
        .SeriesStyle("up", huxerui::AreaChartStyle{
            .fill = huxerui::Brush(huxerui::Color::Transparent()),
            .stroke = huxerui::Brush(upColor),
            .stroke_style = huxerui::StrokeStyle{.width = 1.5F}})
        .With(huxerui::Frame{.height = 180.0F});

    huxerui::View sections = huxerui::Column{
        IslandSurface(huxerui::Column{
            huxerui::Text(row.network + "  " + row.host, huxerui::TextRole::Title),
            huxerui::Text(Localized(ended.Get() ? "已结束" : "活动中"))
                .With(huxerui::Foreground(ended.Get()
                    ? theme.colors.on_surface_variant : theme.colors.primary)),
        }.With(huxerui::Spacing(6.0F)), IslandLevel::Raised),
        IslandSurface(huxerui::Column{
            huxerui::Text(Localized("当前速率与累计流量"), huxerui::TextRole::Title),
            metric("↑", formatRate(row.upRate), formatBytes(row.up)),
            metric("↓", formatRate(row.downRate), formatBytes(row.down)),
        }.With(huxerui::Spacing(10.0F)), IslandLevel::Raised),
        IslandSurface(huxerui::Column{
            huxerui::Text(Localized("最近流量变化"), huxerui::TextRole::Title),
            chart,
        }.With(huxerui::Spacing(10.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
                      IslandLevel::Raised),
        IslandSurface(huxerui::Column{
            label(Localized("目标地址"), row.host),
            label(Localized("目标 IP"), row.destinationIP.empty() ? "—" : row.destinationIP),
            label(Localized("入站类型"), row.inboundType.empty() ? "—" : row.inboundType),
            label(Localized("入站标签"), row.inbound.empty() ? "—" : row.inbound),
            label(Localized("链路"), row.chains),
            label(Localized("规则类型"), ConnectionRuleTypes(row.rule)),
            label(Localized("规则"), row.rule),
            label(Localized("开始时间"), row.start.empty() ? "—" : row.start),
        }.With(huxerui::Spacing(10.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
                      IslandLevel::Raised),
    }.With(huxerui::Spacing(12.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    if (withCloseAction) {
        sections = huxerui::Column{std::move(sections),
            huxerui::Button(Localized("关闭连接"))
                .With(huxerui::Enabled(!ended.Get()))
                .OnClick([dialog, tasks, toast, id = row.id, host = row.host] {
                    if (!id.empty())
                        ConfirmCloseConnection(dialog, tasks, toast, id, host);
                }),
        }.With(huxerui::Spacing(12.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }
    return huxerui::ScrollView(std::move(sections))
        .With(huxerui::Grow(1.0F), huxerui::ScrollBar());
}

[[huxerui::composable]] huxerui::View ConnectionDetailPage(
    ConnectionRow initial, std::function<void()> onBack, bool windowTitle) {
    auto current = huxerui::UseState(initial);
    auto ended = huxerui::UseState(false);
    auto history = huxerui::UseState(std::vector<ConnectionRatePoint>{});
    huxerui::View body = ConnectionDetailBody(current, ended, history, initial, false);
    const auto dialog = huxerui::UseDialog();
    const auto toast = UseFluxToast();
    const auto tasks = huxerui::UseTaskScope();
    const ConnectionRow row = current.Get();
    const auto confirmClose = [dialog, tasks, toast, id = row.id, host = row.host] {
        if (!id.empty()) ConfirmCloseConnection(dialog, tasks, toast, id, host);
    };
    huxerui::View closeAction = huxerui::Button(Localized("关闭连接"))
        .With(huxerui::Enabled(!ended.Get()))
        .OnClick(confirmClose);
    if (windowTitle) {
        huxerui::View back = huxerui::IconButton(
            app::images::arrow_back, Localized("返回上一页"))
            .OnClick(onBack);
        return PageScaffold(Localized("连接详情"), closeAction, body,
                            true, false, true, std::nullopt, back)
            .On<huxerui::ViewEvents::BackRequested>(onBack);
    }
    return SecondaryPageScaffold(
        huxerui::Text(Localized("连接详情"), huxerui::TextRole::Title),
        closeAction, body, onBack);
}

namespace {

// ---- 桌面宽屏连接详情侧抽屉 -------------------------------------------------
// 与代理分组抽屉同一套层机制：透明点击收起区（不叠加遮罩色）、面板右滑入场、
// Esc/系统返回关闭；顶部从桌面标题栏下沿开始，标题栏不被抽屉覆盖。

class ConnectionDetailDrawerLayout final
    : public huxerui::Layout<ConnectionDetailDrawerLayout> {
public:
    using Layout::Layout;
    static huxerui::LayoutResult Measure(huxerui::LayoutContext& context,
        huxerui::ViewNode& node, huxerui::Constraints constraints) {
        const auto size = constraints.Constrain({constraints.max_width, constraints.max_height});
        const float width = std::min(400.0F, size.width);
        static_cast<void>(context.Measure(
            node.ChildAt(0), {size.width, size.width, size.height, size.height}));
        static_cast<void>(context.Measure(node.ChildAt(1), {width, width, size.height, size.height}));
        huxerui::LayoutResult result;
        result.SetSize(size);
        result.Place(node.ChildAt(0), {});
        result.Place(node.ChildAt(1), {size.width - width, 0.0F});
        return result;
    }
};

struct ConnectionDetailSheetSession {
    huxerui::LayerId id = 0;
    std::function<void()> close;
};

void ShowConnectionDetailDrawer(huxerui::LayerController layers,
    ConnectionRow initial, float topInset) {
    auto session = std::make_shared<ConnectionDetailSheetSession>();
    const auto requestClose = [session, layers] {
        if (session->close) session->close();
        else layers.Dismiss(session->id);
    };
    session->id = layers.Attach({
        .pointer_policy = huxerui::LayerPointerPolicy::Content,
        .trap_focus = true,
        .cancel_policy = huxerui::LayerCancelPolicy::Dismiss,
        .on_dismiss_request = requestClose,
    }, [=] {
        return huxerui::Scope([=] {
            const auto& theme = huxerui::UseTheme();
            const double duration = theme.motion.reduced_motion ? 0.0 : theme.motion.normal;
            auto shown = huxerui::UseState(theme.motion.reduced_motion);
            auto closing = huxerui::UseState(false);
            auto tasks = huxerui::UseTaskScope();
            const auto close = [shown, closing, tasks, layers, id = session->id, duration] {
                if (closing.Get()) return;
                closing = true;
                shown = false;
                tasks.Launch([layers, id, duration]() -> huxerui::Task<void> {
                    co_await huxerui::Delay(std::chrono::duration<double>{duration});
                    layers.Dismiss(id);
                });
            };
            huxerui::Lifecycle([session, close, tasks, shown, closing] {
                session->close = close;
                tasks.Launch([shown, closing]() -> huxerui::Task<void> {
                    co_await huxerui::Delay(std::chrono::duration<double>{0});
                    if (!closing.Get()) shown = true;
                });
                return [session] { session->close = {}; };
            }, 0);
            auto current = huxerui::UseState(initial);
            auto ended = huxerui::UseState(false);
            auto history = huxerui::UseState(std::vector<ConnectionRatePoint>{});
            huxerui::View header = huxerui::Row{
                huxerui::Text(Localized("连接详情"), huxerui::TextRole::Title),
                huxerui::Spacer(),
                huxerui::IconButton(huxerui::ImageResource{"app", "images/close"},
                                    Localized("关闭"))
                    .With(huxerui::Tooltip(Localized("关闭")))
                    .OnClick(close),
            }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
            auto panel = huxerui::Column{std::move(header),
                ConnectionDetailBody(current, ended, history, initial, true)}
                .With(huxerui::Padding(theme.spacing.medium),
                      huxerui::Spacing(theme.spacing.medium),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                      huxerui::Background(theme.colors.surface_container),
                      ResolveFluxShadows(theme).side_drawer,
                      huxerui::Enabled(!closing.Get()), huxerui::Indication{})
                .OnClick([] {})
                .With(huxerui::Offset(huxerui::AnimateTo(
                    huxerui::Point{shown.Get() ? 0.0F : 400.0F, 0.0F},
                    huxerui::TweenSpec{.duration = duration,
                                       .easing = huxerui::Easing::EaseOut})))
                .Key("connection-detail-sheet");
            return huxerui::Column{ConnectionDetailDrawerLayout{
                huxerui::Row{}.With(huxerui::Indication{})
                    .OnClick(close).Key("connection-detail-scrim"),
                std::move(panel),
            }.With(huxerui::Grow(1.0F), huxerui::ClipChildren())}
                .With(huxerui::Padding(huxerui::EdgeInsets{.top = topInset}),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
        });
    });
}

void OpenConnectionDetail(std::shared_ptr<SectionPickerLayers> layers, bool wide,
    huxerui::NavigationController navigation,
    std::shared_ptr<DesktopSettingsNavigationModel> desktopNavigation,
    ConnectionRow row) {
#if defined(__ANDROID__)
    static_cast<void>(layers);
    static_cast<void>(wide);
    OpenConnectionDetailPage(navigation, desktopNavigation, std::move(row));
#else
    if (wide && layers) {
        ShowConnectionDetailDrawer(layers->layers, std::move(row),
                                   kDesktopTitleBarHeight + 1.0F);
        return;
    }
    OpenConnectionDetailPage(navigation, desktopNavigation, std::move(row));
#endif
}

} // namespace

} // namespace clashflux::ui
