#pragma once

#include <huxerui/huxerui.h>

namespace clashflux::ui {

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

inline huxerui::ColorScheme FluxDarkColors() {
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
    return colors;
}

inline huxerui::ColorScheme FluxLightColors() {
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

} // namespace clashflux::ui
