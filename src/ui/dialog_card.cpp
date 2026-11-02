#include <huxerui/huxerui.h>

#include "ui.h"
#include "page_layout.h"

namespace clashflux::ui {

[[huxerui::composable]] huxerui::View DialogCard(huxerui::View content) {
    const auto& theme = huxerui::UseTheme();
    const auto islands = ResolveIslandTheme(theme);
    // Environment is a transparent scope; the surface owns all view behavior.
    return huxerui::Column {content}.With(
        kPageTitlesInWindow ? ResolveFluxShadows(theme).dialog
            : huxerui::Shadow{ThemeShadowColor(theme, 0.30F), {}, 28.0F, 2.0F},
        huxerui::Background(islands.overlay),
        huxerui::CornerRadius(islands.island_radius),
        huxerui::Border(islands.outline_soft, kCardOutlineWidth),
        huxerui::ClipChildren(),
        huxerui::Padding(islands.island_padding),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace clashflux::ui
