#pragma once

#include <huxerui/huxerui.h>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace clashflux::ui {

// Keep the real row visible without presenting an unhydrated default as a value.
inline huxerui::View SettingsValue(bool ready, huxerui::View control) {
    if (ready) return control;
    return huxerui::Text("—").With(
        huxerui::Frame{.min_width = 52.0F, .min_height = 32.0F});
}

// Select's growing display needs a content-derived width to avoid filling the
// remaining row. Measure the same styled label that the trigger actually draws.
inline huxerui::View ContentSizedSelect(std::vector<std::string> labels, std::size_t selected,
    huxerui::StringVariant label, std::function<void(std::size_t)> onChanged) {
    return huxerui::Scope([labels, selected, label, onChanged] {
        const auto& theme = huxerui::UseTheme();
        auto style = huxerui::UseEnvironment<huxerui::SelectStyle>();
        style.minimum_width = 0.0F;
        const huxerui::TextStyle textStyle{
            huxerui::Font::System(theme.typography.body_medium), theme.colors.on_surface};
        const float width = std::ceil(huxerui::UseTextMeasurer().MeasureText(labels.at(selected), textStyle).size.width +
            style.trigger_padding.left + style.trigger_padding.right + style.content_spacing + style.indicator_size);
        return huxerui::ProvideEnvironment(style,
            huxerui::Select(labels, selected, [textStyle](const std::string& text) {
                return huxerui::Text(text).Style(textStyle);
            }).Label(label).OnChanged(onChanged).With(huxerui::Frame{.width = width}));
    });
}

} // namespace clashflux::ui
