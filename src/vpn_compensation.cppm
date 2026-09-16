// Session-owned routes that keep native VPN transport outside the main TUN.
export module clashflux.vpn_compensation;

import std;

export namespace vpn::compensation {

// These priorities are part of the managed main TUN contract (pref 9000).
inline constexpr int TransportPriority = 8000;
inline constexpr int BoundInterfacePriority = 8100;
inline constexpr int NativeFallbackPriority = 12000;
inline constexpr int FirstTable = 52000;
inline constexpr int LastTable = 52999;

enum class RoutePurpose { Transport, BoundInterface, NativeDestination };

struct RouteTarget {
    RoutePurpose purpose = RoutePurpose::NativeDestination;
    std::string destination;
    std::string interfaceName;
    std::string gateway;
    std::string source;
    inline bool operator==(const RouteTarget&) const = default;
};

struct PhysicalRoute {
    std::string destination;
    std::string interfaceName;
    std::string gateway;
    std::string source;
    unsigned int metric = 0;
    bool physical = true;
};

struct CommandResult {
    int exitCode = -1;
    std::string output;
};

using CommandRunner = std::function<CommandResult(const std::vector<std::string>&)>;
// Copies share ownership; the final owner removes only this session's objects.
using RouteLease = std::shared_ptr<void>;

std::optional<std::string> NormalizeIpv4Cidr(std::string_view value,
                                           bool allowDefault = false);
bool IsSyntheticIpv4(std::string_view address);
std::optional<PhysicalRoute> SelectPhysicalRoute(
    std::string_view address, std::span<const PhysicalRoute> routes);

// The injected executor permits transaction/ownership tests without privileges.
class RouteRegistry {
public:
    explicit RouteRegistry(CommandRunner runner);
    ~RouteRegistry();
    RouteLease acquire(RouteTarget target, std::string& error);
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

struct TransportLease {
    std::string address;
    RouteLease route;
};

// Resolve once and return the numeric address that the dialer must use.
// PPTP and the current native OpenVPN route backend support IPv4 transport.
std::optional<TransportLease> PrepareTransport(std::string_view host,
                                              std::string& error);
RouteLease PrepareBoundInterface(std::string_view interfaceName,
                                 std::string& error);
// Full desired set, with rollback: callers swap only after every acquire succeeds.
bool ReplaceNativeRoutes(std::string_view interfaceName,
                         std::span<const std::string> routes,
                         std::vector<RouteLease>& leases,
                         std::string& error);
bool ReplaceNativeRoutes(RouteRegistry& registry,
                         std::string_view interfaceName,
                         std::span<const std::string> routes,
                         std::vector<RouteLease>& leases,
                         std::string& error);

} // namespace vpn::compensation
