#pragma once

#include <huxerui/huxerui.h>
#include <functional>
#include <string>
#include <vector>

namespace clashflux::ui {

// Use the same retained radio interaction as language choices; only its compact
// geometry differs so the three options fit the dashboard's two-row card.
inline huxerui::View HomeModeChoices(std::vector<huxerui::StringVariant> labels,
    std::size_t selected, bool enabled, std::function<void(std::size_t)> select) {
    return huxerui::Scope([labels, selected, enabled, select] {
        auto style = huxerui::UseEnvironment<huxerui::RadioButtonStyle>();
        style.size = 18.0F;
        style.dot_radius = 4.0F;
        style.minimum_interactive_size = 36.0F;
        style.state_layer_size = 32.0F;
        std::vector<huxerui::View> options;
        for (std::size_t i = 0; i < labels.size(); ++i) {
            options.push_back(huxerui::RadioButton(labels[i], i == selected)
                .With(huxerui::Grow(1.0F), huxerui::Enabled(enabled))
                .OnChanged([select, i](bool checked) { if (checked) select(i); })
                .Key("home-mode-" + std::to_string(i)));
        }
        return huxerui::ProvideEnvironment(style, huxerui::Column(std::move(options)).With(
            huxerui::Spacing(2.0F), huxerui::Grow(1.0F),
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
    }).With(huxerui::Grow(1.0F));
}

} // namespace clashflux::ui
