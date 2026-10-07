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
    huxerui::View title, huxerui::View actions, bool stackedActions,
    bool expandTitle = false) {
    if (!title) {
        if (!actions) return {};
        return huxerui::Row{huxerui::Spacer(), std::move(actions)}
            .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
            .Key("page-actions");
    }
    if (expandTitle) title = std::move(title).With(huxerui::Grow(1.0F));
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
    if (expandTitle) {
        // Spacer 默认也会增长；搜索标题行不让它分走输入框的可用宽度。
        return huxerui::Row{std::move(title), std::move(actions)}
            .With(huxerui::Frame{.min_height = kPageHeaderHeight},
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
            .Key("page-header");
    }
    return huxerui::Row{
        std::move(title), huxerui::Spacer(), std::move(actions),
    }.With(huxerui::Frame{.min_height = kPageHeaderHeight},
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
        .Key("page-header");
}

inline huxerui::View CenteredPageHeader(const huxerui::ThemeSpec& theme,
    huxerui::View title, huxerui::View leading, huxerui::View trailing) {
    const auto actionSlot = [](huxerui::View action) {
        return huxerui::Row{std::move(action)}
            .With(huxerui::Frame{.width = 40.0F, .height = 40.0F},
                  huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    };
    return huxerui::Row{
        actionSlot(std::move(leading)),
        huxerui::Row{std::move(title)}.With(huxerui::Grow(1.0F),
            huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        actionSlot(std::move(trailing)),
    }.With(huxerui::Frame{.min_height = kPageHeaderHeight},
           huxerui::Spacing(theme.spacing.small),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
        .Key("centered-page-header");
}

} // namespace clashflux::ui
