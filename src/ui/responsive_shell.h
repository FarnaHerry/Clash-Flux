#pragma once

#include <utility>
#include <vector>
#include <huxerui/huxerui.h>

namespace clashflux::ui {

// 内容保留在同一 Row/Key 中，窗口越过断点时不重新挂载页面。
inline huxerui::View ResponsiveDesktopShell(huxerui::View content,
    huxerui::View sidebar, huxerui::View compactChrome, huxerui::View compactNavigation) {
    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    std::vector<huxerui::View> body;
    if (!compact) body.push_back(std::move(sidebar).Key("desktop-sidebar"));
    body.push_back(std::move(content).With(huxerui::Grow(1.0F)).Key("responsive-pages"));
    std::vector<huxerui::View> children;
    if (compact) children.push_back(std::move(compactChrome).Key("compact-window-chrome"));
    children.push_back(huxerui::Row(std::move(body)).With(huxerui::Grow(1.0F),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)).Key("responsive-body"));
    if (compact && compactNavigation) children.push_back(std::move(compactNavigation).Key("compact-navigation"));
    return huxerui::Column(std::move(children)).With(huxerui::Grow(1.0F), huxerui::Spacing(0.0F),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace clashflux::ui
