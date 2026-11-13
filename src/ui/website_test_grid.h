#pragma once
#include "page_layout.h"

namespace clashflux::ui {

inline huxerui::View WebsiteTestGrid(std::vector<huxerui::View> cards,
    bool compact, std::shared_ptr<PageScrollOffset> offset) {
    return huxerui::Scope([cards, compact, offset] {
        // Like ScrollablePageContent, seed a controller when the scroll
        // container mounts again under a different page scaffold.
        const auto controller = huxerui::UseScrollController(offset->value);
        return ScrollArea(PageVirtualGrid(cards.size(), [cards](std::size_t index) { return cards[index]; },
            kSectionCardSpacing)
            .Columns(compact ? huxerui::GridColumns::Fixed(1) : huxerui::GridColumns::Adaptive(280.0F))
            .EstimatedRowExtent(150.0F)
            .ColumnSpacing(kSectionCardSpacing).Controller(controller)
            .With(RememberPageScroll{offset}), controller, offset).With(huxerui::Grow(1.0F));
    }).With(huxerui::Grow(1.0F));
}

} // namespace clashflux::ui
