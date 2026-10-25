#pragma once
#include <huxerui/huxerui.h>
#include <functional>
#include <string>
namespace clashflux::ui {
// Presentation only. The application model owns requests, cancellation and results.
huxerui::View RuntimeDiagnosticsContent(huxerui::State<huxerui::TextEditingValue> target,
    bool busy, bool canRun, std::string report, std::function<void()> start,
    std::function<void()> cancel, std::function<void()> copy, std::function<void()> close,
    std::function<void()> save = {}, bool canSave = false, bool saving = false);
}
