#include "card_hover_outline.h"
#include "ui.h"

namespace clashflux::ui {

// Shared surface, selection and interaction for cards and group-picker choices.
SelectableTileColors ResolveSelectableTileColors(const huxerui::ThemeSpec& theme,
                                                 bool selected) {
    return {
        selected ? theme.colors.secondary_container : ResolveIslandTheme(theme).active,
        theme.colors.on_surface,
        theme.colors.on_surface_variant,
        theme.colors.outline,
    };
}

[[huxerui::composable]] huxerui::View SelectableTile(
    huxerui::View content, bool selected, std::string semantics_label,
    std::function<void()> on_click, SelectableTileInteraction interaction, SelectableTileStyle style,
    float corner_radius) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const bool fourColor = huxerui::UseEnvironment<FluxGradientStyle>().enabled;
    const auto hovered = huxerui::UseState(false);
    const SelectableTileColors colors =
        ResolveSelectableTileColors(theme, selected);
    const bool interactive = interaction != SelectableTileInteraction::Disabled &&
                             static_cast<bool>(on_click);
    const bool dragPreview = style == SelectableTileStyle::DragPreview;
    // 表面/圆角/描边在建树时就落在 Column 上（不能在返回值外面补修饰符，原因见
    // Card 的注释）。选中卡的底色和描边由各自的主题样式处理。
    huxerui::View tile =
        huxerui::Column {std::move(content)}
            .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 8.0F)),
                  huxerui::Background(colors.surface),
                  huxerui::CornerRadius(corner_radius),
                  huxerui::ClipChildren(),
                  huxerui::Grow(1.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                  huxerui::Semantics{.role = interactive ? huxerui::SemanticRole::Button
                                                           : huxerui::SemanticRole::Generic,
                                     .label = std::move(semantics_label), .selected = selected},
                  huxerui::Focusable(interactive),
                  huxerui::Enabled(interaction != SelectableTileInteraction::Disabled));
    if (!interactive && (selected || dragPreview)) {
        tile = WithStaticCardSelectionOutline(std::move(tile), theme, corner_radius, selected && fourColor);
    }
    if (dragPreview) {
        tile = std::move(tile).With(ResolveFluxShadows(theme).profile_drag_preview);
    }
    if (interactive) {
        tile = WithSelectableCardHover(std::move(tile), theme, hovered, corner_radius, selected, fourColor, dragPreview)
            .OnClick(std::move(on_click));
    }
    return tile;
}

} // namespace clashflux::ui
