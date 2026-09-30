// logs_page.cpp — 日志页：内核 /logs + stdout/stderr 兜底行，或应用自身诊断日志；
// 「内核日志/应用日志」来源分区走全项目统一的下划线标签栏（SectionTabBar，与
// 代理页分组、规则页「订阅规则/全局路由」同款）+ 左右滑动切换；级别过滤
// （全部/信息/警告/错误/调试）+ 清空 + 自动滚底。
//
// 数据流：推送驱动——内核 WS 日志 / 应用日志 / 内核 stdout 任一写入都会通知
// （同一路合并），UI 线程一次性 drain 增量、拼时间戳后 append 进各自 StateList
// （上限 800 行防爆内存）；两类日志分开缓存，切换来源不会丢数据。
#include <huxerui/huxerui.h>

#include <chrono>
#include <string>
#include <vector>

#include "app_resources.h"
#include "ui.h"
#include "task_bridge.h"

import clashflux.stream;
import clashflux.store.core;
import clashflux.utils;

// 用到 stream::StreamKind，必须在模块导入之后（同 profiles_cache.h 的约定）。
#include "stream_updates.h"

namespace clashflux::ui {
namespace {

constexpr std::size_t kMaxLines = 800;

// 过滤级别：0=全部 1=信息 2=警告 3=错误 4=调试。
const std::vector<std::string> kLevelNames{"全部", "信息", "警告", "错误", "调试"};
// 日志来源分区：key 参与标签选中匹配，label 是展示文本（见 SectionTabBar）。
const std::vector<SectionTab> kSourceTabs{{"core", Localized("内核日志")},
                                          {"application", Localized("应用日志")}};

struct LogEntry {
    std::string text;
    int level = 1;
};

int levelRank(const std::string& level) {
    if (level == "info") return 1;
    if (level == "warning") return 2;
    if (level == "error") return 3;
    if (level == "debug") return 4;
    return 1;
}

} // namespace

[[huxerui::composable]] huxerui::View LogsPage(std::function<void()> onBack, bool active) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto coreEntries = huxerui::UseStateList<LogEntry>();
    auto applicationEntries = huxerui::UseStateList<LogEntry>();
    auto source = huxerui::UseState<std::size_t>(0);
    auto filter = huxerui::UseState<std::size_t>(0);
    // 内核日志推送流是否就绪：断线（IX 会自动重连）时给出可见状态，而不是静默
    // 停更。
    auto streamReady = huxerui::UseState(false);
    // 来源分区滑动切换的手势状态（处理器与阈值见 common.cpp SectionTabSwipeHandler）。
    auto swipeOrigin =
        huxerui::UseState<huxerui::Point>(huxerui::Point{0.0F, 0.0F});
    auto swipeOwned = huxerui::UseState(false);
    const auto scroll = huxerui::UseScrollController();

    huxerui::Lifecycle(
        [tasks, coreEntries, applicationEntries, streamReady] {
            // 初始历史：页面重进时恢复已持久化的日志（同时消费掉 pending）。
            const auto coreHistory = stream::coreLogHistory();
            const std::size_t firstCore =
                coreHistory.size() > kMaxLines ? coreHistory.size() - kMaxLines : 0;
            for (std::size_t i = firstCore; i < coreHistory.size(); ++i) {
                const auto& line = coreHistory[i];
                coreEntries.PushBack(LogEntry{
                    .text = std::format("[{}] {}", formatClock(line.at),
                                        line.payload),
                    .level = levelRank(line.level),
                });
            }
            const auto applicationHistory = stream::applicationLogHistory();
            const std::size_t firstApplication =
                applicationHistory.size() > kMaxLines
                    ? applicationHistory.size() - kMaxLines
                    : 0;
            for (std::size_t i = firstApplication;
                 i < applicationHistory.size(); ++i) {
                const auto& line = applicationHistory[i];
                applicationEntries.PushBack(LogEntry{
                    .text = std::format("[{}] {}", formatClock(line.at),
                                        line.payload),
                    .level = levelRank(line.level),
                });
            }
            // 之后由推送驱动：内核 WS 日志、应用日志、内核 stdout/stderr 任一
            // 写入都会通知一次（同一路合并），这里一次性 drain 全部增量。
            streamReady = store::coreStore().streams().logsOpen();
            const std::uint64_t subscription = SubscribeStreamUpdates(
                tasks, [coreEntries, applicationEntries,
                        streamReady](stream::StreamKind kind) {
                    if (kind != stream::StreamKind::Logs) return;
                    streamReady = store::coreStore().streams().logsOpen();
                    for (const auto& l : stream::drainCoreLogs()) {
                        coreEntries.PushBack(LogEntry{
                            .text = std::format("[{}] {}", formatClock(l.at),
                                                l.payload),
                            .level = levelRank(l.level),
                        });
                    }
                    // WS 断线时的兜底：内核 stdout/stderr 行（启动期日志）。
                    for (auto& l : store::coreStore().process().drainOutput()) {
                        coreEntries.PushBack(
                            LogEntry{.text = std::move(l), .level = 1});
                    }
                    for (const auto& l : stream::drainApplicationLogs()) {
                        applicationEntries.PushBack(LogEntry{
                            .text = std::format("[{}] {}", formatClock(l.at),
                                                l.payload),
                            .level = levelRank(l.level),
                        });
                    }
                    while (coreEntries.Size() > kMaxLines) coreEntries.Erase(0);
                    while (applicationEntries.Size() > kMaxLines) {
                        applicationEntries.Erase(0);
                    }
                });
            return [subscription] { UnsubscribeStreamUpdates(subscription); };
        },
        0);

    // 过滤后的索引视图。
    const auto entries = source.Get() == 0 ? coreEntries : applicationEntries;
    std::vector<std::size_t> visible;
    for (std::size_t i = 0; i < entries.Size(); ++i) {
        if (filter.Get() == 0 ||
            entries[i].level == static_cast<int>(filter.Get())) {
            visible.push_back(i);
        }
    }

    huxerui::View body = huxerui::Column {
        huxerui::Text(Localized(source.Get() == 0 ? "暂无内核日志" : "暂无应用日志"))
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                      theme.colors.on_surface_variant}),
    }.With(huxerui::Padding(32.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));

    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;

    // 不可见时只保留本页 State/Lifecycle，不构建内容：桌面 IndexedPages 让七个
    // 一级页同帧参与测量，隐藏页（日志/连接有推送流更新）的重子树会拖慢每一次渲染。
    if (!active) return huxerui::View{huxerui::Row{}}.Key("logs-idle");

    if (!visible.empty()) {
        const std::size_t visibleCount = visible.size();
        huxerui::View list =
            huxerui::VirtualList(
                visibleCount + (compact ? 1U : 0U),
                [entries, visible, theme, compact, visibleCount](
                    std::size_t index) -> huxerui::View {
                    if (compact && index == visibleCount) {
                        return CompactFloatingNavigationFooter()
                            .Key("compact-floating-footer");
                    }
                    const std::size_t sourceIndex = visible[index];
                    const std::string& text = entries[sourceIndex].text;
                    return UnifiedListRow(
                        huxerui::Text(text).Style(huxerui::TextStyle{
                            huxerui::Font::Monospace(font_size::kMonoBody),
                            theme.colors.on_surface}),
                        std::format("log-{}", sourceIndex), compact,
                        index + 1 < visibleCount);
                })
                .EstimatedItemExtent(compact ? 38.0F : 34.0F)
                .Controller(scroll)
                .With(huxerui::Grow(1.0F), huxerui::ScrollBar());
        // 分区组件默认支持滑动（见 ui.h）：日志区左右滑动在内核/应用日志间切换。
        if (kSectionTabsSwipeDefault) {
            const bool onCore = source.Get() == 0;
            list = std::move(list).On<huxerui::ViewEvents::PointerIntercept>(
                SectionTabSwipeHandler(
                    swipeOrigin, swipeOwned,
                    onCore ? nullptr
                           : std::function<void()>{[source] { source = 0; }},
                    onCore ? std::function<void()>{[source] { source = 1; }}
                           : nullptr));
        }
        body = std::move(list);
    }

    huxerui::View filterControl = huxerui::Select(
                                    kLevelNames, filter.Get(),
                                    [](const std::string& name) {
                                        return huxerui::Text(Localized(name));
                                    })
                                    .OnChanged([filter](std::size_t idx) {
                                        filter = idx;
                                    })
                                    .With(huxerui::Frame{.width = 112.0F});
    // 来源分区与代理页/规则页/订阅页共用同一下划线标签栏（SectionTabBar）。
    huxerui::View sourceTabs = SectionTabBar(
        kSourceTabs, source.Get() == 0 ? "core" : "application",
        [source](const std::string& key) {
            source = key == "application" ? 1 : 0;
        });
    huxerui::View clearControl = huxerui::IconButton(app::images::clear_all, Localized("清空日志"))
        .With(huxerui::Tooltip(Localized("清空日志")))
        .OnClick([coreEntries, applicationEntries] {
            coreEntries.Clear();
            applicationEntries.Clear();
            // 丢弃已经进入队列但尚未绘制的旧日志，保证“清空”不会马上被补回来。
            store::coreStore().process().drainOutput();
            stream::clearCoreLogs();
            stream::clearApplicationLogs();
        });
    huxerui::View actions = huxerui::Row {
        std::move(filterControl),
        huxerui::Text(Localized(streamReady.Get() ? "推送流已连接"
                                                  : "推送流未连接（自动重试中）"))
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption),
                                      streamReady.Get()
                                          ? theme.colors.on_surface_variant
                                          : theme.colors.error}),
        std::move(clearControl),
    }.With(huxerui::Spacing(6.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    body = huxerui::Column {
        std::move(sourceTabs),
        std::move(body),
    }.With(huxerui::Spacing(10.0F), huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    if (onBack) return SecondaryPageScaffold(huxerui::Text(Localized("日志"), huxerui::TextRole::Title), std::move(actions),
                                              std::move(body), onBack);
    return PageScaffold(Localized("日志"), std::move(actions), std::move(body));
}

} // namespace clashflux::ui
