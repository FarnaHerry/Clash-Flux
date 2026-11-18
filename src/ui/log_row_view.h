#pragma once

#include <huxerui/huxerui.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "ui.h"

namespace clashflux::ui {

inline std::string_view LogLevelPrefix(int rank) noexcept {
    switch (rank) {
    case 2: return "warning ";
    case 3: return "error ";
    case 4: return "debug ";
    default: return "info ";
    }
}

inline huxerui::Color LogLevelColor(const huxerui::ThemeSpec& theme, int rank) noexcept {
    switch (rank) {
    case 3: return theme.colors.error;
    case 2: return SemanticWarningColor(theme);
    case 4: return theme.colors.outline;
    default: return theme.colors.on_surface_variant;
    }
}

// Terminal-style fill: URLs, selector names and hashes have no UAX-14 break
// opportunity, so word wrapping would move the whole token to the next line.
// Rendering adds zero-width spaces inside long tokens (after a delimiter once
// at least 6 characters passed, or every 24 characters); copy/export keep the
// original text.
inline std::string BreakableLogText(std::string_view text) {
    constexpr std::string_view kBreakAfter = "/\\:;,=])}>|#?!&%~-_.@";
    std::string out;
    out.reserve(text.size() + 16);
    std::size_t sinceBreak = 0;
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t length = 1;
        if (lead >= 0xF0) length = 4;
        else if (lead >= 0xE0) length = 3;
        else if (lead >= 0xC0) length = 2;
        length = std::min(length, text.size() - i);
        out.append(text.substr(i, length));
        const char c = text[i];
        i += length;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || length > 1) {
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

// One paragraph: the level prefix in its level color followed by the message.
inline huxerui::AttributedText LogAttributedText(int level, std::string_view text,
                                                 huxerui::Color levelColor) {
    return huxerui::AttributedText{
        huxerui::TextSpan(std::string(LogLevelPrefix(level)))
            .Style(huxerui::TextSpanStyle{.foreground = levelColor}),
        huxerui::TextSpan(BreakableLogText(text)),
    };
}

// A platform paragraph layout for one log entry at one content width. The
// framework owns wrapping, fallback fonts, line boxes and baselines, so the
// measured height always contains every painted line.
struct LogParagraph {
    huxerui::TextLayout layout;
    float height = 0.0F;
    huxerui::Color levelColor;
    huxerui::Color bodyColor;
};

struct LogParagraphKey {
    std::uint64_t entryId = 0;
    int widthPx = 0;
    float fontSize = 0.0F;
    float resolvedLineHeight = 0.0F;
    bool operator==(const LogParagraphKey&) const = default;
};

struct LogParagraphKeyHash {
    std::size_t operator()(const LogParagraphKey& key) const noexcept {
        std::size_t seed = std::hash<std::uint64_t>{}(key.entryId);
        seed ^= std::hash<int>{}(key.widthPx) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
        seed ^= std::hash<float>{}(key.fontSize) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
        seed ^= std::hash<float>{}(key.resolvedLineHeight) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
        return seed;
    }
};

// TextLayout must stay on its creating UI thread; the cache is thread-local.
inline auto& LogParagraphCache() {
    thread_local std::unordered_map<LogParagraphKey, std::shared_ptr<const LogParagraph>,
                                    LogParagraphKeyHash>
        cache;
    return cache;
}

inline std::shared_ptr<const LogParagraph> FindLogParagraph(
    std::uint64_t entryId, int widthPx, float fontSize, float resolvedLineHeight,
    huxerui::Color levelColor, huxerui::Color bodyColor) {
    if (entryId == 0 || widthPx <= 0) return nullptr;
    const auto& cache = LogParagraphCache();
    const auto found = cache.find(
        LogParagraphKey{entryId, widthPx, fontSize, resolvedLineHeight});
    if (found == cache.end() || found->second->levelColor != levelColor ||
        found->second->bodyColor != bodyColor) return nullptr;
    return found->second;
}

// Builds and caches a paragraph. Returns nullptr when the width is not yet
// known or the measurement is not trustworthy; callers then fall back to an
// ordinary Text view and nothing invalid is cached.
inline std::shared_ptr<const LogParagraph> BuildLogParagraph(
    huxerui::TextMeasurer& measurer, std::uint64_t entryId, int level, std::string_view text,
    const huxerui::Font& font, float resolvedLineHeight,
    huxerui::Color levelColor, huxerui::Color bodyColor, int widthPx) {
    if (entryId == 0 || widthPx < 40) return nullptr;
    try {
        auto layout = measurer.CreateTextLayout(LogAttributedText(level, text, levelColor),
            huxerui::TextStyle{font, bodyColor}, static_cast<float>(widthPx));
        const auto metrics = layout.Metrics();
        if (metrics.line_count == 0 || !std::isfinite(metrics.size.height) ||
            metrics.size.height < font.Size() * 0.8F) return nullptr;
        auto paragraph = std::make_shared<const LogParagraph>(LogParagraph{
            std::move(layout), std::ceil(metrics.size.height), levelColor, bodyColor});
        auto& cache = LogParagraphCache();
        if (cache.size() >= 2048) cache.clear();
        cache.insert_or_assign(
            LogParagraphKey{entryId, widthPx, font.Size(), resolvedLineHeight}, paragraph);
        return paragraph;
    } catch (...) {
        return nullptr;
    }
}

// Reports the real width of a row's text box. Every row carries it so the
// list self-corrects to the content width actually allotted by layout.
struct ObserveLogTextWidth {
    class Extension;
    huxerui::TaskScope tasks;
    huxerui::State<int> widthPx;
    std::shared_ptr<int> postedPx;
    bool operator==(const ObserveLogTextWidth& other) const {
        return widthPx == other.widthPx;
    }
};

class ObserveLogTextWidth::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode&, const ObserveLogTextWidth& value) : value_(value) {}
    void Update(huxerui::ViewNode&, const ObserveLogTextWidth& value) { value_ = value; }

    PaintInvalidation PrepareGeometry(huxerui::ViewNode& node, huxerui::TextMeasurer&) override {
        const float rawWidth = node.ContentBounds().width;
        const int nextPx = static_cast<int>(std::floor(rawWidth));
        if (nextPx >= 40 && nextPx != lastObservedPx_) {
            lastObservedPx_ = nextPx;
            if (value_.widthPx.Get() != nextPx &&
                (!value_.postedPx || *value_.postedPx != nextPx)) {
                if (value_.postedPx) *value_.postedPx = nextPx;
                auto state = value_.widthPx;
                value_.tasks.Post([state, nextPx] {
                    if (state.Get() != nextPx) state = nextPx;
                });
            }
        }
        return PaintInvalidation::None;
    }

private:
    ObserveLogTextWidth value_;
    int lastObservedPx_ = -1;
};

struct LogParagraphPaint {
    class Extension;
    std::shared_ptr<const LogParagraph> paragraph;
    bool operator==(const LogParagraphPaint&) const = default;
};

class LogParagraphPaint::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode&, const LogParagraphPaint& value) : value_(value) {}
    void Update(huxerui::ViewNode&, const LogParagraphPaint& value) {
        if (value_ == value) return;
        value_ = value;
        InvalidatePaint(PaintInvalidation::Foreground);
    }

    void PaintAboveContent(const huxerui::ViewNode& node, huxerui::PaintContext& paint) const override {
        if (!value_.paragraph) return;
        const auto bounds = node.ContentBounds();
        if (bounds.IsEmpty()) return;
        // Win32 headers included before this header alias DrawText to DrawTextA/W.
#if defined(DrawText)
#pragma push_macro("DrawText")
#undef DrawText
        paint.DrawText(huxerui::Point{bounds.x, bounds.y}, value_.paragraph->layout);
#pragma pop_macro("DrawText")
#else
        paint.DrawText(huxerui::Point{bounds.x, bounds.y}, value_.paragraph->layout);
#endif
    }

private:
    LogParagraphPaint value_;
};

} // namespace clashflux::ui
