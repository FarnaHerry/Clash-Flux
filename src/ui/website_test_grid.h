#pragma once
#include <limits>
#include "page_layout.h"

namespace clashflux::ui {

inline huxerui::View WebsiteTestGrid(huxerui::View header, std::vector<huxerui::View> cards,
    bool compact, huxerui::ScrollController controller, std::shared_ptr<PageScrollOffset> offset) {
    std::vector<std::size_t> spans(cards.size() + 2, 1);
    spans.front() = spans.back() = std::numeric_limits<std::size_t>::max();
    return huxerui::VirtualGrid(cards.size() + 2, [cards, header](std::size_t index) -> huxerui::View {
        if (index == 0) return header;
        if (index == cards.size() + 1) return huxerui::Row{}.With(huxerui::Frame{.height = 24.0F});
        return cards[index - 1];
    }).Columns(compact ? huxerui::GridColumns::Fixed(1) : huxerui::GridColumns::Adaptive(280.0F))
        .EstimatedRowExtent(150.0F).ItemSpans(std::move(spans))
        .ColumnSpacing(kSectionCardSpacing).RowSpacing(kSectionCardSpacing).Controller(controller)
        .With(huxerui::Grow(1.0F), huxerui::ScrollBar(), RememberPageScroll{offset});
}

} // namespace clashflux::ui
