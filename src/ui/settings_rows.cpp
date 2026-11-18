#include <huxerui/huxerui.h>

#include <optional>
#include <utility>

#include "settings_group_layout.h"
#include "ui.h"

namespace clashflux::ui {

[[huxerui::composable]] huxerui::View SettingItemIcon(huxerui::ImageResource icon, bool danger) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Image(icon)
        .Fit(huxerui::ImageFit::Contain)
        .Tint(danger ? theme.colors.error : theme.colors.on_surface_variant)
        .With(huxerui::Frame{.width = 20.0F, .height = 20.0F});
}

[[huxerui::composable]] huxerui::View SettingSwitchRow(
    huxerui::StringVariant label, huxerui::StringVariant hint, huxerui::View control,
    bool danger, std::optional<huxerui::ImageResource> icon) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const bool hasHint = !huxerui::UseString(hint).empty();
    // A standard switch already reserves a 48pt touch target inside the row.
    const auto verticalPadding = hasHint ? std::optional<float>{} : std::optional<float>{0.0F};
    huxerui::View text = huxerui::Column {
        huxerui::Text(label).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kBody),
            danger ? theme.colors.error : theme.colors.on_surface}),
        hasHint
            ? huxerui::View{huxerui::Text(hint).Style(huxerui::TextStyle{
                  huxerui::Font::System(font_size::kCaption),
                  theme.colors.on_surface_variant})}
            : huxerui::View{},
    }.With(huxerui::Spacing(2.0F), huxerui::Grow(1.0F));
    if (icon) {
        return SettingsContentRow(huxerui::Row {
          SettingItemIcon(*icon, danger),
          text,
          control,
        }.With(huxerui::Spacing(12.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)), verticalPadding);
    }
    return SettingsContentRow(huxerui::Row {
        std::move(text),
        control,
    }.With(huxerui::Spacing(12.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)), verticalPadding);
}

} // namespace clashflux::ui
