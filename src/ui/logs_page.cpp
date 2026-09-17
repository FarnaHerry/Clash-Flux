// logs_page.cpp — 日志页：内核 /logs + stdout/stderr 兜底行，或应用自身诊断日志；
// 级别过滤（全部/信息/警告/错误/调试）+ 清空 + 自动滚底。
//
// 数据流：UI 泵每 250ms drain 对应日志队列，拼上时间戳后 append 进各自
// StateList（上限 800 行防爆内存）；两类日志分开缓存，切换来源不会丢数据。
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

namespace clashflux::ui {
namespace {

constexpr std::size_t kMaxLines = 800;

// 过滤级别：0=全部 1=信息 2=警告 3=错误 4=调试。
const std::vector<std::string> kLevelNames{"全部", "信息", "警告", "错误", "调试"};
const std::vector<huxerui::StringVariant> kSourceNames{"内核日志", "应用日志"};

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

[[huxerui::composable]] huxerui::View LogsPage(std::function<void()> onBack) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto coreEntries = huxerui::UseStateList<LogEntry>();
    auto applicationEntries = huxerui::UseStateList<LogEntry>();
    auto source = huxerui::UseState<std::size_t>(0);
    auto filter = huxerui::UseState<std::size_t>(0);
    auto clearTick = huxerui::UseState(0);
    const auto scroll = huxerui::UseScrollController();

    huxerui::Lifecycle(
        [tasks, coreEntries, applicationEntries, clearTick] {
            tasks.Launch([=]() -> huxerui::Task<void> {
                int lastClear = clearTick.Get();
                co_await PollWhile(std::chrono::duration<double>{0.25}, [=]() mutable {
                    if (clearTick.Get() != lastClear) {
                        lastClear = clearTick.Get();
                        coreEntries.Clear();
                        applicationEntries.Clear();
                        auto& core = store::coreStore();
                        // 丢弃已经进入队列但尚未绘制的旧日志，保证“清空”
                        // 不会在下一拍又把旧内容补回来。
                        core.streams().drainLogs();
                        core.process().drainOutput();
                        stream::drainApplicationLogs();
                        return true;
                    }
                    auto& core = store::coreStore();
                    auto batch = core.streams().drainLogs();
                    if (!batch.empty()) {
                        for (const auto& l : batch) {
                            coreEntries.PushBack(LogEntry{
                                .text = std::format("[{}] {}", formatClock(l.at),
                                                    l.payload),
                                .level = levelRank(l.level),
                            });
                        }
                    }
                    // WS 断线时的兜底：内核 stdout/stderr 行（启动期日志）。
                    auto raw = core.process().drainOutput();
                    if (!raw.empty()) {
                        for (auto& l : raw) {
                            coreEntries.PushBack(
                                LogEntry{.text = std::move(l), .level = 1});
                        }
                    }
                    for (const auto& l : stream::drainApplicationLogs()) {
                        applicationEntries.PushBack(LogEntry{
                            .text = std::format("[{}] {}", formatClock(l.at),
                                                l.payload),
                            .level = levelRank(l.level),
                        });
                    }
                    while (coreEntries.Size() > kMaxLines) {
                        coreEntries.Erase(0);
                    }
                    while (applicationEntries.Size() > kMaxLines) {
                        applicationEntries.Erase(0);
                    }
                    return true;
                });
            });
            return [] {};
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
        huxerui::Text(source.Get() == 0 ? "暂无内核日志" : "暂无应用日志")
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                      theme.colors.on_surface_variant}),
    }.With(huxerui::Padding(32.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));

    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    if (!visible.empty()) {
        const std::size_t visibleCount = visible.size();
        body = huxerui::VirtualList(
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
                           theme, std::format("log-{}", sourceIndex), compact);
                   })
                   .EstimatedItemExtent(compact ? 38.0F : 34.0F)
                   .Controller(scroll)
                   .With(huxerui::Grow(1.0F), huxerui::ScrollBar());
    }

    huxerui::View filterControl = huxerui::Select(
                                    kLevelNames, filter.Get(),
                                    [](const std::string& name) {
                                        return huxerui::Text(name);
                                    })
                                    .OnChanged([filter](std::size_t idx) {
                                        filter = idx;
                                    })
                                    .With(huxerui::Frame{.width = 112.0F});
    huxerui::View sourceControl =
        huxerui::SegmentedButton(kSourceNames, source.Get())
            .OnChanged([source](std::size_t idx) { source = idx; });
    huxerui::View clearControl = huxerui::IconButton(app::images::clear_all, "清空日志")
        .With(huxerui::Tooltip("清空日志"))
        .OnClick([clearTick] {
        clearTick = clearTick.Get() + 1;
    });
    huxerui::View actions = huxerui::Row {
        std::move(filterControl), std::move(clearControl),
    }.With(huxerui::Spacing(6.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    body = huxerui::Column {
        std::move(sourceControl),
        std::move(body),
    }.With(huxerui::Spacing(10.0F), huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    if (onBack) return SecondaryPageScaffold(huxerui::Text("日志", huxerui::TextRole::Title), std::move(actions),
                                              std::move(body), onBack);
    return PageScaffold("日志", std::move(actions), std::move(body));
}

} // namespace clashflux::ui
