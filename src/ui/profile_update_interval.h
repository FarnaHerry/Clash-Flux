#pragma once

#include <charconv>
#include <optional>
#include <string_view>

#include "ui.h"

namespace clashflux::ui {

inline std::optional<int> ParseProfileUpdateInterval(std::string_view text) {
    if (text.empty()) return {};
    for (char c : text) if (c < '0' || c > '9') return {};
    int minutes = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), minutes);
    if (error != std::errc{} || end != text.data() + text.size()) return {};
    return minutes;
}

// Older records can have a retained positive interval behind a disabled flag.
inline int ProfileUpdateIntervalForEdit(bool enabled, int minutes) {
    return enabled && minutes > 0 ? minutes : 0;
}

inline huxerui::StringVariant ProfileUpdateIntervalError() {
    return Localized("自动更新间隔需要填写非负整数（分钟）");
}

inline huxerui::View ProfileUpdateIntervalField(huxerui::State<huxerui::TextEditingValue> interval) {
    return huxerui::Scope([interval] {
        const auto value = interval.Get();
        const auto& theme = huxerui::UseTheme();
        return huxerui::Column {
          huxerui::TextField(value)
              .Label(Localized("自动更新间隔（分钟）"))
              .Variant(huxerui::TextFieldVariant::Outlined)
              .InputConfiguration({.type = huxerui::TextInputType::Number, .autocorrect = false})
              .Validation(ParseProfileUpdateInterval(value.text) ? huxerui::ValidationResult::None()
                  : huxerui::ValidationResult::Invalid(ProfileUpdateIntervalError()))
              .OnChanged([interval](const huxerui::TextEditingValue& next) { interval = next; })
              .Key("profile-update-interval"),
          huxerui::Text(Localized("0 为关闭自动更新")).Style({
              huxerui::Font::System(font_size::kCaption), theme.colors.on_surface_variant}),
        }.With(huxerui::Spacing(theme.spacing.extra_small),
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    });
}

} // namespace clashflux::ui
