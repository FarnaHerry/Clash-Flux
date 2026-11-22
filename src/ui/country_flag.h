#pragma once

#include <huxerui/huxerui.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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

// Resolve the leading emoji against the same packaged atlas on every platform.
// The remaining text is for display only. Keep the original name for search and selection.
struct ProxyNameFlag {
    std::size_t index;
    std::string_view name;
};

inline std::optional<ProxyNameFlag> UseProxyNameFlag(std::string_view name) {
    const auto prefix = ExtractLeadingRegionalFlag(name);
    if (!prefix) return std::nullopt;
    const auto codes = huxerui::UseRawResource(app::raw::country_codes_txt).ReadString(true);
    const auto index = CountryFlagAtlasIndex(prefix->first, codes);
    if (!index) return std::nullopt;
    return ProxyNameFlag{*index, prefix->second};
}

inline constexpr float kCountryFlagWidth = 20.0F;
inline constexpr float kCountryFlagHeight = 15.0F;
inline constexpr float kCountryFlagTextGap = 4.0F;

inline huxerui::View UseCountryFlagTile(std::size_t index, float width = kCountryFlagWidth,
                                       float height = kCountryFlagHeight) {
    const auto image = huxerui::UseImage(app::images::country_flags);
    return huxerui::Canvas([image, index](huxerui::PaintContext& paint, huxerui::Size size) {
        paint.DrawImageRect(image,
            {static_cast<float>(index % 16) * 48.0F,
             static_cast<float>(index / 16) * 36.0F, 48.0F, 36.0F},
            {0.0F, 0.0F, size.width, size.height});
    }).With(huxerui::Frame{.width = width, .height = height});
}

template<class Title>
huxerui::View UseProxyNameTitle(std::string_view name, Title renderTitle) {
    const auto flag = UseProxyNameFlag(name);
    auto title = renderTitle(flag ? flag->name : name);
    if (!flag) return title;
    return huxerui::Row {
      UseCountryFlagTile(flag->index),
      std::move(title).With(huxerui::Grow(1.0F)),
    }.With(huxerui::Spacing(kCountryFlagTextGap), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

[[huxerui::composable]] inline huxerui::View CountryFlag(
    std::string code, float width = kCountryFlagWidth, float height = kCountryFlagHeight,
    bool fallback_globe = false) {
    return huxerui::Scope([code = std::move(code), width, height, fallback_globe] {
        const auto codes = huxerui::UseRawResource(app::raw::country_codes_txt).ReadString(true);
        const auto index = CountryFlagAtlasIndex(code, codes);
        if (index) return UseCountryFlagTile(*index, width, height);
        if (!fallback_globe) return huxerui::View{huxerui::Row{}};
        const auto& theme = huxerui::UseTheme();
        return huxerui::View{huxerui::Image(app::images::globe)
                                 .Fit(huxerui::ImageFit::Contain)
                                 .Tint(theme.colors.on_surface_variant)
                                 .With(huxerui::Frame{.width = width, .height = width})};
    });
}

} // namespace clashflux::ui
