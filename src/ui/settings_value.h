#pragma once

#include <huxerui/huxerui.h>

namespace clashflux::ui {

// Keep the real row visible without presenting an unhydrated default as a value.
inline huxerui::View SettingsValue(bool ready, huxerui::View control) {
    if (ready) return control;
    return huxerui::Text("—").With(
        huxerui::Frame{.min_width = 52.0F, .min_height = 32.0F});
}

} // namespace clashflux::ui
