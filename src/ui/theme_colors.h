#pragma once

#include <huxerui/huxerui.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace clashflux::ui {

inline constexpr int kRgbChannelMax = 255;
inline constexpr float kHsbTurnDegrees = 360.0F;
inline constexpr float kHsbHueSectorDegrees = 60.0F;
inline constexpr std::size_t kHsbHueSectorCount =
    static_cast<std::size_t>(kHsbTurnDegrees / kHsbHueSectorDegrees);
inline constexpr float kHsbHueSliderMaximum = kHsbTurnDegrees;
inline constexpr float kHsbPercentMaximum = 100.0F;
inline constexpr float kHsbUnitMinimum = 0.0F;
inline constexpr float kHsbUnitMaximum = 1.0F;
inline constexpr float kThemeAccentLightLuminanceCeiling = 0.25F;
inline constexpr float kThemeAccentDarkLuminanceFloor = 0.36F;
inline constexpr float kThemeAccentMixStep = 0.05F;
inline constexpr float kThemeAccentLightContainerTint = 0.88F;
// 两个候选文字色（黑/白）的 WCAG 对比度相等时的相对明度。
inline constexpr float kThemeColorBlackWhiteSplit = 0.179F;

// 应用颜色唯一配置入口：品牌色、深浅色角色与扩展语义色均集中在此。
struct FluxPalette {
    static constexpr huxerui::Color logo_backdrop() noexcept {
        return huxerui::Color::Rgb(255, 255, 255); // 黑猫品牌图的白色圆形底托
    }

    static constexpr huxerui::Color abyss() noexcept {
        return huxerui::Color::Rgb(6, 20, 39); // 深蓝字色，用于品牌色按钮
    }

    static constexpr huxerui::Color water() noexcept {
        return huxerui::Color::Rgb(63, 184, 255); // 品牌亮水蓝 #3FB8FF
    }

    static constexpr huxerui::Color water_deep() noexcept {
        return huxerui::Color::Rgb(18, 137, 204); // 浅色模式交互蓝
    }

    static constexpr huxerui::Color ice() noexcept {
        return huxerui::Color::Rgb(182, 242, 255); // 品牌冰青 #B6F2FF
    }
};

struct FluxAccent {
    std::string id;
    std::string name;
    huxerui::Color light, dark, onLight, onDark, lightContainer, onLightContainer;
};

struct HsbColor {
    float hue = 0.0F;
    float saturation = 0.0F;
    float brightness = 0.0F;
};

enum class HsbChannel { Hue, Saturation, Brightness };

inline const std::array<FluxAccent, 6> kFluxAccents{{
    {"blue", "蓝色", FluxPalette::water_deep(), FluxPalette::water(),
     FluxPalette::abyss(), FluxPalette::abyss(),
     huxerui::Color::Rgb(220, 238, 255), huxerui::Color::Rgb(16, 73, 103)},
    {"purple", "紫色", huxerui::Color::Rgb(108, 69, 189), huxerui::Color::Rgb(192, 165, 255),
     huxerui::Color::White(), huxerui::Color::Rgb(33, 17, 57),
     huxerui::Color::Rgb(237, 224, 255), huxerui::Color::Rgb(60, 27, 107)},
    {"green", "绿色", huxerui::Color::Rgb(38, 117, 70), huxerui::Color::Rgb(130, 220, 166),
     huxerui::Color::White(), huxerui::Color::Rgb(9, 38, 22),
     huxerui::Color::Rgb(207, 242, 218), huxerui::Color::Rgb(18, 65, 35)},
    {"orange", "橙色", huxerui::Color::Rgb(162, 78, 8), huxerui::Color::Rgb(255, 184, 119),
     huxerui::Color::White(), huxerui::Color::Rgb(48, 24, 4),
     huxerui::Color::Rgb(255, 227, 201), huxerui::Color::Rgb(100, 46, 5)},
    {"pink", "粉色", huxerui::Color::Rgb(178, 50, 121), huxerui::Color::Rgb(251, 166, 209),
     huxerui::Color::White(), huxerui::Color::Rgb(48, 13, 33),
     huxerui::Color::Rgb(255, 222, 238), huxerui::Color::Rgb(105, 23, 67)},
    {"teal", "青色", huxerui::Color::Rgb(0, 117, 117), huxerui::Color::Rgb(108, 214, 210),
     huxerui::Color::White(), huxerui::Color::Rgb(0, 36, 35),
     huxerui::Color::Rgb(196, 242, 237), huxerui::Color::Rgb(0, 69, 67)},
}};

inline std::optional<std::string> NormalizeThemeColor(std::string_view value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return {};
    value = value.substr(start, value.find_last_not_of(" \t\r\n") - start + 1);
    if (value.starts_with('#')) value.remove_prefix(1);
    if (value.size() != 6) return {};
    std::string result = "#";
    for (char ch : value) {
        if (ch >= 'a' && ch <= 'f') ch -= 'a' - 'A';
        if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F'))) return {};
        result += ch;
    }
    return result;
}

inline huxerui::Color ThemeColorFromHex(std::string_view normalized) {
    unsigned value = 0;
    for (const char ch : normalized.substr(1))
        value = value * 16 + static_cast<unsigned>(ch <= '9' ? ch - '0' : ch - 'A' + 10);
    return huxerui::Color::Rgb((value >> 16) & 255, (value >> 8) & 255, value & 255);
}

inline HsbColor ThemeColorToHsb(huxerui::Color color) noexcept {
    const float maximum = std::max({color.red, color.green, color.blue});
    const float minimum = std::min({color.red, color.green, color.blue});
    const float delta = maximum - minimum;
    float hue = 0.0F;
    if (delta > 0.0F) {
        if (maximum == color.red) {
            hue = kHsbHueSectorDegrees * std::fmod((color.green - color.blue) / delta, 6.0F);
        } else if (maximum == color.green) {
            hue = kHsbHueSectorDegrees * (((color.blue - color.red) / delta) + 2.0F);
        } else {
            hue = kHsbHueSectorDegrees * (((color.red - color.green) / delta) + 4.0F);
        }
        if (hue < 0.0F) hue += kHsbTurnDegrees;
    }
    return {hue, maximum == 0.0F ? 0.0F : delta / maximum, maximum};
}

inline huxerui::Color ThemeColorFromHsb(HsbColor color) noexcept {
    float hue = std::fmod(color.hue, kHsbTurnDegrees);
    if (hue < 0.0F) hue += kHsbTurnDegrees;
    const float saturation = std::clamp(color.saturation, 0.0F, 1.0F);
    const float brightness = std::clamp(color.brightness, 0.0F, 1.0F);
    const float chroma = brightness * saturation;
    const float sector = std::fmod(hue / kHsbHueSectorDegrees, 2.0F);
    const float second = chroma * (1.0F - std::abs(sector - 1.0F));
    const float offset = brightness - chroma;
    float red = 0.0F, green = 0.0F, blue = 0.0F;
    if (hue < kHsbHueSectorDegrees) {
        red = chroma; green = second;
    } else if (hue < 2.0F * kHsbHueSectorDegrees) {
        red = second; green = chroma;
    } else if (hue < 3.0F * kHsbHueSectorDegrees) {
        green = chroma; blue = second;
    } else if (hue < 4.0F * kHsbHueSectorDegrees) {
        green = second; blue = chroma;
    } else if (hue < 5.0F * kHsbHueSectorDegrees) {
        red = second; blue = chroma;
    } else {
        red = chroma; blue = second;
    }
    return {red + offset, green + offset, blue + offset, 1.0F};
}

inline HsbColor UpdateHsbChannel(HsbColor color, HsbChannel channel, float sliderValue) noexcept {
    switch (channel) {
    case HsbChannel::Hue:
        color.hue = std::clamp(sliderValue, kHsbUnitMinimum, kHsbHueSliderMaximum);
        break;
    case HsbChannel::Saturation:
        color.saturation = std::clamp(sliderValue / kHsbPercentMaximum, kHsbUnitMinimum, kHsbUnitMaximum);
        break;
    case HsbChannel::Brightness:
        color.brightness = std::clamp(sliderValue / kHsbPercentMaximum, kHsbUnitMinimum, kHsbUnitMaximum);
        break;
    }
    return color;
}

inline huxerui::LinearGradient HsbChannelGradient(HsbColor selected, HsbChannel channel) {
    huxerui::LinearGradient gradient;
    switch (channel) {
    case HsbChannel::Hue:
        gradient.stops.reserve(kHsbHueSectorCount + 1U);
        for (std::size_t sector = 0; sector <= kHsbHueSectorCount; ++sector) {
            const float hue = static_cast<float>(sector) * kHsbHueSectorDegrees;
            gradient.stops.push_back({hue / kHsbTurnDegrees,
                ThemeColorFromHsb({hue, kHsbUnitMaximum, kHsbUnitMaximum})});
        }
        break;
    case HsbChannel::Saturation:
        gradient.stops = {
            {kHsbUnitMinimum, ThemeColorFromHsb({selected.hue, kHsbUnitMinimum, selected.brightness})},
            {kHsbUnitMaximum, ThemeColorFromHsb({selected.hue, kHsbUnitMaximum, selected.brightness})},
        };
        break;
    case HsbChannel::Brightness:
        gradient.stops = {
            {kHsbUnitMinimum, ThemeColorFromHsb({selected.hue, selected.saturation, kHsbUnitMinimum})},
            {kHsbUnitMaximum, ThemeColorFromHsb({selected.hue, selected.saturation, kHsbUnitMaximum})},
        };
        break;
    }
    return gradient;
}

inline std::string ThemeColorToHex(huxerui::Color color) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string value = "#";
    for (float channel : {color.red, color.green, color.blue}) {
        const int byte = static_cast<int>(std::round(std::clamp(channel, 0.0F, 1.0F) * kRgbChannelMax));
        value += digits[byte >> 4]; value += digits[byte & 15];
    }
    return value;
}

inline std::vector<std::string> ReadCustomThemeColors(std::string_view saved) {
    std::vector<std::string> colors;
    while (!saved.empty()) {
        const auto end = saved.find('\n');
        const auto color = NormalizeThemeColor(saved.substr(0, end));
        if (color && std::find(colors.begin(), colors.end(), *color) == colors.end()) colors.push_back(*color);
        if (end == std::string_view::npos) break;
        saved.remove_prefix(end + 1);
    }
    return colors;
}

inline std::string SaveCustomThemeColors(const std::vector<std::string>& colors) {
    std::string saved;
    for (const auto& color : colors) { if (!saved.empty()) saved += '\n'; saved += color; }
    return saved;
}

inline bool IsPresetThemeColor(std::string_view id) {
    return std::any_of(kFluxAccents.begin(), kFluxAccents.end(),
        [id](const FluxAccent& accent) { return accent.id == id; });
}

inline std::vector<std::string> ReadHiddenThemeColors(std::string_view saved) {
    std::vector<std::string> colors;
    while (!saved.empty()) {
        const auto end = saved.find('\n');
        const auto id = saved.substr(0, end);
        if (IsPresetThemeColor(id) && std::find(colors.begin(), colors.end(), id) == colors.end())
            colors.emplace_back(id);
        if (end == std::string_view::npos) break;
        saved.remove_prefix(end + 1);
    }
    return colors;
}

inline std::string SaveHiddenThemeColors(const std::vector<std::string>& colors) {
    std::string saved;
    for (const auto& color : colors) { if (!saved.empty()) saved += '\n'; saved += color; }
    return saved;
}

inline std::vector<std::string> HidePresetThemeColor(
    std::vector<std::string> hidden, std::string_view id) {
    if (IsPresetThemeColor(id) &&
        std::find(hidden.begin(), hidden.end(), id) == hidden.end())
        hidden.emplace_back(id);
    return hidden;
}

inline std::vector<std::string> ReplaceCustomThemeColor(
    std::vector<std::string> colors, std::string_view existing, std::string_view replacement) {
    const auto normalized = NormalizeThemeColor(replacement);
    if (!normalized) return colors;
    bool replaced = false;
    for (auto& color : colors) {
        if (color == existing) {
            color = *normalized;
            replaced = true;
        }
    }
    if (!replaced) colors.push_back(*normalized);
    std::vector<std::string> unique;
    for (auto& color : colors) {
        if (std::find(unique.begin(), unique.end(), color) == unique.end())
            unique.push_back(std::move(color));
    }
    return unique;
}

inline std::vector<std::string> RemoveCustomThemeColor(
    std::vector<std::string> colors, std::string_view removed) {
    std::erase_if(colors, [removed](const std::string& color) { return color == removed; });
    return colors;
}

inline float ThemeColorLuminance(huxerui::Color color) {
    const auto linear = [](float value) {
        return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
    };
    return 0.2126F * linear(color.red) + 0.7152F * linear(color.green) + 0.0722F * linear(color.blue);
}

inline huxerui::Color MixThemeColor(huxerui::Color color, huxerui::Color target, float weight) {
    return {color.red + (target.red - color.red) * weight,
            color.green + (target.green - color.green) * weight,
            color.blue + (target.blue - color.blue) * weight, 1.0F};
}

inline FluxAccent ResolveFluxAccent(std::string_view id) {
    for (const auto& accent : kFluxAccents) if (accent.id == id) return accent;
    if (id.starts_with('#')) if (const auto hex = NormalizeThemeColor(id)) {
        const auto base = ThemeColorFromHex(*hex);
        auto light = base, dark = base;
        // 深浅模式分别调整亮度，保持色相，并保证按钮与控件的可见对比。
        while (ThemeColorLuminance(light) > kThemeAccentLightLuminanceCeiling)
            light = MixThemeColor(light, huxerui::Color::Black(), kThemeAccentMixStep);
        while (ThemeColorLuminance(dark) < kThemeAccentDarkLuminanceFloor)
            dark = MixThemeColor(dark, huxerui::Color::White(), kThemeAccentMixStep);
        const auto onLight = ThemeColorLuminance(light) > kThemeColorBlackWhiteSplit ?
            huxerui::Color::Black() : huxerui::Color::White();
        return {*hex, *hex, light, dark, onLight, huxerui::Color::Black(),
                MixThemeColor(base, huxerui::Color::White(), kThemeAccentLightContainerTint), FluxPalette::abyss()};
    }
    return kFluxAccents.front();
}

inline void ApplyFluxAccent(huxerui::ColorScheme& colors, bool dark, std::string_view id) {
    const auto& accent = ResolveFluxAccent(id);
    colors.primary = dark ? accent.dark : accent.light;
    colors.on_primary = dark ? accent.onDark : accent.onLight;
    if (dark) {
        colors.on_primary_container = accent.id == "blue" ? FluxPalette::ice() : accent.dark;
    } else {
        colors.primary_container = accent.lightContainer;
        colors.on_primary_container = accent.onLightContainer;
    }
}

inline huxerui::ColorScheme FluxDarkColors(std::string_view accent = "blue") {
    auto colors = huxerui::MaterialDarkThemeSpec().colors;
    colors.primary = FluxPalette::water();
    colors.on_primary = FluxPalette::abyss();
    // 外框比内容底色亮，卡片/交互表面再次提亮。
    colors.primary_container = huxerui::Color::Rgb(34, 35, 36);
    colors.on_primary_container = FluxPalette::ice();
    colors.secondary = huxerui::Color::Rgb(185, 185, 185);
    colors.on_secondary = huxerui::Color::Rgb(32, 32, 32);
    colors.secondary_container = huxerui::Color::Rgb(43, 43, 43);
    colors.on_secondary_container = huxerui::Color::Rgb(226, 226, 226);
    colors.tertiary_container = huxerui::Color::Rgb(36, 36, 36);
    colors.on_tertiary_container = huxerui::Color::Rgb(222, 222, 222);
    colors.background = huxerui::Color::Rgb(26, 27, 28);
    colors.surface = huxerui::Color::Rgb(22, 23, 24);
    colors.surface_container_low = huxerui::Color::Rgb(20, 21, 22);
    colors.surface_container = huxerui::Color::Rgb(30, 31, 32);
    colors.surface_container_high = huxerui::Color::Rgb(36, 37, 38);
    colors.surface_container_highest = huxerui::Color::Rgb(42, 43, 44);
    colors.on_surface = huxerui::Color::Rgb(241, 241, 241);
    colors.on_surface_variant = huxerui::Color::Rgb(176, 176, 176);
    colors.outline = huxerui::Color::Rgb(65, 65, 65);
    colors.inverse_surface = huxerui::Color::Rgb(232, 232, 232);
    colors.inverse_on_surface = huxerui::Color::Rgb(32, 32, 32);
    colors.scrim = huxerui::Color::Rgb(7, 7, 7, 0.66F);
    colors.error = huxerui::Color::Rgb(255, 144, 153);
    ApplyFluxAccent(colors, true, accent);
    return colors;
}

inline huxerui::ColorScheme FluxLightColors(std::string_view accent = "blue") {
    auto colors = huxerui::MaterialLightThemeSpec().colors;
    colors.primary = FluxPalette::water_deep();
    colors.on_primary = FluxPalette::abyss();
    colors.primary_container = huxerui::Color::Rgb(220, 238, 255);
    colors.on_primary_container = huxerui::Color::Rgb(16, 73, 103);
    colors.secondary = huxerui::Color::Rgb(94, 104, 114);
    colors.on_secondary = huxerui::Color::White();
    colors.secondary_container = huxerui::Color::Rgb(227, 231, 235);
    colors.on_secondary_container = huxerui::Color::Rgb(48, 56, 64);
    colors.tertiary_container = huxerui::Color::Rgb(236, 238, 240);
    colors.on_tertiary_container = huxerui::Color::Rgb(63, 70, 77);
    colors.background = huxerui::Color::Rgb(250, 251, 252);
    colors.surface = huxerui::Color::Rgb(253, 253, 253);
    colors.surface_container_low = huxerui::Color::Rgb(239, 241, 244);
    colors.surface_container = huxerui::Color::Rgb(253, 253, 253);
    colors.surface_container_high = huxerui::Color::Rgb(250, 251, 252);
    colors.surface_container_highest = huxerui::Color::Rgb(253, 253, 253);
    colors.on_surface = huxerui::Color::Rgb(32, 36, 41);
    colors.on_surface_variant = huxerui::Color::Rgb(98, 108, 118);
    colors.outline = huxerui::Color::Rgb(209, 214, 220);
    colors.inverse_surface = huxerui::Color::Rgb(37, 42, 48);
    colors.inverse_on_surface = huxerui::Color::Rgb(242, 244, 246);
    colors.scrim = huxerui::Color::Rgb(17, 24, 32, 0.34F);
    colors.error = huxerui::Color::Rgb(180, 35, 50);
    ApplyFluxAccent(colors, false, accent);
    return colors;
}

inline bool IsDarkTheme(const huxerui::ThemeSpec& theme) noexcept {
    const huxerui::Color background = theme.colors.background;
    const float brightness = 0.2126F * background.red +
                             0.7152F * background.green +
                             0.0722F * background.blue;
    return brightness < 0.5F;
}

inline huxerui::Color SemanticSuccessColor(const huxerui::ThemeSpec& theme) {
    return IsDarkTheme(theme) ? huxerui::Color::Rgb(108, 202, 145)
                              : huxerui::Color::Rgb(47, 128, 86);
}

inline huxerui::Color SemanticWarningColor(const huxerui::ThemeSpec& theme) {
    return IsDarkTheme(theme) ? huxerui::Color::Rgb(230, 184, 102)
                              : huxerui::Color::Rgb(144, 96, 8);
}

// 框架无 on_error 角色，复用当前主题反色正文，避免深色模式粉色按钮配白字。
inline huxerui::Color OnErrorColor(const huxerui::ThemeSpec& theme) {
    return theme.colors.inverse_on_surface;
}

// 阴影使用当前主题遮罩的色相，透明度由对应的表面层级决定。
inline huxerui::Color ThemeShadowColor(const huxerui::ThemeSpec& theme, float opacity) {
    auto color = theme.colors.scrim;
    color.alpha = opacity;
    return color;
}

inline huxerui::Color CompactNavigationIndicatorColor(const huxerui::ThemeSpec& theme) {
    auto color = theme.colors.primary;
    color.alpha = 0.16F;
    return color;
}

inline huxerui::Color CompactNavigationSurfaceColor(const huxerui::ThemeSpec& theme) {
    // Telegram 的纯色回退先合成背景和叠色，得到不透明的导航目标色。
    // 深色用更亮的浮层角色，浅色用略深于白色卡片的角色，以边缘建立层次。
    auto color = IsDarkTheme(theme) ? theme.colors.surface_container_highest
                                  : theme.colors.surface_container_high;
    color.alpha = 1.0F;
    return color;
}

inline huxerui::Color CompactNavigationBorderColor(const huxerui::ThemeSpec& theme) {
    // HuxerUI Border 为单色，取 Telegram 上下边缘强度的平均值。
    auto color = theme.colors.on_surface;
    color.alpha = (IsDarkTheme(theme) ? 11.5F : 24.5F) / 255.0F;
    return color;
}

inline huxerui::Color CompactNavigationShadowColor(const huxerui::ThemeSpec& theme) {
    return ThemeShadowColor(theme, (IsDarkTheme(theme) ? 4.0F : 32.0F) / 255.0F);
}

} // namespace clashflux::ui
