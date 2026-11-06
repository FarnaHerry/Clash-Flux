#pragma once
#include <huxerui/huxerui.h>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace clashflux::ui {
// Navigation belongs to an originating page/action, while the import itself
// belongs to the application. A newer action or explicit back invalidates only
// navigation; completion must still publish committed data and release busy.
struct ProfileImportOrigin {
    std::uint64_t revision = 0;
    bool mounted = true;

    std::uint64_t Advance() { return ++revision; }
    void Leave() { mounted = false; ++revision; }
    bool CanNavigate(std::uint64_t ticket) const {
        return mounted && revision == ticket;
    }
};

inline std::shared_ptr<ProfileImportOrigin> UseProfileImportOrigin() {
    const auto origin = huxerui::UseState(std::make_shared<ProfileImportOrigin>()).Get();
    huxerui::Lifecycle([origin] { return [origin] { origin->Leave(); }; }, 0);
    return origin;
}

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
