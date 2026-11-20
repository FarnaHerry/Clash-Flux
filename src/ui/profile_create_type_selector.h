#pragma once

#include <huxerui/huxerui.h>
#include <cstddef>
#include <utility>
#include <vector>

#include "theme_colors.h"

namespace clashflux::ui {

struct ProfileCreateTypeOutline {
    class Extension;
    std::size_t selected;
    huxerui::Border border;
    bool operator==(const ProfileCreateTypeOutline&) const = default;
};

class ProfileCreateTypeOutline::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode&, const ProfileCreateTypeOutline& outline) : outline_(outline) {}

    void Update(huxerui::ViewNode&, const ProfileCreateTypeOutline& outline) {
        outline_ = outline;
        InvalidatePaint(PaintInvalidation::Foreground);
    }

    void PaintAboveContent(const huxerui::ViewNode& node, huxerui::PaintContext& paint) const override {
        if (node.ChildCount() != 1) return;
        const auto& row = node.ChildAt(0);
        if (outline_.selected >= row.ChildCount()) return;
        const auto& segment = row.ChildAt(outline_.selected);
        auto bounds = segment.Bounds();
        const auto offset = segment.LayoutOffset();
        bounds.x += row.LayoutOffset().x + offset.x;
        bounds.y += row.LayoutOffset().y + offset.y;
        // Draw after the adjacent segment so its shared seam cannot cover the selected border.
        paint.DrawBorder(bounds, outline_.border.color, {.width = outline_.border.width},
                         segment.ResolvedCornerRadii());
    }

private:
    ProfileCreateTypeOutline outline_;
};

inline huxerui::View ProfileCreateTypeSelector(
    std::vector<huxerui::StringVariant> types, huxerui::State<std::size_t> selected) {
    const auto& theme = huxerui::UseTheme();
    auto style = huxerui::UseEnvironment<huxerui::SegmentedButtonStyle>();
    const huxerui::Border outline{theme.colors.primary, style.selected_border.width};
    style.selected_background = huxerui::Color::Transparent();
    style.selected_border.color = huxerui::Color::Transparent();
    style.selected_label = theme.colors.primary;
    style.selected_indication = ResolveFluxIndication(theme);
    return huxerui::ProvideEnvironment(style, huxerui::SegmentedButton(std::move(types), selected.Get())
        .OnChanged([selected](std::size_t index) { selected = index; })
        .With(ProfileCreateTypeOutline{selected.Get(), outline}).Key("profile-create-type"));
}

} // namespace clashflux::ui
