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

inline std::optional<std::pair<std::string, std::string_view>> ExtractLeadingRegionalFlag(
    std::string_view text) {
    if (text.size() < 8) return std::nullopt;
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    if (bytes[0] != 0xF0U || bytes[1] != 0x9FU || bytes[2] != 0x87U ||
        bytes[4] != 0xF0U || bytes[5] != 0x9FU || bytes[6] != 0x87U ||
        bytes[3] < 0xA6U || bytes[3] > 0xBFU ||
        bytes[7] < 0xA6U || bytes[7] > 0xBFU) {
        return std::nullopt;
    }
    std::string code{
        static_cast<char>('a' + bytes[3] - 0xA6U),
        static_cast<char>('a' + bytes[7] - 0xA6U)};
    std::string_view rest = text.substr(8);
    if (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
    return std::pair{std::move(code), rest};
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
