#pragma once

#include <utility>
#include <huxerui/huxerui.h>
#include "ui.h"

namespace clashflux::ui {

// Scope 使弹窗/懒加载工厂也能安全复用；样式只在挂载后的组合期读取。
inline huxerui::View EmptyState(huxerui::StringVariant message,
    huxerui::ImageVariant icon) {
    return huxerui::Scope([message = std::move(message), icon = std::move(icon)] {
        const auto& theme = huxerui::UseTheme();
        return huxerui::Column{
            huxerui::View{huxerui::Column{
                huxerui::Image(icon).Fit(huxerui::ImageFit::Contain)
                    .Tint(theme.colors.on_surface_variant)
                    .With(huxerui::Frame{.width = 48.0F, .height = 48.0F}).Key("empty-state-icon"),
                huxerui::Text(message).Align(huxerui::TextAlign::Center)
                    .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                             theme.colors.on_surface_variant})
                    .With(huxerui::Frame{.max_width = 360.0F}).Key("empty-state-message"),
            }.With(huxerui::Frame{.max_width = 360.0F}, huxerui::Spacing(12.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)).Key("empty-state-content")},
        }.With(huxerui::Grow(1.0F), huxerui::Frame{.min_height = 160.0F},
               huxerui::Padding(24.0F),
               huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    }).With(huxerui::Grow(1.0F));
}

} // namespace clashflux::ui
