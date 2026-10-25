#pragma once

#include <cstddef>
#include <string>
#include <huxerui/huxerui.h>

#include "page_layout.h"

namespace clashflux::ui {

// A loading placeholder has no timer or minimum display time. Callers keep their
// data hooks mounted and replace it as soon as the first real snapshot is ready.
inline huxerui::View PageLoadingSkeleton(const huxerui::ThemeSpec& theme, bool list = false) {
    const auto item = [theme, list](std::size_t index) -> huxerui::View {
        const auto line = [theme](float width, float height) {
            return huxerui::Row {}.With(huxerui::Frame{.width = width, .height = height},
                huxerui::Background(theme.colors.surface_container_highest), huxerui::CornerRadius(4.0F));
        };
        return huxerui::Column {
          line(index % 2 == 0 ? 112.0F : 88.0F, 16.0F),
          line(index % 2 == 0 ? 72.0F : 128.0F, 12.0F),
        }.With(huxerui::Padding(16.0F), huxerui::Spacing(12.0F),
               huxerui::Frame{.height = list ? 76.0F : 108.0F},
               huxerui::Background(theme.colors.surface_container),
               huxerui::CornerRadius(theme.shapes.medium),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Start))
            .Key("page-loading-item-" + std::to_string(index));
    };
    huxerui::View content;
    if (list) {
        content = huxerui::VirtualList(8, [item](std::size_t index) {
            return huxerui::Column {item(index)}.With(
                huxerui::Padding(huxerui::EdgeInsets{.bottom = kSectionCardSpacing}),
                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
        }).EstimatedItemExtent(76.0F + kSectionCardSpacing).With(huxerui::Grow(1.0F));
    } else {
        content = huxerui::VirtualGrid(6, item)
            .Columns(huxerui::GridColumns::Adaptive(220.0F))
            .EstimatedRowExtent(108.0F)
            .ColumnSpacing(kSectionCardSpacing).RowSpacing(kSectionCardSpacing)
            .With(huxerui::Grow(1.0F));
    }
    return huxerui::Column {content}.With(huxerui::Grow(1.0F),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch), huxerui::ClipChildren())
        .Key("page-loading-skeleton");
}

inline huxerui::View PageLoadingScaffold(huxerui::StringVariant title,
    const huxerui::ThemeSpec& theme, bool list = false) {
    return PageScaffold(title, {}, PageLoadingSkeleton(theme, list), false, false, true);
}

} // namespace clashflux::ui
