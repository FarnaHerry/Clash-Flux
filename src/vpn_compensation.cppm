// Session-owned routes that keep native VPN transport outside the main TUN.
export module clashflux.vpn_compensation;

import std;

export namespace vpn::compensation {

// Best-effort cleanup for process/service shutdown. It only touches the
// application's reserved policy priority and protocol-marked tables.
void CleanupManagedLinuxRoutes(std::string& error);

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

// Windows has one routing table. A shared lease must distinguish an entry we
// created from an identical entry borrowed from the system or another app.
struct WindowsRoute {
    std::uint64_t interfaceLuid = 0;
    std::string destination;
    std::string gateway;
    unsigned int metric = 0;
    inline bool operator==(const WindowsRoute&) const = default;
};
enum class RouteCreation { Failed, Created, Borrowed };
struct WindowsRouteOperations {
    std::function<RouteCreation(const WindowsRoute&, std::string&)> create;
    std::function<void(const WindowsRoute&)> remove;
};
class WindowsRouteRegistry {
public:
    explicit WindowsRouteRegistry(WindowsRouteOperations operations);
    ~WindowsRouteRegistry();
    RouteLease acquire(WindowsRoute route, std::string& error);
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
std::optional<std::vector<std::string>> WindowsCaptureDestinations(
    std::span<const std::string> destinations, std::span<const std::string> exclusions,
    std::string& error);

#ifdef _WIN32
bool ReplaceWindowsNativeRoutes(std::uint64_t interfaceLuid,
                                std::span<const std::string> routes,
                                std::vector<RouteLease>& leases, std::string& error);
// The main core owns this lease, not the RAS session. Release before stopping
// TUN: interface-bound fallback defaults must never outlive the capture device.
RouteLease PrepareWindowsTun(std::string_view tunInterface,
                            std::span<const std::string> nativeInterfaces,
                            std::span<const std::string> exclusions,
                            std::string& error);
#endif

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
#ifdef _WIN32
std::optional<TransportLease> PrepareWindowsTransport(std::string_view host,
                                                     std::string& error);
#endif

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
