#pragma once

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <optional>
#include <utility>
#include <huxerui/huxerui.h>

#include "theme_colors.h"

namespace clashflux::ui {

// Paint the containing card's outline independently of child indications.
struct CardHoverOutline {
    class Extension;
    struct Selection {
        huxerui::ThemeBrightness brightness;
        bool animate = true;
        bool operator==(const Selection&) const = default;
    };
    huxerui::Border border;
    huxerui::CornerRadii corners;
    huxerui::AnimationSpec fade;
    bool hovered;
    std::optional<huxerui::Color> idleColor = std::nullopt;
    std::optional<Selection> selection = std::nullopt;
    bool operator==(const CardHoverOutline&) const = default;
};

class CardHoverOutline::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode&, const CardHoverOutline& outline)
        : outline_(outline), progress_(outline.hovered ? 1.0F : 0.0F) {}

    void Update(huxerui::ViewNode&, const CardHoverOutline& outline) {
        const bool selectionChanged = outline_.selection != outline.selection ||
            (outline.selection && outline_.corners != outline.corners);
        if (outline.selection && outline.selection->animate && !outline_.selection) {
            sweep_ = std::make_unique<huxerui::MotionController>(0.0F);
            sweep_->AnimateTo(1.0F, huxerui::TweenSpec{.duration = 0.64, .easing = huxerui::Easing::Linear});
        } else if (!outline.selection || !outline.selection->animate) {
            sweep_.reset();
        }
        outline_ = outline;
        progress_.AnimateTo(outline.hovered ? 1.0F : 0.0F, outline.fade);
        InvalidatePaint(selectionChanged ? PaintInvalidation::Both : PaintInvalidation::Foreground);
    }

    FrameResult OnFrame(huxerui::ViewNode&, const huxerui::FrameInfo& frame) override {
        const auto result = progress_.Advance(frame);
        const auto sweep = sweep_ ? sweep_->Advance(frame) : huxerui::MotionAdvanceResult{};
        if (sweep_ && !sweep_->IsRunning()) sweep_.reset();
        if (result.changed || sweep.changed) InvalidatePaint(PaintInvalidation::Foreground);
        return {.needs_frame = result.needs_frame || sweep.needs_frame, .wake_after = result.wake_after};
    }

    void PaintBehindContent(const huxerui::ViewNode& node, huxerui::PaintContext& paint) const override {
        if (!outline_.selection) return;
        // Keep the four static fields in the content sequence so the sweep only records foreground paint.
        const auto bounds = node.Bounds();
        const auto path = BorderPath(bounds);
        if (path.IsEmpty()) return;
        const auto palette = ResolveFourColorForegroundPalette(outline_.selection->brightness);
        constexpr huxerui::StrokeStyle stroke{.width = kCardOutlineWidth};
        paint.StrokePath(path, palette.base, stroke);
        const std::array fields{palette.gold, palette.green, palette.blue, palette.red};
        for (std::size_t index = 0; index < fields.size(); ++index) {
            auto transparent = fields[index];
            transparent.alpha = 0.0F;
            paint.StrokePath(path, huxerui::RadialGradient{
                .center = kFourColorFieldCenters[index], .radius = {0.65F, 0.93F},
                .stops = {{0.0F, fields[index]}, {1.0F, transparent}},
            }, bounds, stroke);
        }
    }

    void PaintAboveContent(const huxerui::ViewNode& node, huxerui::PaintContext& paint) const override {
        const float progress = std::clamp(progress_.Value(), 0.0F, 1.0F);
        if (outline_.selection) {
            const auto bounds = node.Bounds();
            const float sweep = sweep_ ? sweep_->Value() : 0.0F;
            if (progress > 0.0F) {
                auto tint = huxerui::Color::White();
                tint.alpha = 0.12F * progress;
                paint.DrawBorder(bounds, tint, {.width = kCardOutlineWidth}, outline_.corners);
            }
            if (sweep > 0.0F && sweep < 1.0F) {
                const float radians = (2.0F * sweep - 0.5F) * std::numbers::pi_v<float>;
                const float x = std::cos(radians), y = std::sin(radians);
                const float edge = 0.5F / std::max(std::abs(x), std::abs(y));
                auto light = huxerui::Color::White();
                light.alpha = 0.58F * std::min({1.0F, sweep * 8.0F, (1.0F - sweep) * 8.0F});
                paint.StrokePath(BorderPath(bounds), huxerui::RadialGradient{
                    .center = {0.5F + edge * x, 0.5F + edge * y}, .radius = {0.20F, 0.35F},
                    .stops = {{0.0F, light}, {1.0F, huxerui::Color{1.0F, 1.0F, 1.0F, 0.0F}}},
                }, bounds, {.width = kCardOutlineWidth});
            }
            return;
        }
        auto color = outline_.border.color;
        if (outline_.idleColor) color = MixThemeColor(*outline_.idleColor, color, progress);
        else color.alpha *= progress;
        if (color.alpha <= 0.0F) return;
        paint.DrawBorder(node.Bounds(), color, huxerui::StrokeStyle{.width = outline_.border.width}, outline_.corners);
    }

private:
    huxerui::Path BorderPath(huxerui::Rect bounds) const {
        constexpr float inset = kCardOutlineWidth * 0.5F;
        if (std::min(bounds.width, bounds.height) <= kCardOutlineWidth) return {};
        const auto innerRadius = [](float radius) { return std::max(0.0F, radius - inset); };
        return huxerui::Path::RoundedRect(
            {bounds.x + inset, bounds.y + inset, bounds.width - kCardOutlineWidth, bounds.height - kCardOutlineWidth},
            {innerRadius(outline_.corners.top_left), innerRadius(outline_.corners.top_right),
             innerRadius(outline_.corners.bottom_right), innerRadius(outline_.corners.bottom_left)});
    }

    CardHoverOutline outline_;
    huxerui::MotionController progress_;
    std::unique_ptr<huxerui::MotionController> sweep_;
};

inline huxerui::View WithSelectableCardHover(huxerui::View card, const huxerui::ThemeSpec& theme,
    huxerui::State<bool> hovered, float radius, bool selected, bool fourColor, bool alwaysOutlined = false) {
    auto indication = ResolveCardIndication(theme, selected);
    const auto hover = *indication.hover;
    indication.hover.reset();
    const bool showOutline = hovered.Get();
    return std::move(card).With(indication, CardHoverOutline{
            *hover.border, huxerui::CornerRadii{radius}, showOutline ? hover.enter : hover.exit, showOutline,
            selected || alwaysOutlined ? std::optional{theme.colors.primary} : std::nullopt,
            selected && fourColor ? std::optional{CardHoverOutline::Selection{theme.brightness}} : std::nullopt})
        .On<huxerui::ViewEvents::Hover>([hovered](const huxerui::HoverEvent& event) {
            if (event.type != huxerui::HoverEventType::Move)
                hovered = event.type == huxerui::HoverEventType::Enter;
        });
}

inline huxerui::View WithStaticCardSelectionOutline(huxerui::View card, const huxerui::ThemeSpec& theme,
    float radius, bool fourColor) {
    const huxerui::Border border{theme.colors.primary, kCardOutlineWidth};
    if (!fourColor) return std::move(card).With(border);
    return std::move(card).With(CardHoverOutline{border, huxerui::CornerRadii{radius}, huxerui::SnapSpec{}, false,
        std::nullopt, CardHoverOutline::Selection{theme.brightness, false}});
}

} // namespace clashflux::ui
