#pragma once
#include <huxerui/huxerui.h>
#include "theme_colors.h"
#include "toast_stack.h"

namespace clashflux::ui {

// Preserve the Material geometry while replacing owning indication colors.
inline huxerui::View FluxInteractionStyles(huxerui::View content) {
    return huxerui::Scope([content] {
        const auto& theme = huxerui::UseTheme();
        auto icons = huxerui::UseEnvironment<huxerui::IconButtonStyle>();
        icons.indication = ResolveFluxIndication(theme);
        auto chips = huxerui::UseEnvironment<huxerui::ChipStyle>();
        chips.indication = ResolveFluxIndication(theme);
        chips.selected_indication = ResolveFluxIndication(theme, theme.colors.on_primary_container);
        auto tabs = huxerui::UseEnvironment<huxerui::TabsStyle>();
        tabs.indication = ResolveFluxIndication(theme);
        auto toasts = huxerui::UseEnvironment<huxerui::ToastStyle>();
        toasts.motion = ResolveFluxToastMotion(theme);
        huxerui::ThemeDefinition overrides;
        overrides.Set(icons).Set(chips).Set(tabs).Set(toasts);
        return huxerui::Theme(overrides, FluxToastStack(content));
    });
}

} // namespace clashflux::ui
