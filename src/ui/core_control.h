// Include after core_model.h (which requires the store module).
#pragma once

#include <optional>

#include "core_model.h"

namespace clashflux::ui {

// Owning presentation handles are captured during composition. All work and
// result publication belong to the application, even when its caller unmounts.
struct CoreControl {
    std::shared_ptr<CoreModel> model;
    huxerui::TaskScope tasks;
    std::optional<FluxToastHandle> toast;
    std::optional<huxerui::DialogHandle> dialog;
    std::optional<huxerui::WindowHandle> window;
    std::shared_ptr<huxerui::Clipboard> clipboard;
    huxerui::Color textColor;
    huxerui::Color hintColor;
    // Tray actions may outlive the window that supplied presentation handles.
    std::function<bool()> presentationAvailable;
    std::function<void(bool, const std::string&)> onHeadlessFailure;

    bool Busy() const {
        return !model->view.Get().ready || model->operations.Busy() ||
               model->serviceChecking.Get();
    }
    bool TunEnabled() const {
        const auto& command = model->operations.command.Get();
        return command.action == CoreAction::Tun ? command.enabled
                                                : model->view.Get().tunIntent;
    }
    bool VpnEnabled() const {
        const auto& command = model->operations.command.Get();
        if (command.action == CoreAction::Tun) return command.enabled;
        const auto& view = model->view.Get();
        return view.tunIntent || view.vpnState == 1 || view.vpnState == 2;
    }
    bool SystemProxyEnabled() const {
        const auto& command = model->operations.command.Get();
        return command.action == CoreAction::SystemProxy ? command.enabled
            : model->view.Get().systemProxyIntent;
    }
    std::string Mode() const {
        const auto& command = model->operations.command.Get();
        return command.action == CoreAction::Mode ? command.mode
                                                 : model->view.Get().modeIntent;
    }
    bool Running() const {
        const auto action = model->operations.command.Get().action;
        if (action == CoreAction::Start || action == CoreAction::Restart) return true;
        if (action == CoreAction::Stop) return false;
        return model->view.Get().core.state == core::CoreState::Running;
    }
    bool ProxyActive() const { return model->view.Get().ProxyActive(); }
    // Reuse presentation motions without confusing connection commands with core lifetime.
    CoreAction ConnectionAction() const {
        const auto action = model->operations.command.Get().action;
        if (action == CoreAction::Connect) return CoreAction::Start;
        if (action == CoreAction::Disconnect || action == CoreAction::Stop) return CoreAction::Stop;
        return action == CoreAction::Start ? CoreAction::Idle : action;
    }
    void Run(CoreCommand command) const;
};

inline CoreControl UseCoreControl() {
    const auto& theme = huxerui::UseTheme();
    return {huxerui::UseService<CoreModel>(), huxerui::UseApplicationTaskScope(),
            UseFluxToast(), huxerui::UseDialog(), huxerui::UseWindow(),
            huxerui::UseApplication().Clipboard(), theme.colors.on_surface,
            theme.colors.on_surface_variant};
}

} // namespace clashflux::ui
