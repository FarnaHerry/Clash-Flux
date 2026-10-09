#pragma once

#include <utility>
#include <vector>
#include <huxerui/huxerui.h>

#include "theme_colors.h"

namespace clashflux::ui {

inline huxerui::View CompactNavigationDock(huxerui::View navigation,
    const huxerui::ThemeSpec& theme) {
    return huxerui::Column {
        std::move(navigation).With(huxerui::Frame{.max_width = 352.0F},
            huxerui::Background(CompactNavigationSurfaceColor(theme)),
            huxerui::Border{CompactNavigationBorderColor(theme), 0.4F},
            huxerui::Shadow{CompactNavigationShadowColor(theme), {0.0F, 0.85F}, 2.667F},
            huxerui::CornerRadius(32.0F), huxerui::ClipChildren()),
    }.With(huxerui::Padding(huxerui::EdgeInsets{
        .right = theme.spacing.medium,
        .bottom = theme.spacing.small,
        .left = theme.spacing.medium,
    }), huxerui::MainAlign(huxerui::MainAxisAlignment::End),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

// 从胶囊 64pt 导航行的水平中线向下渐隐到页面底色，中线上方不覆盖内容。
// 与导航消费相同的底部安全区，使手机系统栏变化时起点仍对齐导航行中线。
// 遮罩铺满页面宽度，仅绘制背景，不注册输入；随胶囊一同显示和隐藏。
inline huxerui::View CompactNavigationScrim(const huxerui::ThemeSpec& theme) {
    constexpr float kPillHeight = 64.0F;   // AndroidNavigationSurface 条目高度
    const float total = kPillHeight * 0.5F + theme.spacing.small;
    const auto stop = [&theme](float offset, float alpha) {
        huxerui::Color color = theme.colors.surface_container_low;
        color.alpha = alpha;
        return huxerui::GradientStop{offset, color};
    };
    huxerui::LinearGradient gradient;
    gradient.start = {0.5F, 0.0F};
    gradient.end = {0.5F, 1.0F};
    // 起点缓慢增加覆盖，避免水平中线出现明显色带；底部保持原有覆盖强度。
    gradient.stops = {
        stop(0.0F, 0.0F),
        stop(0.25F, 0.08F),
        stop(0.5F, 0.24F),
        stop(0.75F, 0.46F),
        stop(1.0F, 0.68F),
    };
    return huxerui::Column {
        huxerui::View{huxerui::Column {
            huxerui::Row{}.With(huxerui::Frame{.height = total}),
        }.With(huxerui::SafeAreaPadding{.top = false, .right = false, .left = false},
               huxerui::Background(std::move(gradient)),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
            .Key("compact-navigation-fade")},
    }.With(huxerui::MainAlign(huxerui::MainAxisAlignment::End),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
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
    if (compact && compactNavigation) {
        layers.push_back(CompactNavigationScrim(theme).Key("compact-navigation-scrim"));
        layers.push_back(CompactNavigationDock(std::move(compactNavigation), theme).Key("compact-navigation"));
    }
    children.push_back(huxerui::Stack(std::move(layers)).With(huxerui::Grow(1.0F),
        huxerui::Align(huxerui::HorizontalAlignment::Stretch, huxerui::VerticalAlignment::Stretch))
        .Key("responsive-content-stack"));
    return huxerui::Column(std::move(children)).With(huxerui::Grow(1.0F), huxerui::Spacing(0.0F),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace clashflux::ui
