#pragma once
#include <huxerui/huxerui.h>

namespace clashflux::ui {

// Page-owned task control, shared by camera and album actions. Cancelling the
// camera also cancels its current decode before a selected image can publish.
struct QrScannerControl {
    huxerui::State<bool> scanning{false}, cameraActive{false}, selecting{false};
    huxerui::TaskHandle cameraTask;

    bool StartCamera() {
        if (scanning.Get() || selecting.Get()) return false;
        scanning = true;
        return true;
    }
    void StopCamera() {
        cameraTask.Cancel();
        scanning = false;
        cameraActive = false;
    }
    bool BeginSelection() {
        if (selecting.Get()) return false;
        StopCamera();
        selecting = true;
        return true;
    }
    void EndSelection() { selecting = false; }
};

} // namespace clashflux::ui
