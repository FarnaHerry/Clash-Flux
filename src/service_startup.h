// Shared startup policy. All callbacks perform blocking service work on a worker.
#pragma once
#include <string>

namespace clashflux::service_startup {
// Replace a service-owned child only after its stop callback confirms exit
// and releases the PID. Failed stops retain the old identity for a retry.
template <class Pid, class Stop, class Spawn>
bool restartChild(Pid& child, Stop stop, Spawn spawn, std::string& error) {
    error.clear();
    if (child > 0 && (!stop(child, error) || child > 0)) {
        if (error.empty()) error = "旧内核未确认退出，拒绝启动新内核";
        return false;
    }
    const auto replacement = spawn(error);
    if (replacement <= 0) {
        if (error.empty()) error = "新内核进程启动失败";
        return false;
    }
    child = replacement;
    return true;
}

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
