#pragma once

#include <huxerui/huxerui.h>
#include <optional>
#include "theme_colors.h"

namespace clashflux::ui {

// Paint-only focus reveal. The caller supplies focus and foreground/reduced-motion
// policy; the retained modifier owns frame progression without recomposition.
huxerui::View WithSearchFocusOutline(huxerui::View content, bool focused,
                                     huxerui::Color color, bool animated = true,
                                     bool fourColor = false,
                                     huxerui::ThemeBrightness brightness = huxerui::ThemeBrightness::Light,
                                     std::optional<FluxMultiColorPalette> palette = std::nullopt);

} // namespace clashflux::ui
