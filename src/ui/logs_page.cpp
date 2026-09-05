// logs_page.cpp — 日志页：WS /logs 推送 + 内核进程 stdout/stderr 兜底行，
// 级别过滤（全部/信息/警告/错误/调试）+ 清空 + 自动滚底。
//
// 数据流：UI 泵每 250ms drain 流层日志队列与内核进程输出队列，拼上时间戳后
// append 进 State<vector<string>>（上限 800 行防爆内存），有新行时
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
    auto lines = huxerui::UseState<std::vector<std::string>>({});
    auto levels = huxerui::UseState<std::vector<int>>({});  // 与 lines 平行的级别
    auto filter = huxerui::UseState<std::size_t>(0);
    auto clearTick = huxerui::UseState(0);
    const auto scroll = huxerui::UseScrollController();

    huxerui::Lifecycle(
        [tasks, lines, levels, clearTick] {
            tasks.Launch([=]() -> huxerui::Task<void> {
                int lastClear = clearTick.Get();
                co_await PollWhile(std::chrono::duration<double>{0.25}, [=]() mutable {
                    if (clearTick.Get() != lastClear) {
                        lastClear = clearTick.Get();
                        lines = {};
                        levels = {};
                        return true;
                    }
                    auto& core = store::coreStore();
                    auto batch = core.streams().drainLogs();
                    bool changed = false;
                    if (!batch.empty()) {
                        auto ls = lines.Get();
                        auto lv = levels.Get();
                        for (const auto& l : batch) {
                            ls.push_back(std::format("[{}] {}", formatClock(l.at),
                                                     l.payload));
                            lv.push_back(levelRank(l.level));
                        }
                        lines = std::move(ls);
                        levels = std::move(lv);
                        changed = true;
                    }
                    // WS 断线时的兜底：内核 stdout/stderr 行（启动期日志）。
                    auto raw = core.process().drainOutput();
                    if (!raw.empty()) {
                        auto ls = lines.Get();
                        auto lv = levels.Get();
                        for (auto& l : raw) {
                            ls.push_back(std::move(l));
                            lv.push_back(1);
                        }
                        lines = std::move(ls);
                        levels = std::move(lv);
                        changed = true;
                    }
                    if (changed && lines.Get().size() > kMaxLines) {
                        auto ls = lines.Get();
                        auto lv = levels.Get();
                        const std::size_t drop = ls.size() - kMaxLines;
                        ls.erase(ls.begin(), ls.begin() +
                                              static_cast<std::ptrdiff_t>(drop));
                        lv.erase(lv.begin(), lv.begin() +
                                              static_cast<std::ptrdiff_t>(drop));
                        lines = std::move(ls);
                        levels = std::move(lv);
                    }
                    return true;
                });
            });
            return [] {};
        },
        0);

    // 过滤后的索引视图。
    std::vector<std::size_t> visible;
    for (std::size_t i = 0; i < lines.Get().size(); ++i) {
        if (filter.Get() == 0 ||
            (i < levels.Get().size() && levels.Get()[i] ==
                                            static_cast<int>(filter.Get()))) {
            visible.push_back(i);
        }
    }

    huxerui::View body = huxerui::Column {
        huxerui::Text("暂无日志（内核未运行？）")
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                      theme.colors.on_surface_variant}),
    }.With(huxerui::Padding(32.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));

    if (!visible.empty()) {
        const auto allLines = lines.Get();
        body = huxerui::VirtualList(
                   visible,
                   [allLines, theme](std::size_t& idx) {
                       return huxerui::Text(idx < allLines.size() ? allLines[idx] : "")
                           .Style(huxerui::TextStyle{
                               huxerui::Font::Monospace(font_size::kMonoBody),
                               theme.colors.on_surface})
                           .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(
                                     4.0F, 1.0F)))
                           .Key(static_cast<std::int64_t>(idx));
                   })
                   .EstimatedItemExtent(22.0F)
                   .Controller(scroll)
                   .With(huxerui::Grow(1.0F), huxerui::ScrollBar());
    }

    return PageScaffold(
        "日志",
        huxerui::Row {
            huxerui::Select(
                kLevelNames, filter.Get(),
                [](const std::string& name) { return huxerui::Text(name); })
                .OnChanged([filter](std::size_t idx) { filter = idx; }),
            huxerui::Button("清空").OnClick([clearTick] {
                clearTick = clearTick.Get() + 1;
            }),
        }.With(huxerui::Spacing(8.0F)),
        std::move(body));
}

} // namespace clashflux::ui
