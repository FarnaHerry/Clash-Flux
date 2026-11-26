#pragma once

#include <huxerui/huxerui.h>
#include <functional>
#include <string>
#include <vector>

#include "card_hover_outline.h"
#include "settings_group_layout.h"

namespace clashflux::ui {

inline huxerui::View HomeModeChoices(std::vector<huxerui::StringVariant> labels,
    std::size_t selected, bool enabled, std::function<void(std::size_t)> select) {
    return huxerui::Scope([labels, selected, enabled, select] {
        const auto& theme = huxerui::UseTheme();
        const auto& gradient = huxerui::UseEnvironment<FluxGradientStyle>();
        const SettingsRowInsets insets{.horizontal = theme.spacing.small, .minimum_height = 36.0F};
        std::vector<huxerui::View> options;
        for (std::size_t i = 0; i < labels.size(); ++i) {
            const bool checked = i == selected;
            const auto foreground = checked && !gradient.enabled ? theme.colors.primary : theme.colors.on_surface;
            const auto background = checked ? theme.colors.secondary_container : theme.colors.surface_container_high;
            huxerui::View row = SettingsActionRow(
                huxerui::Text(labels[i]).With(huxerui::Foreground(foreground)),
                [select, i, checked] { if (!checked) select(i); },
                {.role = huxerui::SemanticRole::RadioButton, .label = labels[i],
                 .checked = checked ? huxerui::SemanticCheckedState::Checked : huxerui::SemanticCheckedState::Unchecked,
                 .descendants = huxerui::SemanticDescendantPolicy::Exclude},
                insets)
                .With(huxerui::Grow(1.0F), huxerui::Enabled(enabled))
                .Key("home-mode-" + std::to_string(i));
            options.push_back(huxerui::Column {row}.With(
                huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                huxerui::Background(background), huxerui::CornerRadius(theme.shapes.small),
                huxerui::ClipChildren(),
                FourColorOverlayOutline{checked && gradient.enabled, true, theme.brightness,
                    theme.colors.primary, huxerui::CornerRadii{theme.shapes.small}, kCardOutlineWidth,
                    gradient.palette}));
        }
        return huxerui::Column(std::move(options)).With(
            huxerui::Spacing(theme.spacing.extra_small), huxerui::Grow(1.0F),
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }).With(huxerui::Grow(1.0F));
}

} // namespace clashflux::ui
