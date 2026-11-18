#pragma once

#include <huxerui/huxerui.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ui.h"
#include "refresh_motion.h"
#include "scroll_motion.h"

namespace clashflux::ui {

inline constexpr float kProxyDelaySlotWidth = 62.0F;
inline constexpr float kProxyDelaySlotHeight = 16.0F;
inline constexpr float kProxyProbeActionSize = 16.0F;

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

inline const huxerui::Indication& ResolveProxyProbeIndication(const huxerui::ThemeSpec& theme) {
    struct CachedProbeIndication {
        huxerui::Color primary{};
        double fast = -1.0;
        bool reducedMotion = false;
        huxerui::Indication indication{};
    };
    thread_local CachedProbeIndication cache;
    if (cache.fast != theme.motion.fast ||
        cache.reducedMotion != theme.motion.reduced_motion ||
        cache.primary != theme.colors.primary) {
        huxerui::Indication indication = ResolveFluxIndication(theme);
        indication.geometry.layer_size = huxerui::Size{kProxyProbeActionSize, kProxyProbeActionSize};
        indication.geometry.clip_corner_radii = huxerui::CornerRadii{kProxyProbeActionSize * 0.5F};
        cache = CachedProbeIndication{
            theme.colors.primary,
            theme.motion.fast,
            theme.motion.reduced_motion,
            std::move(indication)};
    }
    return cache.indication;
}

inline huxerui::View ProxyNodeTile(huxerui::View title, huxerui::View detail,
    huxerui::View delay, bool selected, std::string label, std::function<void()> activate,
    SelectableTileInteraction interaction, bool canProbe, bool testing, bool blocked,
    std::function<void()> startProbe) {
    return huxerui::Scope([=] {
        constexpr std::uint8_t kHovered = 1U << 0U;
        constexpr std::uint8_t kButtonFocused = 1U << 1U;
        constexpr std::uint8_t kAnimationReady = 1U << 2U;
        auto flags = huxerui::UseState<std::uint8_t>(0U);
        huxerui::Lifecycle([flags, testing] {
            const std::uint8_t current = flags.Get();
            const std::uint8_t next = testing
                ? static_cast<std::uint8_t>(current | kAnimationReady)
                : static_cast<std::uint8_t>(current & ~kAnimationReady);
            if (next != current) flags = next;
            return [] {};
        }, testing);
        const std::uint8_t state = flags.Get();
        const bool isHovered = (state & kHovered) != 0U;
        const bool isButtonFocused = (state & kButtonFocused) != 0U;
        const bool isAnimationReady = (state & kAnimationReady) != 0U;
        const auto& theme = huxerui::UseTheme();
        const auto wheelRoute = huxerui::UseEnvironment<ScrollMotionContext>().wheel;
        const auto colors = ResolveSelectableTileColors(theme, selected);
        const bool showButton = canProbe && (!delay || testing || isHovered || isButtonFocused);
        const bool activeProbeTarget = canProbe && (testing || isHovered || isButtonFocused);
        huxerui::View action;
        if (canProbe) {
            const auto probeLabel = huxerui::UseString(Localized("测速")) + ": " + label;
            huxerui::Color iconColor = colors.fg;
            if (blocked) iconColor.alpha *= theme.interactions.disabled_opacity;
            huxerui::View button =
                huxerui::Image(huxerui::ImageResource{"app", "images/refresh"})
                    .Fit(huxerui::ImageFit::Contain)
                    .Align(huxerui::HorizontalAlignment::Center, huxerui::VerticalAlignment::Center)
                    .Tint(showButton ? iconColor : huxerui::Color::Transparent())
                    .With(huxerui::Frame{.width = kProxyProbeActionSize, .height = kProxyProbeActionSize},
                          huxerui::Padding(1.0F),
                          huxerui::CornerRadius(kProxyProbeActionSize * 0.5F),
                          huxerui::Opacity(showButton ? 1.0F : 0.0F),
                          huxerui::Focusable(true),
                          huxerui::Enabled(!blocked),
                          huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                             .label = probeLabel,
                                             .identifier = "node-probe:" + label,
                                             .busy = testing})
                    .OnClick([startProbe, testing] { if (!testing) startProbe(); })
                    .On<huxerui::ViewEvents::FocusChanged>([flags](bool focused) {
                        const std::uint8_t current = flags.Get();
                        const bool wantFocus = focused && (current & kHovered) == 0U;
                        const std::uint8_t next = wantFocus
                            ? static_cast<std::uint8_t>(current | kButtonFocused)
                            : static_cast<std::uint8_t>(current & ~kButtonFocused);
                        if (next != current) flags = next;
                    });
            if (testing) {
                button = std::move(button).With(
                    huxerui::Rotation(RefreshIconRotation(theme, isAnimationReady)));
            }
            if (activeProbeTarget) {
                button = std::move(button).With(
                    ResolveProxyProbeIndication(theme),
                    huxerui::PointerCursor(huxerui::PointerCursorKind::Hand),
                    huxerui::Tooltip(Localized("测速")));
            }
            action = std::move(button).Key("node-probe-action");
        }
        std::vector<huxerui::View> slotChildren;
        slotChildren.reserve(2);
        if (action) slotChildren.push_back(std::move(action));
        if (!showButton && delay) slotChildren.push_back(delay);
        const bool interactive = interaction != SelectableTileInteraction::Disabled &&
                                 static_cast<bool>(activate);
        huxerui::View tile =
            huxerui::Row {
              huxerui::Column {
                title,
                detail,
              }.With(huxerui::Grow(1.0F), huxerui::Spacing(2.0F),
                     huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                     huxerui::Semantics{.hidden = true}),
              huxerui::Stack(std::move(slotChildren))
                  .With(huxerui::Frame{.width = kProxyDelaySlotWidth, .min_height = kProxyDelaySlotHeight},
                        huxerui::Align(huxerui::HorizontalAlignment::End, huxerui::VerticalAlignment::Center))
                  .Key("node-probe-slot"),
            }.With(huxerui::Spacing(4.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
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
            tile = std::move(tile).OnClick(activate);
        }
        if (canProbe) {
            tile = std::move(tile).On<huxerui::ViewEvents::Hover>(
                [flags, wheelRoute](const huxerui::HoverEvent& event) {
                    thread_local std::optional<huxerui::Point> stationaryLeavePos;
                    const std::uint8_t current = flags.Get();
                    if (event.type == huxerui::HoverEventType::Leave) {
                        stationaryLeavePos = event.window_position;
                        const auto next = static_cast<std::uint8_t>(current & ~kHovered);
                        if (next != current) flags = next;
                        return;
                    }
                    if (event.type == huxerui::HoverEventType::Enter &&
                        stationaryLeavePos && *stationaryLeavePos == event.window_position) {
                        const auto motion = wheelRoute ? wheelRoute->target.lock() : nullptr;
                        if (motion && motion->IsActive()) return;
                    }
                    stationaryLeavePos.reset();
                    const auto next = static_cast<std::uint8_t>(current | kHovered);
                    if (next != current) flags = next;
                });
        }
        return tile;
    });
}

} // namespace clashflux::ui
