#pragma once

#include <huxerui/huxerui.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "app_resources.h"

namespace clashflux::ui {

// codes is the packaged flags.codes string: three lowercase characters per flag.
inline std::optional<std::size_t> CountryFlagAtlasIndex(std::string_view code,
                                                        std::string_view codes) {
    if (code.size() != 2) return std::nullopt;
    char lower[2]{};
    for (std::size_t index = 0; index < code.size(); ++index) {
        char value = code[index];
        if (value >= 'A' && value <= 'Z') value = static_cast<char>(value - 'A' + 'a');
        if (value < 'a' || value > 'z') return std::nullopt;
        lower[index] = value;
    }
    const std::string_view key{lower, sizeof(lower)};
    const auto position = codes.find(key);
    if (position == std::string_view::npos || position % 3 != 0) return std::nullopt;
    return position / 3;
}

// Uses the packaged flag atlas. HUI text fallback does not consistently render
// regional-indicator pairs on Windows, so the card must not rely on emoji text.
[[huxerui::composable]] inline huxerui::View CountryFlag(
    std::string code, float width = 20.0F, float height = 15.0F,
    bool fallback_globe = false) {
    return huxerui::Scope([code = std::move(code), width, height, fallback_globe] {
        const auto image = huxerui::UseImage(app::images::country_flags);
        const auto codes = huxerui::UseRawResource(app::raw::country_codes_txt).ReadString(true);
        const auto index = CountryFlagAtlasIndex(code, codes);
        if (!index) {
            if (!fallback_globe) return huxerui::View{huxerui::Row{}};
            const auto& theme = huxerui::UseTheme();
            return huxerui::View{huxerui::Image(app::images::globe)
                                     .Fit(huxerui::ImageFit::Contain)
                                     .Tint(theme.colors.primary)
                                     .With(huxerui::Frame{.width = width, .height = width})};
        }
        return huxerui::View{huxerui::Canvas([image, index = *index](huxerui::PaintContext& paint,
                                                                    huxerui::Size size) {
            paint.DrawImageRect(image,
                {static_cast<float>(index % 16) * 48.0F,
                 static_cast<float>(index / 16) * 36.0F, 48.0F, 36.0F},
                {0.0F, 0.0F, size.width, size.height});
        }).With(huxerui::Frame{.width = width, .height = height})};
    });
}

} // namespace clashflux::ui
