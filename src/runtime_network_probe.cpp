#include "runtime_network_probe.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <random>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include "win32_raii.h"
#elif !defined(__ANDROID__) && !defined(CLASHFLUX_IOS)
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#endif
#endif

namespace clashflux::network_probe {
namespace {
unsigned word(const std::vector<std::uint8_t>& bytes, std::size_t at) {
    return (unsigned(bytes[at]) << 8) | bytes[at + 1];
}
bool nameEnd(const std::vector<std::uint8_t>& bytes, std::size_t& at) {
    std::size_t cursor = at, resume = 0;
    for (unsigned steps = 0; steps < 128 && cursor < bytes.size(); ++steps) {
        auto size = bytes[cursor++];
        if (!size) { at = resume ? resume : cursor; return true; }
        if ((size & 0xc0) == 0xc0) {
            if (cursor >= bytes.size()) return false;
            const auto next = std::size_t((size & 0x3f) << 8) | bytes[cursor++];
            if (next >= bytes.size()) return false;
            if (!resume) resume = cursor;
            cursor = next;
        } else {
            if (size > 63 || cursor + size > bytes.size()) return false;
            cursor += size;
        }
    }
    return false;
}
}
std::vector<std::uint8_t> DnsQuestion(std::uint16_t id) {
    std::vector<std::uint8_t> packet{std::uint8_t(id >> 8), std::uint8_t(id), 1, 0, 0, 1, 0, 0, 0, 0, 0, 0};
    for (std::string_view label : {"clash-flux-health", "invalid"}) {
        packet.push_back(static_cast<std::uint8_t>(label.size()));
        packet.insert(packet.end(), label.begin(), label.end());
    }
    packet.insert(packet.end(), {0, 0, 1, 0, 1});
    return packet;
}
Evidence ValidateDns(const std::vector<std::uint8_t>& query, const std::vector<std::uint8_t>& bytes) {
    if (query.size() < 17 || bytes.size() < query.size() || bytes.size() > 4096 ||
        word(query, 0) != word(bytes, 0) || !(bytes[2] & 0x80) || (bytes[2] & 0x7a) ||
        word(bytes, 4) != 1 || !std::equal(query.begin() + 12, query.end(), bytes.begin() + 12))
        return Evidence::InvalidResponse;
    auto at = query.size();
    const unsigned records = word(bytes, 6) + word(bytes, 8) + word(bytes, 10);
    if (records > 256) return Evidence::InvalidResponse;
    for (unsigned i = 0; i < records; ++i) {
        if (!nameEnd(bytes, at) || at + 10 > bytes.size()) return Evidence::InvalidResponse;
        const auto length = word(bytes, at + 8);
        at += 10;
        if (length > bytes.size() - at) return Evidence::InvalidResponse;
        at += length;
    }
    if (at != bytes.size()) return Evidence::InvalidResponse;
    const auto rcode = bytes[3] & 15;
    return rcode == 0 ? Evidence::Response : rcode == 3 ? Evidence::NegativeResponse : Evidence::ServerError;
}

#if !defined(__ANDROID__) && !defined(CLASHFLUX_IOS)
namespace {
#ifdef _WIN32
using SocketValue = SOCKET;
constexpr auto InvalidSocket = INVALID_SOCKET;
struct Socket {
    SocketValue value = InvalidSocket;
    explicit Socket(SocketValue v) : value(v) {}
    ~Socket() { if (value != InvalidSocket) closesocket(value); }
    Socket(const Socket&) = delete;
};
struct Winsock {
    bool ready;
    Winsock() { WSADATA data{}; ready = WSAStartup(MAKEWORD(2,2), &data) == 0; }
    ~Winsock() { if (ready) WSACleanup(); }
};
bool parse(const std::string& value, IN_ADDR& address) { return InetPtonA(AF_INET, value.c_str(), &address) == 1; }
Proxy manualProxy(int port) {
    clashflux::win32::UniqueHkey key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
        0, KEY_QUERY_VALUE, key.put()) != ERROR_SUCCESS) return Proxy::Unknown;
    DWORD enabled=0,size=sizeof(enabled);
    const auto status=RegGetValueW(key.get(),nullptr,L"ProxyEnable",RRF_RT_REG_DWORD,nullptr,&enabled,&size);
    if (status==ERROR_FILE_NOT_FOUND) return Proxy::Disabled;
    if (status!=ERROR_SUCCESS) return Proxy::Unknown;
    if (!enabled) return Proxy::Disabled;
    std::array<wchar_t,4096> server{};size=sizeof(server);
    if (RegGetValueW(key.get(),nullptr,L"ProxyServer",RRF_RT_REG_SZ,nullptr,server.data(),&size)!=ERROR_SUCCESS)
        return Proxy::Unknown;
    if(port<=0||port>65535)return Proxy::Unknown;
    return std::wstring_view(server.data()) == L"127.0.0.1:"+std::to_wstring(port) ? Proxy::Ours : Proxy::Other;
}
unsigned findInterface(const Request& request, Evidence& evidence) {
    std::wstring name(request.interfaceName.begin(), request.interfaceName.end());
    // Interface aliases can be non-ASCII in imported plans.
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, request.interfaceName.data(),
        static_cast<int>(request.interfaceName.size()), nullptr, 0);
    if (size <= 0) return 0;
    name.resize(size);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, request.interfaceName.data(),
        static_cast<int>(request.interfaceName.size()), name.data(), size);
    NET_LUID luid{};
    auto status = ConvertInterfaceAliasToLuid(name.c_str(), &luid);
    if (status != NO_ERROR) { evidence = status == ERROR_INVALID_PARAMETER || status == ERROR_FILE_NOT_FOUND ||
        status == ERROR_NOT_FOUND ? Evidence::Absent : Evidence::Unknown; return 0; }
    MIB_IF_ROW2 row{}; row.InterfaceLuid = luid;
    if (GetIfEntry2(&row) != NO_ERROR) return 0;
    if (row.OperStatus != IfOperStatusUp) { evidence = Evidence::Down; return 0; }
    IN_ADDR expected{};
    if (!parse(request.address, expected)) return 0;
    MIB_UNICASTIPADDRESS_ROW address{}; InitializeUnicastIpAddressEntry(&address);
    address.InterfaceLuid = luid; address.Address.si_family = AF_INET;
    address.Address.Ipv4.sin_addr = expected;
    status = GetUnicastIpAddressEntry(&address);
    if (status != NO_ERROR || address.DadState != IpDadStatePreferred) {
        evidence = status == ERROR_NOT_FOUND ? Evidence::AddressMismatch : Evidence::Unknown; return 0;
    }
    evidence = Evidence::Present; return row.InterfaceIndex;
}
Evidence route(const std::string& destination, unsigned tun) {
    SOCKADDR_INET target{}; target.si_family = AF_INET;
    if (!parse(destination, target.Ipv4.sin_addr)) return Evidence::Unknown;
    MIB_IPFORWARD_ROW2 row{}; SOCKADDR_INET source{};
    if (GetBestRoute2(nullptr, 0, nullptr, &target, 0, &row, &source) != NO_ERROR) return Evidence::Unknown;
    return row.InterfaceIndex == tun ? Evidence::ViaTun : Evidence::OtherInterface;
}
#else
using SocketValue = int;
constexpr int InvalidSocket = -1;
struct Socket {
    int value;
    explicit Socket(int v) : value(v) {}
    ~Socket() { if (value >= 0) close(value); }
    Socket(const Socket&) = delete;
};
bool parse(const std::string& value, in_addr& address) { return inet_pton(AF_INET, value.c_str(), &address) == 1; }
unsigned findInterface(const Request& request, Evidence& evidence) {
    auto index = request.interfaceName.empty()?0:if_nametoindex(request.interfaceName.c_str());
    if(request.interfaceName.empty()) {
        ifaddrs* rows=nullptr;in_addr expected{};
        if(!parse(request.address,expected)||getifaddrs(&rows)!=0)return 0;
        struct Guard{ifaddrs* p;~Guard(){freeifaddrs(p);}} guard{rows};
        for(auto p=rows;p;p=p->ifa_next) {
            if(!p->ifa_addr||p->ifa_addr->sa_family!=AF_INET||
                reinterpret_cast<sockaddr_in*>(p->ifa_addr)->sin_addr.s_addr!=expected.s_addr)continue;
            const auto found=if_nametoindex(p->ifa_name);
            if(index&&index!=found)return 0;
            index=found;
        }
    }
    if (!index) { evidence = Evidence::Absent; return 0; }
    ifaddrs* raw = nullptr;
    if (getifaddrs(&raw) != 0) return 0;
    struct Guard { ifaddrs* p; ~Guard(){ freeifaddrs(p); } } guard{raw};
    in_addr expected{}; if (!parse(request.address, expected)) return 0;
    bool up = false;
    for (auto p = raw; p; p = p->ifa_next) {
        if (if_nametoindex(p->ifa_name)!=index) continue;
        up |= (p->ifa_flags & IFF_UP) != 0;
        if (p->ifa_addr && p->ifa_addr->sa_family == AF_INET && up &&
            reinterpret_cast<sockaddr_in*>(p->ifa_addr)->sin_addr.s_addr == expected.s_addr) {
            evidence = Evidence::Present; return index;
        }
    }
    evidence = up ? Evidence::AddressMismatch : Evidence::Down; return 0;
}
Evidence route(const std::string& destination, unsigned tun) {
#ifdef __linux__
    struct Query { nlmsghdr header; rtmsg message; rtattr attribute; in_addr address; } query{};
    if (!parse(destination, query.address)) return Evidence::Unknown;
    query.header.nlmsg_len = sizeof(query); query.header.nlmsg_type = RTM_GETROUTE;
    query.header.nlmsg_flags = NLM_F_REQUEST; query.header.nlmsg_seq = 1;
    query.message.rtm_family = AF_INET; query.message.rtm_dst_len = 32;
    query.attribute.rta_type = RTA_DST; query.attribute.rta_len = RTA_LENGTH(sizeof(in_addr));
    Socket socket{::socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_ROUTE)};
    if (socket.value < 0) return Evidence::Unknown;
    sockaddr_nl kernel{}; kernel.nl_family = AF_NETLINK;
    if (sendto(socket.value, &query, sizeof(query), 0, reinterpret_cast<sockaddr*>(&kernel), sizeof(kernel)) < 0)
        return Evidence::Unknown;
    pollfd descriptor{socket.value, POLLIN, 0};
    if (poll(&descriptor, 1, 100) <= 0) return Evidence::Unknown;
    alignas(nlmsghdr) std::array<char, 8192> buffer{};
    auto length = recv(socket.value, buffer.data(), buffer.size(), 0);
    if (length <= 0) return Evidence::Unknown;
    int left = static_cast<int>(length);
    for (auto h = reinterpret_cast<nlmsghdr*>(buffer.data()); NLMSG_OK(h,left); h = NLMSG_NEXT(h,left)) {
        if (h->nlmsg_type != RTM_NEWROUTE || h->nlmsg_seq != 1) continue;
        auto data = reinterpret_cast<rtmsg*>(NLMSG_DATA(h)); int size = RTM_PAYLOAD(h);
        for (auto a = RTM_RTA(data); RTA_OK(a,size); a = RTA_NEXT(a,size)) {
            if (a->rta_type == RTA_OIF && RTA_PAYLOAD(a) >= sizeof(unsigned)) {
                unsigned index; std::memcpy(&index, RTA_DATA(a), sizeof(index));
                return index == tun ? Evidence::ViaTun : Evidence::OtherInterface;
            }
        }
    }
    return Evidence::Unknown;
#else
    (void)destination; (void)tun; return Evidence::Unsupported;
#endif
}
#endif
Evidence dns(const Request& request, unsigned index, std::stop_token cancel,
             std::chrono::steady_clock::time_point deadline) {
    if (cancel.stop_requested()) return Evidence::Cancelled;
    Socket socket{::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)};
    if (socket.value == InvalidSocket) return Evidence::Unknown;
    sockaddr_in local{}; local.sin_family = AF_INET;
    if (!parse(request.address, local.sin_addr) ||
        bind(socket.value, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) return Evidence::Unknown;
#ifdef _WIN32
    DWORD networkIndex = htonl(index);
    if (setsockopt(socket.value, IPPROTO_IP, IP_UNICAST_IF, reinterpret_cast<const char*>(&networkIndex),
        sizeof(networkIndex)) != 0) return Evidence::Unknown;
#elif defined(__linux__)
    std::array<char,IF_NAMESIZE> interfaceName{};
    if(!if_indextoname(index,interfaceName.data())||setsockopt(socket.value, SOL_SOCKET, SO_BINDTODEVICE, interfaceName.data(),
        static_cast<socklen_t>(std::strlen(interfaceName.data())+1)) != 0) return Evidence::Unknown;
#else
    if (setsockopt(socket.value, IPPROTO_IP, IP_BOUND_IF, &index, sizeof(index)) != 0) return Evidence::Unknown;
#endif
    sockaddr_in remote{}; remote.sin_family = AF_INET; remote.sin_port = htons(request.dnsPort);
    if (!parse(request.dnsAddress, remote.sin_addr) ||
        connect(socket.value, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) != 0) return Evidence::Unknown;
    const auto question = DnsQuestion(static_cast<std::uint16_t>(std::random_device{}()));
    if (send(socket.value, reinterpret_cast<const char*>(question.data()), static_cast<int>(question.size()), 0) !=
        static_cast<int>(question.size())) return Evidence::Unknown;
    while (std::chrono::steady_clock::now() < deadline && !cancel.stop_requested()) {
#ifdef _WIN32
        fd_set set; FD_ZERO(&set); FD_SET(socket.value, &set); timeval wait{0,50000};
        const auto ready = select(0, &set, nullptr, nullptr, &wait);
#else
        pollfd descriptor{socket.value,POLLIN,0};
        const auto ready = poll(&descriptor,1,50);
#endif
        if (ready < 0) return Evidence::Unknown;
        if (!ready) continue;
        std::vector<std::uint8_t> response(4097);
        const auto size = recv(socket.value, reinterpret_cast<char*>(response.data()), static_cast<int>(response.size()), 0);
        if (size < 0) return Evidence::Unknown;
        response.resize(static_cast<std::size_t>(size));
        return ValidateDns(question, response);
    }
    return cancel.stop_requested() ? Evidence::Cancelled : Evidence::Timeout;
}
}
#endif
Result Inspect(const Request& request, std::stop_token cancel, std::chrono::milliseconds budget) {
    Result result;
#if defined(__ANDROID__) || defined(CLASHFLUX_IOS)
    (void)request; (void)cancel; (void)budget;
    result.interface = result.route = result.dnsRoute = result.dns = Evidence::Unsupported;
    result.proxy = Proxy::Unsupported;
#else
#ifdef _WIN32
    Winsock winsock; if (!winsock.ready) return result;
    result.proxy = manualProxy(request.mixedPort);
#else
    result.proxy = Proxy::Unsupported;
#endif
    const auto deadline = std::chrono::steady_clock::now() + std::clamp(budget,
        std::chrono::milliseconds(0), std::chrono::milliseconds(1200));
    if (cancel.stop_requested()) { result.interface = Evidence::Cancelled; return result; }
    if (budget.count() <= 0 || request.address.empty() || request.interfaceName.size() > 256) return result;
    const auto index = findInterface(request, result.interface);
    if (!index) return result;
    result.route = route(request.routeDestination, index);
    if (!request.dnsAddress.empty()) result.dnsRoute = route(request.dnsAddress, index);
    // Never send a diagnostic DNS packet to the physical network when TUN
    // capture disappeared or cannot be established. No fallback resolver.
    if (request.dnsRoundTrip && result.dnsRoute == Evidence::ViaTun)
        result.dns = dns(request, index, cancel, deadline);
#endif
    return result;
}
} // namespace clashflux::network_probe
