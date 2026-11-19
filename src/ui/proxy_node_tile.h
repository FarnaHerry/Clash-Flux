#pragma once

#include <huxerui/huxerui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ui.h"

namespace clashflux::ui {

inline constexpr float kProxyDelaySlotWidth = 62.0F;
inline constexpr float kProxyDelaySlotHeight = 18.0F;

class ProxyProbeDotsMetrics {
public:
    float Width() const {
        if (!width_) {
            const huxerui::TextStyle style{huxerui::Font::System(font_size::kCaption), huxerui::Color::Transparent()};
            width_ = huxerui::UseTextMeasurer().MeasureText("000", style, kProxyDelaySlotWidth).size.width;
        }
        return *width_;
    }

private:
    mutable std::optional<float> width_;
};

inline std::string BoundSingleLineRun(std::string_view input, std::size_t maxCodePoints = 64) {
    std::string out;
    out.reserve(std::min<std::size_t>(input.size(), maxCodePoints * 4 + 4));
    std::size_t count = 0;
    for (std::size_t i = 0; i < input.size();) {
        const auto lead = static_cast<unsigned char>(input[i]);
        if (lead == '\r' || lead == '\n' || lead == '\v' || lead == '\f') {
            if (count >= maxCodePoints) {
                out += "\xE2\x80\xA6";
                return out;
            }
            out.push_back(' ');
            ++i;
            ++count;
        } else if (lead == 0xC2U && i + 1 < input.size() &&
                   static_cast<unsigned char>(input[i + 1]) == 0x85U) {
            if (count >= maxCodePoints) {
                out += "\xE2\x80\xA6";
                return out;
            }
            out.push_back(' ');
            i += 2;
            ++count;
        } else if (lead == 0xE2U && i + 2 < input.size() &&
                   static_cast<unsigned char>(input[i + 1]) == 0x80U &&
                   (static_cast<unsigned char>(input[i + 2]) == 0xA8U ||
                    static_cast<unsigned char>(input[i + 2]) == 0xA9U)) {
            if (count >= maxCodePoints) {
                out += "\xE2\x80\xA6";
                return out;
            }
            out.push_back(' ');
            i += 3;
            ++count;
        } else {
            const std::size_t len = lead < 0x80U ? 1U
                : (lead & 0xE0U) == 0xC0U ? 2U
                : (lead & 0xF0U) == 0xE0U ? 3U
                : (lead & 0xF8U) == 0xF0U ? 4U : 1U;
            if (i + len > input.size()) break;
            if (count >= maxCodePoints) {
                out += "\xE2\x80\xA6";
                return out;
            }
            out.append(input.data() + i, len);
            i += len;
            ++count;
        }
    }
    return out;
}

// Single-line truncated text run backed by MeasureRun/DrawTextRun and a bounded
// per-thread elision cache, avoiding uncached DirectWrite paragraph binary search
// when long CJK node names mount or cross VirtualGrid CacheExtent boundaries.
struct SingleLineEllipsisLabel {
    class Extension;
    std::string text;
    huxerui::TextStyle style;
    bool operator==(const SingleLineEllipsisLabel&) const = default;
};

class SingleLineEllipsisLabel::Extension final : public huxerui::NodeExtension {
public:
    struct CacheKey {
        std::string text;
        float fontSize = 0.0F;
        huxerui::FontWeight weight = huxerui::FontWeight::Regular;
        int widthPx = 0;
        bool operator==(const CacheKey&) const = default;
    };
    struct CacheKeyHash {
        std::size_t operator()(const CacheKey& key) const noexcept {
            std::size_t seed = std::hash<std::string>{}(key.text);
            seed ^= std::hash<float>{}(key.fontSize) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
            seed ^= std::hash<int>{}(static_cast<int>(key.weight)) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
            seed ^= std::hash<int>{}(key.widthPx) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
            return seed;
        }
    };
    struct CacheEntry {
        std::string display;
        float visualY = 0.0F;
        float visualHeight = 0.0F;
    };

    Extension(huxerui::ViewNode& node, const SingleLineEllipsisLabel& value) { Update(node, value); }

    void Update(huxerui::ViewNode&, const SingleLineEllipsisLabel& value) {
        const bool geometrySame = value_.text == value.text &&
            value_.style.font == value.style.font;
        const bool colorChanged = value_.style.foreground != value.style.foreground ||
            value_.style.decoration != value.style.decoration;
        value_ = value;
        if (!geometrySame) {
            dirty_ = true;
            InvalidatePaint(PaintInvalidation::Foreground);
        } else if (colorChanged) {
            InvalidatePaint(PaintInvalidation::Foreground);
        }
    }

    PaintInvalidation PrepareGeometry(huxerui::ViewNode& node, huxerui::TextMeasurer& measurer) override {
        const float width = node.ContentBounds().width;
        const int widthKey = static_cast<int>(std::floor(std::max(0.0F, width)));
        if (!dirty_ && widthKey == widthKey_) {
            return PaintInvalidation::None;
        }
        dirty_ = false;
        widthKey_ = widthKey;
        if (value_.text.empty() || width <= 0.0F) {
            display_.clear();
            visualY_ = 0.0F;
            visualHeight_ = 0.0F;
            return PaintInvalidation::Foreground;
        }
        thread_local std::unordered_map<CacheKey, CacheEntry, CacheKeyHash> cache;
        const CacheKey key{value_.text, value_.style.font.Size(), value_.style.font.Weight(), widthKey};
        if (const auto found = cache.find(key); found != cache.end()) {
            display_ = found->second.display;
            visualY_ = found->second.visualY;
            visualHeight_ = found->second.visualHeight;
            return PaintInvalidation::Foreground;
        }

        const auto full = measurer.MeasureRun(value_.text, value_.style);
        if (full.advance <= width) {
            display_ = value_.text;
            visualY_ = full.visual_bounds.y;
            visualHeight_ = full.visual_bounds.height;
        } else {
            const auto ellipsis = measurer.MeasureRun("\xE2\x80\xA6", value_.style);
            if (ellipsis.advance > width) {
                display_.clear();
                visualY_ = 0.0F;
                visualHeight_ = 0.0F;
            } else {
                std::vector<std::size_t> ends{0};
                ends.reserve(66);
                for (std::size_t index = 1; index <= value_.text.size(); ++index) {
                    if (index == value_.text.size() ||
                        (static_cast<unsigned char>(value_.text[index]) & 0xC0U) != 0x80U) {
                        ends.push_back(index);
                    }
                }
                std::size_t low = 0;
                std::size_t high = ends.size();
                std::string bestDisplay = "\xE2\x80\xA6";
                float bestVisualY = ellipsis.visual_bounds.y;
                float bestVisualHeight = ellipsis.visual_bounds.height;
                while (low + 1 < high) {
                    const std::size_t middle = (low + high) / 2;
                    std::string candidate = value_.text.substr(0, ends[middle]) + "\xE2\x80\xA6";
                    const auto measured = measurer.MeasureRun(candidate, value_.style);
                    if (measured.advance <= width) {
                        low = middle;
                        bestDisplay = std::move(candidate);
                        bestVisualY = measured.visual_bounds.y;
                        bestVisualHeight = measured.visual_bounds.height;
                    } else {
                        high = middle;
                    }
                }
                display_ = std::move(bestDisplay);
                visualY_ = bestVisualY;
                visualHeight_ = bestVisualHeight;
            }
        }
        if (cache.size() >= 4096) cache.clear();
        cache.emplace(key, CacheEntry{display_, visualY_, visualHeight_});
        return PaintInvalidation::Foreground;
    }

    void PaintAboveContent(const huxerui::ViewNode& node, huxerui::PaintContext& paint) const override {
        const auto bounds = node.ContentBounds();
        if (display_.empty() || bounds.IsEmpty()) return;
        const float topOffset = std::max(0.0F, (bounds.height - visualHeight_) * 0.5F);
        paint.DrawTextRun(bounds, {bounds.x, bounds.y + topOffset - visualY_}, display_, value_.style);
    }

private:
    SingleLineEllipsisLabel value_;
    int widthKey_ = -1;
    bool dirty_ = true;
    std::string display_;
    float visualY_ = 0.0F;
    float visualHeight_ = 0.0F;
};

inline huxerui::View SingleLineEllipsisText(std::string_view text, huxerui::TextStyle style) {
    const auto fm = huxerui::UseTextMeasurer().Metrics(style.font);
    const float height = std::max(1.0F, std::ceil(fm.ascent + fm.descent + fm.leading));
    return huxerui::Row{}
        .With(huxerui::Frame{.height = height},
              SingleLineEllipsisLabel{BoundSingleLineRun(text), std::move(style)});
}

struct ProxyProbeDotsPaint {
    class Extension;
    huxerui::Color color;
    bool animated = false;
    bool operator==(const ProxyProbeDotsPaint&) const = default;
};

class ProxyProbeDotsPaint::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode&, const ProxyProbeDotsPaint& value) : value_(value) {}
    void Update(huxerui::ViewNode&, const ProxyProbeDotsPaint& value) {
        value_ = value;
        InvalidatePaint(PaintInvalidation::Foreground);
    }

    FrameResult OnFrame(huxerui::ViewNode&, const huxerui::FrameInfo& frame) override {
        if (!value_.animated || frame.reduced_motion) {
            if (startedAt_) {
                startedAt_.reset();
                elapsed_ = 0.0;
                InvalidatePaint(PaintInvalidation::Foreground);
            }
            return {};
        }
        const bool starting = !startedAt_;
        if (starting) {
            startedAt_ = frame.timestamp;
            fade_.Set(0.0F);
            fade_.AnimateTo(1.0F, huxerui::TweenSpec{0.2, huxerui::CubicBezierCurve{0.0F, 0.0F, 0.58F, 1.0F}});
            for (std::size_t i = 0; i < dots_.size(); ++i) {
                dots_[i].Set(0.0F);
                dots_[i].AnimateTo(1.0F,
                    huxerui::TweenSpec{0.45, huxerui::CubicBezierCurve{0.42F, 0.0F, 0.58F, 1.0F}},
                    huxerui::AnimationPlayback{.delay = kStagger * i,
                        .iterations = std::nullopt, .repeat_mode = huxerui::RepeatMode::Reverse});
            }
        }
        fade_.Advance(frame);
        for (auto& dot : dots_) dot.Advance(frame);
        const double elapsed = frame.timestamp - *startedAt_;
        if (starting || elapsed != elapsed_) {
            elapsed_ = elapsed;
            InvalidatePaint(PaintInvalidation::Foreground);
        }
        return {.wake_after = 1.0 / 60.0};
    }

    void PaintAboveContent(const huxerui::ViewNode& node, huxerui::PaintContext& paint) const override {
        const auto bounds = node.ContentBounds();
        const float scale = std::min(bounds.width / 33.0F, bounds.height / kProxyDelaySlotHeight);
        const float radius = 3.5F * scale;
        const float step = (bounds.width - 2.0F * radius) * 0.5F;
        const float opacity = startedAt_ ? fade_.Value() : 1.0F;
        for (std::size_t i = 0; i < dots_.size(); ++i) {
            const bool bouncing = startedAt_ && elapsed_ >= kStagger * i;
            const float progress = dots_[i].Value();
            const float offset = bouncing ? std::lerp(5.0F, -6.0F, progress) : 0.0F;
            auto color = value_.color;
            color.alpha *= opacity * (bouncing ? std::lerp(0.4F, 1.0F, progress) : 0.9F);
            paint.DrawCircle({bounds.x + radius + step * static_cast<float>(i),
                              bounds.y + bounds.height * 0.5F + (0.5F + offset) * scale},
                             radius, color);
        }
    }

private:
    static constexpr double kStagger = 0.15;
    ProxyProbeDotsPaint value_;
    std::array<huxerui::MotionController, 3> dots_;
    huxerui::MotionController fade_;
    std::optional<double> startedAt_;
    double elapsed_ = 0.0;
};

inline huxerui::View ProxyProbeDots(huxerui::Color color, float width) {
    return huxerui::Scope([color, width] {
        const bool foreground = huxerui::UseWindow().LifecycleState() != huxerui::WindowLifecycleState::Background;
        return huxerui::Row {}
            .With(huxerui::Frame{.width = width, .height = kProxyDelaySlotHeight},
                  ProxyProbeDotsPaint{color, foreground})
            .Key("node-probe-dots");
    });
}

inline huxerui::View ProxyNodeTile(huxerui::View title, huxerui::View detail,
    huxerui::View delay, float dotsWidth, bool selected, std::string label, std::function<void()> activate,
    SelectableTileInteraction interaction, bool canProbe, bool testing, bool blocked,
    std::function<void()> startProbe) {
    title = std::move(title).With(huxerui::Semantics{.hidden = true});
    detail = std::move(detail).With(huxerui::Grow(1.0F), huxerui::Semantics{.hidden = true});
    return huxerui::Scope([=] {
        const auto& theme = huxerui::UseTheme();
        const auto colors = ResolveSelectableTileColors(theme, selected);
        huxerui::View status = testing ? ProxyProbeDots(selected ? colors.fg : colors.muted, dotsWidth) : delay;
        if (canProbe) {
            if (!status) {
                auto color = selected ? colors.fg : colors.muted;
                if (blocked) color.alpha *= theme.interactions.disabled_opacity;
                status = huxerui::Text(Localized("测速"))
                    .LineLimit(1).Overflow(huxerui::TextOverflow::Ellipsis)
                    .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption), color});
            }
            const auto probeLabel = huxerui::UseString(Localized("测速")) + ": " + label;
            huxerui::View action = huxerui::Row {status}
                .With(huxerui::Frame{.min_height = kProxyDelaySlotHeight},
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                      huxerui::Focusable(true),
                      huxerui::Enabled(!blocked || testing),
                      huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                         .label = probeLabel,
                                         .identifier = "node-probe:" + label,
                                         .busy = testing})
                .OnClick([startProbe, testing, blocked] {
                    if (!testing && !blocked) startProbe();
                })
                .Key("node-probe-action");
            if (!blocked && !testing) {
                action = std::move(action).With(
                    huxerui::PointerCursor(huxerui::PointerCursorKind::Hand),
                    huxerui::Tooltip(Localized("测速")));
            }
            status = std::move(action);
        } else if (testing) {
            status = std::move(status).With(huxerui::Semantics{
                .label = Localized("测速中…"), .busy = true});
        }

        const bool interactive = interaction != SelectableTileInteraction::Disabled &&
                                 static_cast<bool>(activate);
        huxerui::View tile = huxerui::Column {
          title,
          huxerui::Row {
            detail,
            huxerui::Stack {status}
                .With(huxerui::Frame{.width = kProxyDelaySlotWidth, .min_height = kProxyDelaySlotHeight},
                      huxerui::Align(huxerui::HorizontalAlignment::End, huxerui::VerticalAlignment::Center))
                .Key("node-probe-slot"),
          }.With(huxerui::Spacing(4.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        }.With(huxerui::Spacing(2.0F),
               huxerui::MainAlign(huxerui::MainAxisAlignment::SpaceBetween),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
               huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 8.0F)),
               huxerui::Background(colors.surface),
               huxerui::CornerRadius(kSelectableTileRadius),
               huxerui::Grow(1.0F),
               huxerui::Semantics{.role = interactive ? huxerui::SemanticRole::Button
                                                      : huxerui::SemanticRole::Generic,
                                  .label = label,
                                  .selected = selected},
               huxerui::Focusable(interactive),
               huxerui::Enabled(interaction != SelectableTileInteraction::Disabled));
        if (selected) {
            tile = std::move(tile).With(huxerui::Border(theme.colors.primary, kCardSelectedOutlineWidth));
        }
        if (interactive) {
            tile = std::move(tile).With(ResolveCardIndication(theme, selected ? kCardSelectedOutlineWidth : 0.0F))
                .OnClick(activate);
        }
        return tile;
    });
}

} // namespace clashflux::ui
