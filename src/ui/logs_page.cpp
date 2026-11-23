// logs_page.cpp — 统一日志页：内核 WS、stdout/stderr 与应用诊断共用时间线。
// 历史与推送增量按时间合并，保留最新 800 行；共用搜索、级别过滤、导出与清空。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "app_resources.h"
#include "ui.h"
#include "page_layout.h"
#include "empty_state.h"
#include "search_text.h"
#include "log_actions.h"
#include "log_row_view.h"
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
constexpr float kLogsCacheExtent = 180.0F;

// 过滤级别：0=全部 1=信息 2=警告 3=错误 4=调试。

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

struct LogFeedBuffer {
    std::vector<LogEntry> entries;
    std::uint64_t revision = 0;
    bool flushScheduled = false;

    std::uint64_t cachedRevision = 0;
    std::size_t cachedFilter = 0;
    std::string cachedQuery;
    std::shared_ptr<const std::vector<LogEntry>> cachedVisible;
};

} // namespace

[[huxerui::composable]] huxerui::View LogsPage(std::function<void()> onBack, bool active) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = UseFluxToast();
    auto clipboard = huxerui::UseApplication().Clipboard();
    auto picker = huxerui::UseService<huxerui::FilePicker>();
    auto exporting = huxerui::UseState(false);
    const auto feed = huxerui::UseState(std::make_shared<LogFeedBuffer>()).Get();
    auto revision = huxerui::UseState<std::uint64_t>(0);
    auto filter = huxerui::UseState<std::size_t>(0);
    const auto [logSearch, searchExpanded] = UsePageSearch(active);
    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    auto textWidthPx = huxerui::UseState<int>(0);
    auto postedWidthPx = huxerui::UseState(std::make_shared<int>(0)).Get();
    const auto scroll = huxerui::UseScrollController();

    huxerui::Lifecycle(
        [tasks, feed, revision] {
            const auto insertLine = [feed](const stream::LogLine& line) {
                InsertLogEntry(feed->entries, LogEntry{
                    .text = std::format("[{}] {}", formatClock(line.at), line.payload),
                    .level = levelRank(line.level),
                    .at = line.at,
                }, kMaxLines);
            };
            feed->entries.clear();
            feed->entries.reserve(kMaxLines);
            // 历史读取同时消费 pending，避免首个推送重复显示旧行。
            for (const auto& line : stream::logHistory()) insertLine(line);
            ++feed->revision;
            revision = feed->revision;

            const std::uint64_t subscription = SubscribeStreamUpdates(
                tasks, [tasks, feed, revision, insertLine](stream::StreamKind kind) {
                    if (kind != stream::StreamKind::Logs) return;
                    const bool wasEmpty = feed->entries.empty();
                    bool added = false;
                    for (const auto& line : stream::drainLogs()) {
                        insertLine(line);
                        added = true;
                    }
                    // stdout/stderr 无结构化时间，按本次接收时间插入统一列表。
                    for (auto& line : store::coreStore().process().drainOutput()) {
                        InsertLogEntry(feed->entries, LogEntry{
                            .text = std::move(line),
                            .level = 1,
                            .at = nowUnix(),
                        }, kMaxLines);
                        added = true;
                    }
                    if (!added) return;
                    ++feed->revision;
                    if (wasEmpty) {
                        revision = feed->revision;
                        return;
                    }
                    if (feed->flushScheduled) return;
                    feed->flushScheduled = true;
                    tasks.Launch([feed, revision]() -> huxerui::Task<void> {
                        co_await huxerui::Delay(std::chrono::milliseconds{120});
                        feed->flushScheduled = false;
                        if (revision.Get() != feed->revision) {
                            revision = feed->revision;
                        }
                    });
                });
            return [feed, subscription] {
                feed->flushScheduled = false;
                UnsubscribeStreamUpdates(subscription);
            };
        },
        0);

    if (!active) return huxerui::View{huxerui::Row{}}.Key("logs-idle");

    const std::uint64_t currentRevision = revision.Get();
    const std::size_t currentFilter = filter.Get();
    const std::string query = logSearch.Get().text;

    // 展示顺序最新在顶：内部仍按时间升序存储（InsertLogEntry 二分插入、
    // 800 行淘汰、导出/复制保持正序），仅显示时倒序生成不可变快照；
    // 无关重组（如搜索框焦点/窗口尺寸变化）直接复用同一份 shared_ptr。
    if (!feed->cachedVisible || feed->cachedRevision != currentRevision ||
        feed->cachedFilter != currentFilter || feed->cachedQuery != query) {
        auto nextVisible = std::make_shared<std::vector<LogEntry>>();
        nextVisible->reserve(feed->entries.size());
        for (std::size_t i = feed->entries.size(); i-- > 0;) {
            const LogEntry& entry = feed->entries[i];
            if ((currentFilter == 0 || entry.level == static_cast<int>(currentFilter)) &&
                SearchTextMatches(entry.text, query)) {
                nextVisible->push_back(entry);
            }
        }
        feed->cachedRevision = currentRevision;
        feed->cachedFilter = currentFilter;
        feed->cachedQuery = query;
        feed->cachedVisible = std::move(nextVisible);
    }

    const auto visible = feed->cachedVisible;
    huxerui::View body = EmptyState(
        Localized(feed->entries.empty() ? "暂无日志" : "没有匹配的日志"), app::images::logs);
    if (visible && !visible->empty()) {
        const std::size_t visibleCount = visible->size();
        const IslandTheme islands = ResolveIslandTheme(theme);
        const bool flat = huxerui::UseEnvironment<DialogSurfaceContext>().flat;
        const huxerui::Color cardSurface = flat ? huxerui::Color::Transparent() : islands.raised;
        const float halfPadding = islands.island_padding * 0.5F;
        const float denseRadius = islands.dense_radius;
        const float rowGap = PageScrollInset(theme, true);
        const huxerui::Font monoFont = huxerui::Font::Monospace(font_size::kMonoBody);
        const huxerui::Color bodyColor = theme.colors.on_surface;
        // 0 until a row reports its real text width; rows use framework Text
        // until then, so no paragraph is cached for a guessed width.
        const int currentWidthPx = textWidthPx.Get();
        huxerui::TextMeasurer* measurer = &huxerui::UseTextMeasurer();
        const float rawLineHeight = measurer->Metrics(monoFont).LineHeight();
        const float lineHeight = std::max(std::ceil(rawLineHeight),
                                          std::ceil(monoFont.Size() * 1.3F));

        auto insets = ScrollAreaInsets::Dense(compact && !onBack ? kCompactFloatingNavigationInset : 0.0F);
        insets.top_follows_horizontal = true;
        body = ScrollArea(PageVirtualList(
            visibleCount,
            [visible, visibleCount, theme, cardSurface, halfPadding, denseRadius,
             rowGap, monoFont, bodyColor, currentWidthPx, rawLineHeight, measurer,
             tasks, textWidthPx, postedWidthPx](std::size_t index) -> huxerui::View {
                if (index >= visibleCount) return huxerui::Row{};
                const LogEntry& entry = (*visible)[index];
                const huxerui::Color lvlColor = LogLevelColor(theme, entry.level);
                const float bottomGap = index + 1 == visibleCount ? 0.0F : rowGap;
                const ObserveLogTextWidth observeWidth{tasks, textWidthPx, postedWidthPx};

                // Framework-laid-out paragraph: the correct but slower path,
                // used before the width is known or if a layout is untrusted.
                const auto textContent = [&](int level, const std::string& text) {
                    return huxerui::View{huxerui::Text(LogAttributedText(level, text, lvlColor))
                        .Style(huxerui::TextStyle{monoFont, bodyColor})
                        .With(huxerui::Grow(1.0F), observeWidth)};
                };
                const auto paragraphContent = [&](std::shared_ptr<const LogParagraph> paragraph) {
                    // Read the height before moving the pointer into the modifier.
                    const float height = paragraph->height;
                    return huxerui::View{huxerui::Row{}
                        .With(huxerui::Grow(1.0F), huxerui::Frame{.height = height},
                              LogParagraphPaint{std::move(paragraph)}, observeWidth)};
                };
                const auto surface = [&](huxerui::View content) {
                    return huxerui::Row{std::move(content)}
                        .With(huxerui::Padding(halfPadding),
                              huxerui::Background(cardSurface),
                              huxerui::CornerRadius(denseRadius),
                              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
                };

                std::shared_ptr<const LogParagraph> paragraph =
                    FindLogParagraph(entry.id, currentWidthPx, monoFont.Size(),
                                     rawLineHeight, lvlColor, bodyColor);
                if (!paragraph && currentWidthPx > 0) {
                    paragraph = BuildLogParagraph(
                        *measurer, entry.id, entry.level, entry.text,
                        monoFont, rawLineHeight, lvlColor, bodyColor, currentWidthPx);
                }
                huxerui::View card = surface(
                    paragraph ? paragraphContent(std::move(paragraph))
                              : textContent(entry.level, entry.text));
                return huxerui::Column{std::move(card)}
                    .With(huxerui::Padding(huxerui::EdgeInsets{.bottom = bottomGap}),
                          huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                          huxerui::Semantics{
                              .label = std::string{LogLevelPrefix(entry.level)} + entry.text})
                    .Key(entry.id);
            })
            // 密集卡片行高估算：减半内边距 ×2 + 实测单行字高 + 底部外边距。
            .EstimatedItemExtent(2.0F * halfPadding + lineHeight + rowGap)
            .CacheExtent(kLogsCacheExtent)
            .Controller(scroll), scroll, {}, {}, insets)
                .With(huxerui::Grow(1.0F));
    }
    const auto exportText = [feed, filter, logSearch] {
        return ExportLogText(feed->entries, filter.Get(), logSearch.Get().text);
    };
    const auto copy = [clipboard, toast, exportText] {
        toast.Show(Localized(clipboard->WriteText(exportText()) ? "已复制到剪贴板" : "复制失败"));
    };
    const std::string logFileType = huxerui::UseString(Localized("日志文件"));
    const auto exportFile = [tasks, toast, picker, exporting, exportText, logFileType] {
        if (exporting.Get()) return;
        exporting = true;
        const std::string text = exportText();
        const std::string name = "clash-flux.log";
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
    const auto clear = [feed, revision] {
        feed->entries.clear();
        feed->cachedVisible.reset();
        // 丢弃已排队的旧行，避免清空后马上被补回来。
        store::coreStore().process().drainOutput();
        stream::clearLogs();
        ++feed->revision;
        revision = feed->revision;
    };
    huxerui::View actions = LogActionsMenu(filter.Get(),
        [filter](std::size_t index) { filter = index; }, copy, exportFile, clear,
        picker->CanSaveFiles() && !exporting.Get());
    const auto title = Localized("日志");
    huxerui::View searchToolbar = PageSearchToolbar(
        logSearch, searchExpanded, title, Localized("搜索日志"), actions);
    if (onBack) return SecondaryPageScaffold(PageTitleText(theme, title), {},
                                             std::move(body), onBack, false, true, false, true,
                                             std::nullopt, std::move(searchToolbar));
    return PageScaffold(title, {}, std::move(body),
                        false, true, true, std::nullopt, {}, std::move(searchToolbar));
}

} // namespace clashflux::ui
