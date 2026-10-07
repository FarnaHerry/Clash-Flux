#pragma once

#include <utility>
#include <vector>
#include <huxerui/huxerui.h>

namespace clashflux::ui {

inline huxerui::View CompactNavigationDock(huxerui::View navigation,
    const huxerui::ThemeSpec& theme) {
    return huxerui::Column {
        std::move(navigation).With(huxerui::Frame{.max_width = 352.0F},
            huxerui::CornerRadius(34.0F), huxerui::ClipChildren()),
    }.With(huxerui::Padding(huxerui::EdgeInsets{
        .right = theme.spacing.medium,
        .bottom = theme.spacing.small,
        .left = theme.spacing.medium,
    }), huxerui::MainAlign(huxerui::MainAxisAlignment::End),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

// 内容保留在同一 Row/Key 中，窗口越过断点时不重新挂载页面。
inline huxerui::View ResponsiveDesktopShell(huxerui::View content,
    huxerui::View sidebar, huxerui::View compactChrome, huxerui::View compactNavigation) {
    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    const auto& theme = huxerui::UseTheme();
    std::vector<huxerui::View> body;
    if (!compact) body.push_back(std::move(sidebar).Key("desktop-sidebar"));
    body.push_back(std::move(content).With(huxerui::Grow(1.0F)).Key("responsive-pages"));
    std::vector<huxerui::View> children;
    if (compact) children.push_back(std::move(compactChrome).Key("compact-window-chrome"));
    std::vector<huxerui::View> layers;
    layers.push_back(huxerui::Row(std::move(body)).With(huxerui::Grow(1.0F),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)).Key("responsive-body"));
    if (compact && compactNavigation)
        layers.push_back(CompactNavigationDock(std::move(compactNavigation), theme).Key("compact-navigation"));
    children.push_back(huxerui::Stack(std::move(layers)).With(huxerui::Grow(1.0F),
        huxerui::Align(huxerui::HorizontalAlignment::Stretch, huxerui::VerticalAlignment::Stretch))
        .Key("responsive-content-stack"));
    return huxerui::Column(std::move(children)).With(huxerui::Grow(1.0F), huxerui::Spacing(0.0F),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace clashflux::ui
