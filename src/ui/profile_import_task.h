#pragma once
#include <huxerui/huxerui.h>
#include <cstdint>
#include <exception>
#include <functional>
#include <string>
#include <utility>

namespace clashflux::ui {
// Import commits application data. Retiring/rebuilding the originating page
// must not discard the worker's result and leave its retained busy cell set.
// Blocking work remains on the task thread; completion runs on the UI thread.
template<class Operation, class Complete>
void LaunchProfileImport(huxerui::State<bool> busy, Operation operation, Complete complete) {
    if (busy.Get()) return;
    busy = true;
    try {
        huxerui::UseApplicationTaskScope().Launch(
            [busy, operation = std::move(operation), complete]() mutable -> huxerui::Task<void> {
                std::pair<std::int64_t, std::string> result;
                try { result = co_await std::invoke(operation); }
                catch (const std::exception& error) { result = {0, error.what()}; }
                catch (...) { result = {0, "订阅导入发生未知异常"}; }
                busy = false;
                std::invoke(complete, std::move(result));
            });
    } catch (const std::exception& error) {
        busy = false;
        std::invoke(complete, std::pair<std::int64_t, std::string>{0, error.what()});
    }
}
} // namespace clashflux::ui
