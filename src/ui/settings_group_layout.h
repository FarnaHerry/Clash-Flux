#pragma once

#include <huxerui/huxerui.h>

namespace clashflux::ui {

// Only settings groups supply these insets. Shared setting controls in forms
// keep their original geometry through the neutral default.
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

} // namespace clashflux::ui
