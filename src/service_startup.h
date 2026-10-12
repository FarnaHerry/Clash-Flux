// Shared startup policy. All callbacks perform blocking service work on a worker.
#pragma once
#include <string>

namespace clashflux::service_startup {
struct Result {
    bool installed = false;
    bool ready = true; // An absent optional service does not need maintenance.
    bool updated = false;
    std::string error;
};

template <class Installed, class Query, class Ensure>
Result synchronize(Installed installed, Query query, Ensure ensure) {
    Result result;
    result.installed = installed();
    if (!result.installed) return result; // Never install on application startup.
    const auto before = query();
    if (before.compatible()) return result;

    // Both backends upgrade through their authenticated daemon. Windows can
    // also start an installed stopped service without elevating the GUI.
    result.ready = ensure(result.error);
    if (result.ready) {
        result.updated = before.reachable;
        return result;
    }
    return result;
}
} // namespace clashflux::service_startup
