#pragma once

#include <algorithm>
#include <optional>
#include <utility>
#include <huxerui/huxerui.h>

#include "theme_colors.h"

namespace clashflux::ui {

// Paint the containing card's outline independently of child indications.
struct CardHoverOutline {
    class Extension;
    huxerui::Border border;
    huxerui::CornerRadii corners;
    huxerui::AnimationSpec fade;
    bool hovered;
    std::optional<huxerui::Color> idleColor = std::nullopt;
    bool operator==(const CardHoverOutline&) const = default;
};

class CardHoverOutline::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode&, const CardHoverOutline& outline)
        : outline_(outline), progress_(outline.hovered ? 1.0F : 0.0F) {}

    void Update(huxerui::ViewNode&, const CardHoverOutline& outline) {
        outline_ = outline;
        progress_.AnimateTo(outline.hovered ? 1.0F : 0.0F, outline.fade);
        InvalidatePaint(PaintInvalidation::Foreground);
    }

    FrameResult OnFrame(huxerui::ViewNode&, const huxerui::FrameInfo& frame) override {
        const auto result = progress_.Advance(frame);
        if (result.changed) InvalidatePaint(PaintInvalidation::Foreground);
        return {.needs_frame = result.needs_frame, .wake_after = result.wake_after};
    }

    void PaintAboveContent(const huxerui::ViewNode& node, huxerui::PaintContext& paint) const override {
        const float progress = std::clamp(progress_.Value(), 0.0F, 1.0F);
        auto color = outline_.border.color;
        if (outline_.idleColor) color = MixThemeColor(*outline_.idleColor, color, progress);
        else color.alpha *= progress;
        if (color.alpha <= 0.0F) return;
        paint.DrawBorder(node.Bounds(), color, huxerui::StrokeStyle{.width = outline_.border.width}, outline_.corners);
    }

private:
    CardHoverOutline outline_;
    huxerui::MotionController progress_;
};

inline huxerui::View WithSelectableCardHover(huxerui::View card, const huxerui::ThemeSpec& theme,
    huxerui::State<bool> hovered, float radius, bool selected, bool alwaysOutlined = false) {
    auto indication = ResolveCardIndication(theme, selected);
    const auto hover = *indication.hover;
    indication.hover.reset();
    const bool showOutline = hovered.Get();
    return std::move(card).With(indication, CardHoverOutline{
            *hover.border, huxerui::CornerRadii{radius}, showOutline ? hover.enter : hover.exit, showOutline,
            selected || alwaysOutlined ? std::optional{theme.colors.primary} : std::nullopt})
        .On<huxerui::ViewEvents::Hover>([hovered](const huxerui::HoverEvent& event) {
            if (event.type != huxerui::HoverEventType::Move)
                hovered = event.type == huxerui::HoverEventType::Enter;
        });
}

} // namespace clashflux::ui
