#pragma once
#include <cstdint>

namespace clashflux::runtime_plan {
// Standby is platform control state, independent of the saved main source.
template<class Snapshot>
bool Standby(const Snapshot& snapshot) {
    return snapshot.state == decltype(snapshot.state)::Running && !snapshot.proxyListening;
}
template<class Snapshot>
bool Standby(const Snapshot& snapshot, int vpnState, bool tunRequested) {
    // Android core-only retains local listeners. Require live Binder evidence
    // and no pending VPN intent; VPN authorization/start must remain protected.
    return snapshot.state == decltype(snapshot.state)::Running && snapshot.binderAvailable &&
           vpnState == 0 && !tunRequested;
}
// Shared publication boundary for the accepted running plan. Candidate
// compilation never calls Ready, and Stopped never changes saved user intent.
template<class Snapshot>
void Ready(Snapshot& snapshot, bool acceptedTun, std::uint64_t revision) {
    snapshot.tunEnabled=acceptedTun;
    snapshot.planRevision=revision;
    ++snapshot.runtimeEpoch;
}
template<class Snapshot>
void Stopped(Snapshot& snapshot) {
    snapshot.tunEnabled=false;
    if constexpr (requires { snapshot.proxyListening; snapshot.connectedSince; }) {
        snapshot.proxyListening=false;
        snapshot.connectedSince={};
    }
}
} // namespace clashflux::runtime_plan
