// Windows IP Helper backend. All routes are temporary and owned by leases.
module;
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <windns.h>
#include "win32_raii.h"
#endif

module clashflux.vpn_compensation;
import std;
#ifdef _WIN32
import nlohmann.json;
#endif

#ifdef _WIN32
namespace vpn::compensation {
namespace {

// Windows 路由操作由系统服务执行；ACCESS_DENIED 表示服务操作被系统拒绝，
// 不能引导用户提权 GUI。
std::string winError(std::string_view operation, std::string_view detail,
                     ULONG status) {
    std::string_view note;
    switch (status) {
    case ERROR_ACCESS_DENIED:
        note = "，系统拒绝访问";
        break;
    case ERROR_OBJECT_ALREADY_EXISTS:
        note = "，系统已存在相同接口/网段/下一跳的路由";
        break;
    case ERROR_INVALID_PARAMETER:
        note = "，接口或网关参数无效";
        break;
    case ERROR_NOT_FOUND:
        note = "，接口、网关或路由不存在";
        break;
    case ERROR_NOT_SUPPORTED:
        note = "，系统不支持该路由操作";
        break;
    default:
        break;
    }
    if (detail.empty()) {
        return std::format("{}（Windows 错误码 {}{}）", operation, status, note);
    }
    return std::format("{}（{}，Windows 错误码 {}{}）", operation, detail, status,
                       note);
}

std::string addressText(const IN_ADDR& address) {
    char text[INET_ADDRSTRLEN]{};
    return InetNtopA(AF_INET, &address, text, sizeof(text)) ? text : "";
}

MIB_IPFORWARD_ROW2 nativeRow(const WindowsRoute& route) {
    MIB_IPFORWARD_ROW2 row;
    InitializeIpForwardEntry(&row);
    row.InterfaceLuid.Value = route.interfaceLuid;
    row.DestinationPrefix.Prefix.si_family = AF_INET;
    const auto slash = route.destination.find('/');
    const auto address = route.destination.substr(0, slash);
    InetPtonA(AF_INET, address.c_str(), &row.DestinationPrefix.Prefix.Ipv4.sin_addr);
    unsigned int prefix = 0;
    std::from_chars(route.destination.data() + slash + 1,
                    route.destination.data() + route.destination.size(), prefix);
    row.DestinationPrefix.PrefixLength = static_cast<UINT8>(prefix);
    row.NextHop.si_family = AF_INET;
    if (!route.gateway.empty())
        InetPtonA(AF_INET, route.gateway.c_str(), &row.NextHop.Ipv4.sin_addr);
    row.Metric = route.metric;
    row.Protocol = static_cast<NL_ROUTE_PROTOCOL>(MIB_IPPROTO_NETMGMT);
    return row;
}

WindowsRouteRegistry& registry() {
    static WindowsRouteRegistry value({
        [](const WindowsRoute& route, std::string& error) {
            auto row = nativeRow(route);
            const auto status = CreateIpForwardEntry2(&row);
            if (status == NO_ERROR) return RouteCreation::Created;
            if (status == ERROR_OBJECT_ALREADY_EXISTS) {
                // 同接口/网段/下一跳的路由已经存在（常见来源：上次进程异常退出
                // 留下的路由，或系统/上一次连接自己装的），只是 metric 不同。
                // 目标「该网段走这个接口」已经达成，直接借用即可：不改写别人的
                // metric，释放时 remove 也只会删自己建的那条。旧实现把它当成
                // 失败并报「需要管理员权限」，管理员身份下也无法连接 PPTP。
                auto existing = row;
                if (GetIpForwardEntry2(&existing) == NO_ERROR)
                    return RouteCreation::Borrowed;
            }
            error = winError("安装 Windows 补偿路由失败", route.destination, status);
            return RouteCreation::Failed;
        },
        [](const WindowsRoute& route) {
            auto row = nativeRow(route);
            if (GetIpForwardEntry2(&row) == NO_ERROR && row.Metric == route.metric &&
                row.Protocol == MIB_IPPROTO_NETMGMT) DeleteIpForwardEntry2(&row);
        }});
    return value;
}

std::optional<std::vector<MIB_IPFORWARD_ROW2>> routeTable(std::string& error) {
    MIB_IPFORWARD_TABLE2* table = nullptr;
    const auto status = GetIpForwardTable2(AF_INET, &table);
    if (status != NO_ERROR) { error = winError("读取 IPv4 路由表失败", {}, status); return {}; }
    const std::unique_ptr<MIB_IPFORWARD_TABLE2, decltype(&FreeMibTable)> owner(table, &FreeMibTable);
    std::vector<MIB_IPFORWARD_ROW2> rows(table->Table, table->Table + table->NumEntries);
    return rows;
}

std::optional<NET_LUID> interfaceLuid(std::string_view name) {
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(),
                                         static_cast<int>(name.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring alias(count, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(),
                            static_cast<int>(name.size()), alias.data(), count)) return {};
    NET_LUID luid{};
    if (ConvertInterfaceAliasToLuid(alias.c_str(), &luid) != NO_ERROR) return {};
    return luid;
}

bool physical(NET_LUID luid) {
    MIB_IF_ROW2 row{};
    row.InterfaceLuid = luid;
    return GetIfEntry2(&row) == NO_ERROR && row.OperStatus == IfOperStatusUp &&
           row.InterfaceAndOperStatusFlags.HardwareInterface &&
           row.Type != IF_TYPE_PPP && row.Type != IF_TYPE_TUNNEL &&
           row.Type != IF_TYPE_SOFTWARE_LOOPBACK;
}

std::vector<PhysicalRoute> physicalRoutes(const std::vector<MIB_IPFORWARD_ROW2>& table) {
    std::vector<PhysicalRoute> result;
    for (const auto& row : table) {
        if (!physical(row.InterfaceLuid)) continue;
        MIB_IPINTERFACE_ROW iface{};
        InitializeIpInterfaceEntry(&iface);
        iface.Family = AF_INET;
        iface.InterfaceLuid = row.InterfaceLuid;
        if (GetIpInterfaceEntry(&iface) != NO_ERROR) continue;
        const auto metric = static_cast<std::uint64_t>(row.Metric) + iface.Metric;
        result.push_back({
            std::format("{}/{}", addressText(row.DestinationPrefix.Prefix.Ipv4.sin_addr),
                         row.DestinationPrefix.PrefixLength),
            std::to_string(row.InterfaceIndex),
            row.NextHop.Ipv4.sin_addr.s_addr ? addressText(row.NextHop.Ipv4.sin_addr) : "",
            {}, static_cast<unsigned int>(std::min<std::uint64_t>(metric, UINT_MAX)), true});
    }
    return result;
}

RouteLease physicalLease(std::string_view address, const std::vector<PhysicalRoute>& routes,
                         std::string& error) {
    const auto selected = SelectPhysicalRoute(address, routes);
    if (!selected) { error = "VPN 服务器/DNS 没有可用的物理网卡 IPv4 路由"; return {}; }
    ULONG index = 0;
    std::from_chars(selected->interfaceName.data(),
                    selected->interfaceName.data() + selected->interfaceName.size(), index);
    NET_LUID luid{};
    if (ConvertInterfaceIndexToLuid(index, &luid) != NO_ERROR) {
        error = "物理网卡已消失";
        return {};
    }
    auto lease = registry().acquire({luid.Value, std::string(address) + "/32", selected->gateway, 0}, error);
    if (!lease) return {};
    SOCKADDR_INET destination{};
    destination.si_family = AF_INET;
    InetPtonA(AF_INET, std::string(address).c_str(), &destination.Ipv4.sin_addr);
    MIB_IPFORWARD_ROW2 best{};
    SOCKADDR_INET source{};
    if (GetBestRoute2(nullptr, 0, nullptr, &destination, 0, &best, &source) != NO_ERROR ||
        best.InterfaceLuid.Value != luid.Value) {
        error = "VPN 服务器路由仍被其它接口捕获；保留主 TUN 并拒绝拨号";
        return {};
    }
    return lease;
}

std::vector<std::string> physicalDns() {
    ULONG size = 16384;
    std::vector<unsigned char> buffer(size);
    ULONG status;
    for (int attempt = 0; attempt < 3; ++attempt) {
        status = GetAdaptersAddresses(AF_INET, 0, nullptr,
            reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
        if (status != ERROR_BUFFER_OVERFLOW) break;
        buffer.resize(size);
    }
    if (status != NO_ERROR) return {};
    std::vector<std::string> result;
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
         adapter; adapter = adapter->Next) {
        if (!physical(adapter->Luid)) continue;
        for (auto* server = adapter->FirstDnsServerAddress; server; server = server->Next) {
            if (!server->Address.lpSockaddr || server->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto address = addressText(reinterpret_cast<sockaddr_in*>(server->Address.lpSockaddr)->sin_addr);
            if (std::ranges::find(result, address) == result.end()) result.push_back(address);
        }
    }
    return result;
}

#include "windows_physical_dns.inc"
#include "windows_native_dns_tool.inc"

// Destruction joins the watcher first, then removes the interface defaults,
// then the TUN captures. A crashed core also releases these routes promptly.
struct TunLease {
    std::vector<RouteLease> captures;
    std::vector<RouteLease> defaults;
    std::jthread watcher;
};

} // namespace

std::optional<TransportLease> PrepareWindowsTransport(std::string_view host, std::string& error) {
    const auto table = routeTable(error);
    if (!table) return {};
    const auto routes = physicalRoutes(*table);
    std::vector<std::string> addresses;
    if (const auto ip = NormalizeIpv4Cidr(host); ip && ip->ends_with("/32")) {
        addresses.emplace_back(host);
    } else {
        // Use the same official executable identity allowed by strict TUN,
        // bound to the selected physical interface. Never alter global DNS.
        const auto deadline = GetTickCount64() + 8000;
        for (const auto& server : physicalDns()) {
            if (GetTickCount64() >= deadline) break;
            const auto route = SelectPhysicalRoute(server, routes);
            if (!route) continue;
            ULONG index = 0;
            std::from_chars(route->interfaceName.data(),
                route->interfaceName.data() + route->interfaceName.size(), index);
            for (auto& address : queryNativeDnsTool(host, server, index, error))
                if (SelectPhysicalRoute(address, routes)) addresses.push_back(std::move(address));
            if (!addresses.empty()) break;
        }
    }
    for (const auto& address : addresses) {
        if (auto route = physicalLease(address, routes, error)) {
            error.clear();
            return TransportLease{address, std::move(route)};
        }
    }
    if (error.empty()) error = "无法解析真实 VPN IPv4 地址；请检查物理网卡 DNS 或使用服务器 IPv4 地址";
    return {};
}

bool ReplaceWindowsNativeRoutes(std::uint64_t interfaceLuid,
                                std::span<const std::string> routes,
                                std::vector<RouteLease>& leases, std::string& error) {
    error.clear();
    std::vector<RouteLease> replacement;
    for (const auto& route : routes) {
        const auto normalized = NormalizeIpv4Cidr(route);
        if (!normalized) { error = "PPTP 内网必须是具体 IPv4 地址或 /1–/32 网段"; return false; }
        auto lease = registry().acquire({interfaceLuid, *normalized, {}, 1}, error);
        if (!lease) return false;
        replacement.push_back(std::move(lease));
    }
    leases.swap(replacement);
    return true;
}

bool WindowsTunActive(std::string_view tunInterface) {
    const auto tun = interfaceLuid(tunInterface);
    std::string error;
    const auto table = routeTable(error);
    if (!tun || !table) return false;
    MIB_IF_ROW2 info{}; info.InterfaceLuid = *tun;
    return GetIfEntry2(&info) == NO_ERROR && info.OperStatus == IfOperStatusUp &&
        std::ranges::any_of(*table, [&](const auto& row) {
            return row.InterfaceLuid.Value == tun->Value && row.Metric == 0 &&
                row.NextHop.Ipv4.sin_addr.s_addr != 0;
        });
}

RouteLease PrepareWindowsTun(std::string_view tunInterface,
                             std::span<const std::string> nativeInterfaces,
                             std::span<const std::string> exclusions, std::string& error,
                             std::span<const std::string> prospectiveRoutes) {
    error.clear();
    const auto tun = interfaceLuid(tunInterface);
    const auto table = routeTable(error);
    if (!tun || !table) {
        if (error.empty()) error = "未找到主 TUN 网卡";
        return {};
    }
    MIB_IPINTERFACE_ROW tunInfo{};
    InitializeIpInterfaceEntry(&tunInfo);
    tunInfo.Family = AF_INET;
    tunInfo.InterfaceLuid = *tun;
    if (GetIpInterfaceEntry(&tunInfo) != NO_ERROR || tunInfo.Metric != 0) {
        error = "主 TUN 必须使用 sing-box auto_route 的零接口 metric";
        return {};
    }
    const auto gateway = std::ranges::find_if(*table, [&](const auto& row) {
        return row.InterfaceLuid.Value == tun->Value && row.NextHop.Ipv4.sin_addr.s_addr != 0 && row.Metric == 0;
    });
    if (gateway == table->end()) { error = "主 TUN 尚未建立全流量路由"; return {}; }
    std::vector<std::string> effectiveExclusions(exclusions.begin(), exclusions.end());
    MIB_UNICASTIPADDRESS_TABLE* localAddresses = nullptr;
    const auto addressStatus = GetUnicastIpAddressTable(AF_INET, &localAddresses);
    if (addressStatus != NO_ERROR) { error = winError("读取本机 IPv4 地址失败", {}, addressStatus); return {}; }
    const std::unique_ptr<MIB_UNICASTIPADDRESS_TABLE, decltype(&FreeMibTable)> addressesOwner(localAddresses, &FreeMibTable);
    for (ULONG index = 0; index < localAddresses->NumEntries; ++index)
        effectiveExclusions.push_back(addressText(localAddresses->Table[index].Address.Ipv4.sin_addr) + "/32");
    auto lease = std::make_shared<TunLease>();
    for (const auto& name : nativeInterfaces) {
        const auto native = interfaceLuid(name);
        if (!native || native->Value == tun->Value) { error = "原生 VPN 网卡无效"; return {}; }
        MIB_IF_ROW2 info{};
        info.InterfaceLuid = *native;
        if (GetIfEntry2(&info) != NO_ERROR || info.Type != IF_TYPE_PPP || info.OperStatus != IfOperStatusUp) {
            error = "Windows TUN 补偿当前仅支持活动的 PPTP/PPP 接口";
            return {};
        }
        MIB_IPINTERFACE_ROW nativeInfo{};
        InitializeIpInterfaceEntry(&nativeInfo);
        nativeInfo.Family = AF_INET;
        nativeInfo.InterfaceLuid = *native;
        if (GetIpInterfaceEntry(&nativeInfo) != NO_ERROR) { error = "读取 PPP 接口 metric 失败"; return {}; }
        std::vector<std::string> destinations(prospectiveRoutes.begin(), prospectiveRoutes.end());
        // Add exact matching routes on TUN. Putting these CIDRs in route_address
        // is insufficient: sing-tun merges adjacent/overlapping ranges again.
        for (const auto& row : *table) {
            if (row.InterfaceLuid.Value != native->Value || row.DestinationPrefix.PrefixLength == 0) continue;
            if (row.Metric == 0 && nativeInfo.Metric == 0) {
                error = "PPP 路由 metric 为零，无法保证主 TUN 优先级";
                return {};
            }
            if ((ntohl(row.DestinationPrefix.Prefix.Ipv4.sin_addr.s_addr) >> 28) >= 14) continue;
            MIB_UNICASTIPADDRESS_ROW local{};
            local.InterfaceLuid = *native;
            local.Address = row.DestinationPrefix.Prefix;
            if (row.DestinationPrefix.PrefixLength == 32 && GetUnicastIpAddressEntry(&local) == NO_ERROR) continue;
            const auto destination = std::format("{}/{}", addressText(row.DestinationPrefix.Prefix.Ipv4.sin_addr),
                                                  row.DestinationPrefix.PrefixLength);
            destinations.push_back(destination);
            // A physical LAN or another VPN may have a more specific route
            // inside this network. Capture that prefix too, so it cannot bypass
            // the user's explicit sing-box policy by longest-prefix selection.
            const auto bits = row.DestinationPrefix.PrefixLength;
            const auto mask = 0xffffffffU << (32 - bits);
            const auto network = ntohl(row.DestinationPrefix.Prefix.Ipv4.sin_addr.s_addr) & mask;
            for (const auto& other : *table) {
                if (other.InterfaceLuid.Value == tun->Value || other.DestinationPrefix.PrefixLength <= bits) continue;
                if ((ntohl(other.DestinationPrefix.Prefix.Ipv4.sin_addr.s_addr) & mask) != network) continue;
                destinations.push_back(std::format("{}/{}", addressText(other.DestinationPrefix.Prefix.Ipv4.sin_addr),
                                                    other.DestinationPrefix.PrefixLength));
            }
        }
        const auto captureRoutes = WindowsCaptureDestinations(destinations, effectiveExclusions, error);
        if (!captureRoutes) return {};
        for (const auto& destination : *captureRoutes) {
            auto capture = registry().acquire({tun->Value, destination,
                addressText(gateway->NextHop.Ipv4.sin_addr), 0}, error);
            if (!capture) return {};
            lease->captures.push_back(std::move(capture));
        }
        // IP_UNICAST_IF (sing-box bind_interface) needs an interface route even
        // for domain rules outside configured CIDRs. This costly /0 is created
        // only while TUN's /1 routes capture ordinary traffic.
        auto fallback = registry().acquire({native->Value, "0.0.0.0/0", {}, 60000}, error);
        if (!fallback) return {};
        lease->defaults.push_back(std::move(fallback));
    }
    auto* state = lease.get();
    lease->watcher = std::jthread([state, luid = *tun, sentinel = *gateway](std::stop_token stop) mutable {
        std::mutex mutex;
        std::condition_variable_any wake;
        std::unique_lock lock(mutex);
        while (!stop.stop_requested()) {
            MIB_IF_ROW2 row{};
            row.InterfaceLuid = luid;
            if (GetIfEntry2(&row) != NO_ERROR || row.OperStatus != IfOperStatusUp ||
                GetIpForwardEntry2(&sentinel) != NO_ERROR) {
                state->defaults.clear();
                state->captures.clear();
                return;
            }
            wake.wait_for(lock, stop, std::chrono::milliseconds(200), [] { return false; });
        }
    });
    return lease;
}

} // namespace vpn::compensation
#endif
