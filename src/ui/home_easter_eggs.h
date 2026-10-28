#pragma once

#include <array>
#include <functional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <huxerui/huxerui.h>

#include "ui.h"
#include "app_resources.h"

namespace clashflux::ui {

struct HomeEasterEgg {
    std::string_view id;
    std::string_view author;
    std::string_view url;
};

inline constexpr std::array kHomeEasterEggPool{
    HomeEasterEgg{"hui-author", "FinalScave", "https://github.com/FinalScave"},
};

inline std::size_t PickHomeEasterEgg() {
    if constexpr (kHomeEasterEggPool.size() == 1) return 0;
    static thread_local std::mt19937 random{std::random_device{}()};
    return std::uniform_int_distribution<std::size_t>{0, kHomeEasterEggPool.size() - 1}(random);
}

inline huxerui::View HomeEasterEggContent(
    HomeEasterEgg egg, std::function<void(std::string_view)> open) {
    return huxerui::Scope([egg, open = std::move(open)] {
        const auto& theme = huxerui::UseTheme();
        return huxerui::Column {
          huxerui::Text(Localized("HuxerUI 官方作者")).Style(huxerui::TextStyle{
              huxerui::Font::System(font_size::kCaption).WithWeight(huxerui::FontWeight::SemiBold),
              theme.colors.on_surface_variant}),
          huxerui::Row {
            huxerui::Image(app::images::github).Fit(huxerui::ImageFit::Contain).Tint(theme.colors.primary)
                .With(huxerui::Frame{.width = 18.0F, .height = 18.0F}),
            huxerui::Text(std::string{egg.author}).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody), theme.colors.primary}),
          }.With(huxerui::Spacing(8.0F), huxerui::Frame{.min_height = 28.0F},
                 huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                 theme.interactions.indication, huxerui::Focusable(true),
                 huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                     .label = std::string{egg.author} + " · GitHub"})
              .OnClick([egg, open] { open(egg.url); }).Key("home-easter-egg-link"),
        }.With(huxerui::Grow(1.0F), huxerui::Spacing(4.0F),
               huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    });
}

} // namespace clashflux::ui
