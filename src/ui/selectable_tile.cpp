#include "ui.h"

namespace clashflux::ui {

// Shared surface, selection and interaction for cards and group-picker choices.
SelectableTileColors ResolveSelectableTileColors(const huxerui::ThemeSpec& theme,
                                                 bool selected) {
    SelectableTileColors colors;
    if (selected) {
        // 选中态是「容器淡染底 + 主色描边」（描边由 SelectableTile 落笔）：
        // 实心 primary 铺满整卡过重，primary_container 保持表面层次，文字用
        // on_primary_container 常规对比，标题不再整段反色。
        colors.surface = theme.colors.primary_container;
        colors.fg = theme.colors.on_primary_container;
        colors.muted = theme.colors.on_primary_container;
        colors.faint = theme.colors.on_primary_container;
        colors.muted.alpha = 0.78F;
        colors.faint.alpha = 0.62F;
    } else {
        colors.surface = ResolveIslandTheme(theme).active;
        colors.fg = theme.colors.on_surface;
        colors.muted = theme.colors.on_surface_variant;
        colors.faint = theme.colors.outline;
    }
    return colors;
}

[[huxerui::composable]] huxerui::View SelectableTile(
    huxerui::View content, bool selected, std::string semantics_label,
    std::function<void()> on_click, SelectableTileInteraction interaction, SelectableTileStyle style) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const SelectableTileColors colors =
        ResolveSelectableTileColors(theme, selected);
    const bool interactive = interaction != SelectableTileInteraction::Disabled &&
                             static_cast<bool>(on_click);
    // 表面/圆角/描边在建树时就落在 Column 上（不能在返回值外面补修饰符，原因见
    // Card 的注释）：未选中 islands.active，选中 primary_container 淡染底 +
    // primary 描边。
    huxerui::View tile =
        huxerui::Column {std::move(content)}
            .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 8.0F)),
                  huxerui::Background(colors.surface),
                  huxerui::CornerRadius(kSelectableTileRadius),
                  huxerui::Border(selected ? theme.colors.primary
                                           : huxerui::Color::Transparent(),
                                  selected ? (style == SelectableTileStyle::Subscription
                                      ? kSubscriptionSelectedOutlineWidth : kCardSelectedOutlineWidth) : 0.0F),
                  huxerui::ClipChildren(),
                  huxerui::Grow(1.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                  huxerui::Semantics{.role = interactive ? huxerui::SemanticRole::Button
                                                           : huxerui::SemanticRole::Generic,
                                     .label = std::move(semantics_label), .selected = selected},
                  huxerui::Focusable(interactive),
                  huxerui::Enabled(interaction != SelectableTileInteraction::Disabled));
    if (interactive) {
        tile = std::move(tile).OnClick(std::move(on_click));
    }
    return tile;
}

} // namespace clashflux::ui
