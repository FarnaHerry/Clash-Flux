#pragma once
#include <string>
#include <utility>

namespace clashflux::native_routes {
// Keep the accepted capture lease until BOTH capture preparation and the PPP
// route replacement succeed. A false return or exception releases only the
// candidate; its shared OS routes cannot remove those still owned by current.
template<class Lease, class Prepare, class Apply>
bool Replace(Lease& current, Prepare&& prepare, Apply&& apply, std::string& error) {
    Lease candidate;
    if (!prepare(candidate, error) || !apply(error)) return false;
    current = std::move(candidate);
    return true;
}

// A rejected candidate does not authorize stopping an unrelated running plan.
// If the accepted plan still uses the interface, release requires either a
// successful withdrawal or confirmed core stop. Callers keep the session on failure.
template<class Publish, class References, class Stop>
bool Withdraw(Publish&& publish, References&& references, Stop&& stop) {
    if (publish()) return true;
    return !references() || stop();
}
} // namespace clashflux::native_routes
