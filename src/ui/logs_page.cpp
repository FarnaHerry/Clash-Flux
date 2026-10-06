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
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "app_resources.h"
#include "ui.h"
#include "empty_state.h"
#include "search_text.h"
#include "log_actions.h"
#include "task_bridge.h"

import clashflux.config;
import clashflux.stream;
import clashflux.store.core;
import clashflux.utils;

// 用到 stream::StreamKind，必须在模块导入之后（同 profiles_cache.h 的约定）。
#include "stream_updates.h"

namespace clashflux::ui {
namespace {

constexpr std::size_t kMaxLines = 800;

// 过滤级别：0=全部 1=信息 2=警告 3=错误 4=调试。
// 日志来源分区：key 参与标签选中匹配，label 是展示文本（见 SectionTabBar）。
const std::vector<SectionTab> kSourceTabs{{"core", Localized("内核日志")},
                                          {"application", Localized("应用日志")}};

// 文件选择器只导出已准备好的本地文件。每次操作独占一个目录，
// 选择器结束或任务取消时由 RAII 清理，不能覆盖运行中的日志文件。
class LogExportFile {
public:
    static std::shared_ptr<LogExportFile> Prepare(const std::string& text) {
        auto file = std::make_shared<LogExportFile>();
        std::random_device random;
        for (int attempt = 0; attempt < 8; ++attempt) {
            const auto directory = cfg::dataDir() /
                std::format(".log-export-{}-{}", random(), random());
            std::error_code error;
            if (std::filesystem::create_directory(directory, error)) {
                file->directory_ = directory;
                file->path_ = directory / "logs.txt";
                std::ofstream output(file->path_, std::ios::binary | std::ios::trunc);
                output.write(text.data(), static_cast<std::streamsize>(text.size()));
                output.close();
                if (!output) throw std::runtime_error("Failed to prepare log export");
                return file;
            }
            if (error && error != std::errc::file_exists) break;
        }
        throw std::runtime_error("Failed to create log export directory");
    }
    ~LogExportFile() {
        if (directory_.empty()) return;
        std::error_code error;
        std::filesystem::remove(path_, error);
        std::filesystem::remove(directory_, error);
    }
    const std::filesystem::path& Path() const { return path_; }
private:
    std::filesystem::path directory_;
    std::filesystem::path path_;
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
    auto toast = huxerui::UseToast();
    auto clipboard = huxerui::UseApplication().Clipboard();
    auto picker = huxerui::UseService<huxerui::FilePicker>();
    auto exporting = huxerui::UseState(false);
    auto coreEntries = huxerui::UseStateList<LogEntry>();
    auto applicationEntries = huxerui::UseStateList<LogEntry>();
    auto source = huxerui::UseState<std::size_t>(0);
    auto sectionMotion = UseSectionTabMotion();
    auto filter = huxerui::UseState<std::size_t>(0);
    auto logSearch = huxerui::UseState(huxerui::TextEditingValue{});
    const auto coreScroll = huxerui::UseScrollController();
    const auto applicationScroll = huxerui::UseScrollController();

    huxerui::Lifecycle(
        [tasks, coreEntries, applicationEntries] {
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
            const std::uint64_t subscription = SubscribeStreamUpdates(
                tasks, [coreEntries, applicationEntries](stream::StreamKind kind) {
                    if (kind != stream::StreamKind::Logs) return;
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

    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    if (!active) return huxerui::View{huxerui::Row{}}.Key("logs-idle");

    std::vector<huxerui::View> sourcePages;
    for (std::size_t page = 0; page < kSourceTabs.size(); ++page) {
        // 每个来源保留自己的过滤列表与滚动连接。
        const auto scroll = page == 0 ? coreScroll : applicationScroll;
        const auto entries = page == 0 ? coreEntries : applicationEntries;
        const std::string query = logSearch.Get().text;
        std::vector<std::size_t> visible;
        for (std::size_t i = 0; i < entries.Size(); ++i) {
            if ((filter.Get() == 0 ||
                 entries[i].level == static_cast<int>(filter.Get())) &&
                SearchTextMatches(entries[i].text, query)) {
                visible.push_back(i);
            }
        }

        huxerui::View body = EmptyState(
            Localized(entries.Empty()
                ? (page == 0 ? "暂无内核日志" : "暂无应用日志")
                : "没有匹配的日志"), app::images::logs);

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
            body = std::move(list);
        }

        sourcePages.push_back(huxerui::Column {body}.With(
            huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
            .Key("log-source-" + kSourceTabs[page].key));
    }
    huxerui::View body = SectionTabPages(sourcePages, source.Get(),
        [source](std::size_t index) { source = index; }, sectionMotion);

    // 来源分区与代理页/规则页/订阅页共用同一下划线标签栏（SectionTabBar）。
    huxerui::View sourceTabs = SectionTabBar(
        kSourceTabs, source.Get() == 0 ? "core" : "application",
        [source](const std::string& key) {
            source = key == "application" ? 1 : 0;
        }, sectionMotion);
    const auto exportText = [source, filter, logSearch, coreEntries, applicationEntries] {
        const auto entries = source.Get() == 0 ? coreEntries : applicationEntries;
        std::vector<LogEntry> snapshot;
        snapshot.reserve(entries.Size());
        for (std::size_t index = 0; index < entries.Size(); ++index) snapshot.push_back(entries[index]);
        return ExportLogText(snapshot, filter.Get(), logSearch.Get().text);
    };
    const auto copy = [clipboard, toast, exportText] {
        toast.Show(Localized(clipboard->WriteText(exportText()) ? "已复制到剪贴板" : "复制失败"));
    };
    const std::string logFileType = huxerui::UseString(Localized("日志文件"));
    const auto exportFile = [tasks, toast, picker, exporting, source, exportText, logFileType] {
        if (exporting.Get()) return;
        exporting = true;
        const std::string text = exportText();
        const std::string name = source.Get() == 0 ? "clash-flux-core.log" : "clash-flux-application.log";
        tasks.Launch([=]() -> huxerui::Task<void> {
            try {
                const auto prepared = co_await RunOnTaskThread([text] { return LogExportFile::Prepare(text); });
                const bool saved = co_await picker->SaveFileAsync(
                    huxerui::File(prepared->Path().string()), huxerui::SaveFileOptions{
                        .suggested_name = name,
                        .filter = {.name = logFileType, .extensions = {"log", "txt"},
                                   .content_types = {"text/plain"}},
                    });
                if (saved) toast.Show(Localized("日志已导出"));
            } catch (const std::exception& error) {
                toast.Show(LocalizedFormat("日志导出失败：{}", error.what()));
            }
            exporting = false;
        });
    };
    const auto clear = [coreEntries, applicationEntries] {
        coreEntries.Clear();
        applicationEntries.Clear();
        // 丢弃已排队的旧行，避免清空后马上被补回来。
        store::coreStore().process().drainOutput();
        stream::clearCoreLogs();
        stream::clearApplicationLogs();
    };
    huxerui::View actions = LogActionsMenu(filter.Get(),
        [filter](std::size_t index) { filter = index; }, copy, exportFile, clear,
        picker->CanSaveFiles() && !exporting.Get());
    body = huxerui::Column {
        std::move(sourceTabs),
        std::move(body),
    }.With(huxerui::Spacing(10.0F), huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    huxerui::View searchField = PillSearchField(
        logSearch, Localized("搜索日志"));
    if (onBack) return SecondaryPageScaffold(std::move(searchField), std::move(actions),
                                              std::move(body), onBack, false, true);
    return PageScaffold(Localized("日志"), std::move(actions), std::move(body),
                        false, true, true, std::nullopt, {},
                        std::move(searchField));
}

} // namespace clashflux::ui
