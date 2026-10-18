// logs_page.cpp — 统一日志页：内核 WS、stdout/stderr 与应用诊断共用时间线。
// 历史与推送增量按时间合并，保留最新 800 行；共用搜索、级别过滤、导出与清空。
#include <huxerui/huxerui.h>

#include <algorithm>
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

// 行首等级标签：rank 反解回等级名（levelRank 的逆映射，未知/silent 归 info），
// 按等级着色——info 灰、warning 警示色、error 错误色、debug 更弱。
std::string_view levelName(int rank) {
    switch (rank) {
    case 2: return "warning";
    case 3: return "error";
    case 4: return "debug";
    default: return "info";
    }
}

huxerui::Color levelColor(const huxerui::ThemeSpec& theme, int rank) {
    switch (rank) {
    case 3: return theme.colors.error;
    case 2: return SemanticWarningColor(theme);
    case 4: return theme.colors.outline;
    default: return theme.colors.on_surface_variant;
    }
}

// 终端式填充：URL、selector 名、哈希这类超长 token 在 UAX-14 里没有断行
// 机会，词法换行会把整块挪到下一行、上一行提前留白。仅在渲染层给 token
// 内补零宽空格断点（分隔符后至少 6 字符，纯哈希每 24 字符强制一个），
// 导出/复制仍消费原始文本。
std::string BreakableLogText(std::string_view text) {
    constexpr std::string_view kBreakAfter = "/\\:;,=])}>|#?!&%~-_.@";
    std::string out;
    out.reserve(text.size() + 16);
    std::size_t sinceBreak = 0;
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        std::size_t length = 1;
        if (lead >= 0xF0) length = 4;
        else if (lead >= 0xE0) length = 3;
        else if (lead >= 0xC0) length = 2;
        length = std::min(length, text.size() - i);
        out.append(text.substr(i, length));
        const char c = text[i];
        i += length;
        if (c == ' ' || c == '\t' || length > 1) {
            sinceBreak = 0;
            continue;
        }
        ++sinceBreak;
        const bool candidate = kBreakAfter.find(c) != std::string_view::npos;
        if (sinceBreak >= 24 || (candidate && sinceBreak >= 6)) {
            out.append("\xE2\x80\x8B");
            sinceBreak = 0;
        }
    }
    return out;
}

} // namespace

[[huxerui::composable]] huxerui::View LogsPage(std::function<void()> onBack, bool active) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto clipboard = huxerui::UseApplication().Clipboard();
    auto picker = huxerui::UseService<huxerui::FilePicker>();
    auto exporting = huxerui::UseState(false);
    auto entries = huxerui::UseStateList<LogEntry>();
    auto filter = huxerui::UseState<std::size_t>(0);
    auto logSearch = huxerui::UseState(huxerui::TextEditingValue{});
    const auto scroll = huxerui::UseScrollController();
    // 水平留白与 SectionTabPages 同约定：骨架 fullWidthSections 后由页面根
    // 持有半份卡片间距，不再叠加旧的 24pt 桌面岛内边距。
    const auto sectionInsets = huxerui::UseEnvironment<SectionTabContentInsets>();

    huxerui::Lifecycle(
        [tasks, entries] {
            const auto append = [entries](const stream::LogLine& line) {
                AppendLogEntry(entries, LogEntry{
                    .text = std::format("[{}] {}", formatClock(line.at), line.payload),
                    .level = levelRank(line.level),
                    .at = line.at,
                }, kMaxLines);
            };
            // 历史读取同时消费 pending，避免首个推送重复显示旧行。
            for (const auto& line : stream::logHistory()) append(line);
            const std::uint64_t subscription = SubscribeStreamUpdates(
                tasks, [entries, append](stream::StreamKind kind) {
                    if (kind != stream::StreamKind::Logs) return;
                    for (const auto& line : stream::drainLogs()) append(line);
                    // stdout/stderr 无结构化时间，按本次接收时间插入统一列表。
                    for (auto& line : store::coreStore().process().drainOutput()) {
                        AppendLogEntry(entries, LogEntry{
                            .text = std::move(line), .level = 1, .at = nowUnix(),
                        }, kMaxLines);
                    }
                });
            return [subscription] { UnsubscribeStreamUpdates(subscription); };
        },
        0);

    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    if (!active) return huxerui::View{huxerui::Row{}}.Key("logs-idle");

    const std::string query = logSearch.Get().text;
    // 展示顺序最新在顶：内部仍按时间升序存储（AppendLogEntry 二分插入、
    // 800 行环形淘汰、导出/复制保持正序），仅显示时倒序遍历——打开页面
    // 第一眼就是刚发生的日志，新行到达时出现在顶部，无需滚动跟随。
    std::vector<std::size_t> visible;
    for (std::size_t i = entries.Size(); i-- > 0;) {
        if ((filter.Get() == 0 || entries[i].level == static_cast<int>(filter.Get())) &&
            SearchTextMatches(entries[i].text, query)) {
            visible.push_back(i);
        }
    }
    huxerui::View body = EmptyState(
        Localized(entries.Empty() ? "暂无日志" : "没有匹配的日志"), app::images::logs);
    if (!visible.empty()) {
        const std::size_t visibleCount = visible.size();
        body = huxerui::VirtualList(
            visibleCount + (compact ? 1U : 0U),
            [entries, visible, theme, compact, visibleCount](std::size_t index) -> huxerui::View {
                if (compact && index == visibleCount) {
                    return CompactFloatingNavigationFooter().Key("compact-floating-footer");
                }
                const std::size_t sourceIndex = visible[index];
                const LogEntry& entry = entries[sourceIndex];
                // 等级/时间戳/消息融为单一段落（text 已含时间戳）：换行在整段
                // 内自然流动，不存在固定标签列与消息列各自的断行错位。等级
                // 前缀按等级着色，消息体保持常规前景色。
                const auto attributed = huxerui::AttributedText{
                    huxerui::TextSpan(std::string(levelName(entry.level)) + " ")
                        .Style(huxerui::TextSpanStyle{
                            .foreground = levelColor(theme, entry.level)}),
                    huxerui::TextSpan(BreakableLogText(entry.text)),
                };
                // 每条日志一张密集卡片：dense 圆角 + 减半内边距，底部外边距
                // 提供行间距（VirtualList 无行间距修饰符），与 4pt 档圆角同阶。
                return DenseCard(huxerui::Text(attributed)
                                     .Style(huxerui::TextStyle{
                                         huxerui::Font::Monospace(font_size::kMonoBody),
                                         theme.colors.on_surface}))
                    .With(huxerui::Padding(huxerui::EdgeInsets{.bottom = 4.0F}))
                    .Key(std::format("log-{}", sourceIndex));
            })
            // 密集卡片行高估算：减半内边距（8×2）+ 单行文本 + 底部外边距。
            .EstimatedItemExtent(compact ? 44.0F : 42.0F)
            .Controller(scroll)
            .With(huxerui::Grow(1.0F), huxerui::ScrollBar());
    }
    const auto exportText = [filter, logSearch, entries] {
        std::vector<LogEntry> snapshot(entries.begin(), entries.end());
        return ExportLogText(snapshot, filter.Get(), logSearch.Get().text);
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
    const auto clear = [entries] {
        entries.Clear();
        // 丢弃已排队的旧行，避免清空后马上被补回来。
        store::coreStore().process().drainOutput();
        stream::clearLogs();
    };
    huxerui::View actions = LogActionsMenu(filter.Get(),
        [filter](std::size_t index) { filter = index; }, copy, exportFile, clear,
        picker->CanSaveFiles() && !exporting.Get());
    huxerui::View searchField = PillSearchField(
        logSearch, Localized("搜索日志"));
    body = huxerui::View{std::move(body)}.With(huxerui::Padding(
        huxerui::EdgeInsets::Symmetric(sectionInsets.horizontal * 0.5F, 0.0F)));
    if (onBack) return SecondaryPageScaffold(std::move(searchField), std::move(actions),
                                              std::move(body), onBack, false, true);
    return PageScaffold(Localized("日志"), std::move(actions), std::move(body),
                        false, true, true, std::nullopt, {},
                        std::move(searchField));
}

} // namespace clashflux::ui
