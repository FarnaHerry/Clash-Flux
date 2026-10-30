#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace clashflux::network_conflict {
struct Interface {
    unsigned index = 0;
    std::string name;
    bool up = false, tunnel = false;
    std::vector<std::string> addresses; // Numeric CIDRs, IPv4 and IPv6.
    bool capturesIpv4 = false, capturesIpv6 = false;
};
struct Snapshot {
    bool interfacesKnown = false, routesKnown = false;
    std::vector<Interface> interfaces;
};
struct Tun {
    std::string name;
    std::vector<std::string> addresses;
};
struct Request {
    std::vector<Tun> tuns;
    std::vector<unsigned> owned; // Only authenticated running-plan interfaces.
    std::vector<std::string> nativeInterfaces; // Explicit connected native sessions.
    bool proxyListening = true; // Standby accepts no traffic; defaults preserve proxy-only checks.
};
// Read-only OS queries. No packet, DNS request, process probing, shell or mutation.
Snapshot Inspect();
bool Overlaps(const std::string&, const std::string&);
std::vector<unsigned> OwnedInterfaces(const Snapshot&, const std::vector<Tun>&);
// Empty means no confirmed conflict; incomplete inspection never means healthy.
std::string Check(const Snapshot&, const Request&);
} // namespace clashflux::network_conflict
