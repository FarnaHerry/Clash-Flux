#pragma once

#include <huxerui/huxerui.h>
#include <functional>
#include <optional>

namespace clashflux::ui {

// Group padding belongs to each row so an action's feedback includes its
// whitespace. Shared setting controls outside groups keep the neutral default.
struct SettingsRowInsets {
    float horizontal = 0.0F;
    float vertical = 0.0F;
    float minimum_height = 0.0F;
    static SettingsRowInsets Default() { return {}; }
    bool operator==(const SettingsRowInsets&) const = default;
};

inline huxerui::View SettingsContentRow(huxerui::View content) {
    return huxerui::Scope([content]() -> huxerui::View {
        const auto insets = huxerui::UseEnvironment<SettingsRowInsets>();
        if (insets == SettingsRowInsets{}) return content;
        return huxerui::Column {content}.With(
            huxerui::Padding(huxerui::EdgeInsets::Symmetric(insets.horizontal, insets.vertical)),
            huxerui::Frame{.min_height = insets.minimum_height},
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    });
}

// Whole-row actions share the group's actual cell bounds. Control-specific
// indication geometry (for example a circular icon state layer) must not leak in.
inline huxerui::View SettingsActionRow(huxerui::View content, std::function<void()> onClick,
    huxerui::Semantics semantics, std::optional<SettingsRowInsets> overrideInsets = {}) {
    return huxerui::Scope([content, onClick, semantics, overrideInsets] {
        const auto& theme = huxerui::UseTheme();
        const auto insets = overrideInsets.value_or(huxerui::UseEnvironment<SettingsRowInsets>());
        auto indication = theme.interactions.indication;
        indication.geometry = {};
        for (auto* layer : {&indication.focus, &indication.hover, &indication.press})
            if (*layer) (*layer)->corner_radii.reset();
        return huxerui::Row {
          huxerui::View{content}.With(huxerui::Grow(1.0F)),
        }.With(
            huxerui::Padding(huxerui::EdgeInsets::Symmetric(insets.horizontal, insets.vertical)),
            huxerui::Frame{.min_height = insets.minimum_height},
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
            huxerui::CornerRadius(0.0F), huxerui::ClipChildren(), indication,
            huxerui::Focusable(true), huxerui::PointerCursor(huxerui::PointerCursorKind::Hand), semantics)
            .OnClick(onClick);
    });
}

} // namespace clashflux::ui
