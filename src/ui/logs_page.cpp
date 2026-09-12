// logs_page.cpp — 日志页：WS /logs 推送 + 内核进程 stdout/stderr 兜底行，
// 级别过滤（全部/信息/警告/错误/调试）+ 清空 + 自动滚底。
//
// 数据流：UI 泵每 250ms drain 流层日志队列与内核进程输出队列，拼上时间戳后
// append 进 StateList（上限 800 行防爆内存），有新行时
// ScrollController::ScrollToItem 滚底。
#include <huxerui/huxerui.h>

#include <chrono>
#include <string>
#include <vector>

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

[[huxerui::composable]] huxerui::View LogsPage() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto entries = huxerui::UseStateList<LogEntry>();
    auto filter = huxerui::UseState<std::size_t>(0);
    auto clearTick = huxerui::UseState(0);
    const auto scroll = huxerui::UseScrollController();

    huxerui::Lifecycle(
        [tasks, entries, clearTick] {
            tasks.Launch([=]() -> huxerui::Task<void> {
                int lastClear = clearTick.Get();
                co_await PollWhile(std::chrono::duration<double>{0.25}, [=]() mutable {
                    if (clearTick.Get() != lastClear) {
                        lastClear = clearTick.Get();
                        entries.Clear();
                        return true;
                    }
                    auto& core = store::coreStore();
                    auto batch = core.streams().drainLogs();
                    bool changed = false;
                    if (!batch.empty()) {
                        for (const auto& l : batch) {
                            entries.PushBack(LogEntry{
                                .text = std::format("[{}] {}", formatClock(l.at),
                                                    l.payload),
                                .level = levelRank(l.level),
                            });
                        }
                        changed = true;
                    }
                    // WS 断线时的兜底：内核 stdout/stderr 行（启动期日志）。
                    auto raw = core.process().drainOutput();
                    if (!raw.empty()) {
                        for (auto& l : raw) {
                            entries.PushBack(LogEntry{.text = std::move(l), .level = 1});
                        }
                        changed = true;
                    }
                    if (changed) {
                        while (entries.Size() > kMaxLines) {
                            entries.Erase(0);
                        }
                    }
                    return true;
                });
            });
            return [] {};
        },
        0);

    // 过滤后的索引视图。
    std::vector<std::size_t> visible;
    for (std::size_t i = 0; i < entries.Size(); ++i) {
        if (filter.Get() == 0 ||
            entries[i].level == static_cast<int>(filter.Get())) {
            visible.push_back(i);
        }
    }

    huxerui::View body = huxerui::Column {
        huxerui::Text("暂无日志（内核未运行？）")
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
                       return huxerui::Text(text)
                           .Style(huxerui::TextStyle{
                               huxerui::Font::Monospace(font_size::kMonoBody),
                               theme.colors.on_surface})
                           .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(
                                     4.0F, 1.0F)))
                           .Key(static_cast<std::int64_t>(sourceIndex));
                   })
                   .EstimatedItemExtent(22.0F)
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
                                    .With(huxerui::Frame{.width = 180.0F});
    huxerui::View clearControl = huxerui::Button("清空").OnClick([clearTick] {
        clearTick = clearTick.Get() + 1;
    });
    huxerui::View actions;
    if (compact) {
        actions = huxerui::Column {
            std::move(filterControl),
            std::move(clearControl),
        }.With(huxerui::Spacing(8.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Start));
    } else {
        actions = huxerui::Row {
            std::move(filterControl),
            std::move(clearControl),
        }.With(huxerui::Spacing(8.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    }

    return PageScaffold("日志", std::move(actions), std::move(body));
}

} // namespace clashflux::ui
