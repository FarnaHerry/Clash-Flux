#pragma once
#include <cstdint>

namespace clashflux::runtime_plan {
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
}
} // namespace clashflux::runtime_plan
