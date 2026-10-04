#pragma once

#include "ui.h"

namespace clashflux::ui {

inline constexpr float kDesktopPageHorizontalInset = 24.0F;
inline constexpr float kPageHeaderHeight = 48.0F;

#if defined(__ANDROID__) || defined(CLASHFLUX_IOS)
inline constexpr bool kPageTitlesInWindow = false;
#else
inline constexpr bool kPageTitlesInWindow = true;
#endif

inline huxerui::EdgeInsets PageContentInsets(const huxerui::ThemeSpec& theme,
    bool compact, float horizontal) {
    return {
        .top = compact ? theme.spacing.medium : kDesktopTopContentGap,
        .right = horizontal,
        .bottom = compact ? theme.spacing.medium : theme.spacing.large,
        .left = horizontal,
    };
}

inline huxerui::View PageTitleText(const huxerui::ThemeSpec& theme,
    huxerui::StringVariant title) {
    return huxerui::Text(std::move(title)).Style(huxerui::TextStyle{
        huxerui::Font::System(font_size::kTitle).WithWeight(huxerui::FontWeight::Medium),
        theme.colors.on_surface}).Key("page-title");
}

inline huxerui::View PageHeaderLayout(const huxerui::ThemeSpec& theme,
    huxerui::View title, huxerui::View actions, bool stackedActions) {
    if (!title) {
        if (!actions) return {};
        return huxerui::Row{huxerui::Spacer(), std::move(actions)}
            .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
            .Key("page-actions");
    }
    if (stackedActions) {
        return huxerui::Column{
            huxerui::Row{std::move(title)}.With(
                huxerui::Frame{.min_height = kPageHeaderHeight},
                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
            std::move(actions),
        }.With(huxerui::Spacing(theme.spacing.small),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
            .Key("page-header");
    }
    return huxerui::Row{
        std::move(title), huxerui::Spacer(), std::move(actions),
    }.With(huxerui::Frame{.min_height = kPageHeaderHeight},
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
        .Key("page-header");
}

} // namespace clashflux::ui
