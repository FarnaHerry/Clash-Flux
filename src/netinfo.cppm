// netinfo.cppm — 本机当前网络接口信息（首页「内网 IP」卡片的数据源）。
//
// 返回默认路由使用的活动、非 loopback IPv4；无默认路由时回退第一个活动
// 非 loopback 地址。不发送数据包、不写盘，但按约定仍由调用方放到任务线程。
module;

#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

export module clashflux.netinfo;

import std;

namespace netinfo {

export struct LanAddress {
    std::string iface;
    std::string address;

    bool operator==(const LanAddress&) const = default;
};

#if defined(_WIN32)

// FriendlyName 是宽字符（如「以太网」），转成 UTF-8 供界面显示。
std::string narrowAdapterName(const WCHAR* wide) {
    if (wide == nullptr || *wide == L'\0') return {};
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return {};
    std::string narrow(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow.data(), length, nullptr,
                        nullptr);
    narrow.pop_back();
    return narrow;
}

#endif

// 当前用于外发流量的本机 IPv4 与接口名。优先读取系统默认路由选中的接口；
// 无默认路由时退回第一个活动、非 loopback IPv4。失败或没有地址时返回空，
// 调用方据此显示「未检测到内网地址」。
export std::optional<LanAddress> activeLanIpv4Address() {
#if defined(_WIN32)
    std::optional<LanAddress> fallback;
    ULONG fallbackMetric = std::numeric_limits<ULONG>::max();
    DWORD bestIndex = 0;
    IN_ADDR target{};
    const bool hasBest = InetPtonA(AF_INET, "1.1.1.1", &target) == 1 &&
        GetBestInterface(target.S_un.S_addr, &bestIndex) == NO_ERROR;

    ULONG size = 0;
    constexpr ULONG kFlags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                             GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, kFlags, nullptr, nullptr, &size) !=
            ERROR_BUFFER_OVERFLOW ||
        size == 0) {
        return std::nullopt;
    }
    std::vector<char> buffer(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_INET, kFlags, nullptr, adapters, &size) !=
        NO_ERROR) {
        return std::nullopt;
    }
    for (const IP_ADAPTER_ADDRESSES* adapter = adapters; adapter != nullptr;
         adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp ||
            adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        for (const IP_ADAPTER_UNICAST_ADDRESS* unicast =
                 adapter->FirstUnicastAddress;
             unicast != nullptr; unicast = unicast->Next) {
            const SOCKET_ADDRESS& socket = unicast->Address;
            if (socket.lpSockaddr == nullptr ||
                socket.lpSockaddr->sa_family != AF_INET) {
                continue;
            }
            char text[INET_ADDRSTRLEN]{};
            const auto* address =
                reinterpret_cast<const sockaddr_in*>(socket.lpSockaddr);
            if (InetNtopA(AF_INET, &address->sin_addr, text, sizeof(text)) ==
                nullptr) {
                continue;
            }
            LanAddress candidate{narrowAdapterName(adapter->FriendlyName), text};
            if (hasBest && adapter->IfIndex == bestIndex) return candidate;
            if (!fallback || adapter->Ipv4Metric < fallbackMetric) {
                fallbackMetric = adapter->Ipv4Metric;
                fallback = std::move(candidate);
            }
        }
    }
    return fallback;
#else
    std::optional<std::string> routeSource;
    const int descriptor = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (descriptor >= 0) {
        struct SocketGuard {
            int descriptor;
            ~SocketGuard() { ::close(descriptor); }
        } guard{descriptor};
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(53);
        if (inet_pton(AF_INET, "1.1.1.1", &target.sin_addr) == 1 &&
            ::connect(descriptor, reinterpret_cast<const sockaddr*>(&target),
                      sizeof(target)) == 0) {
            sockaddr_in local{};
            socklen_t length = sizeof(local);
            char text[INET_ADDRSTRLEN]{};
            if (getsockname(descriptor, reinterpret_cast<sockaddr*>(&local),
                            &length) == 0 &&
                inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text)) != nullptr)
                routeSource = text;
        }
    }

    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0 || list == nullptr) return std::nullopt;
    // getifaddrs 的链表必须整体 freeifaddrs，中途 continue 不能漏。
    struct IfaddrsGuard {
        ifaddrs* list;
        ~IfaddrsGuard() { freeifaddrs(list); }
    } guard{list};
    std::optional<LanAddress> fallback;
    for (const ifaddrs* entry = list; entry != nullptr;
         entry = entry->ifa_next) {
        if (entry->ifa_addr == nullptr ||
            entry->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if ((entry->ifa_flags & IFF_UP) == 0 ||
            (entry->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }
        char text[INET_ADDRSTRLEN]{};
        const auto* address =
            reinterpret_cast<const sockaddr_in*>(entry->ifa_addr);
        if (inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text)) ==
            nullptr) {
            continue;
        }
        LanAddress candidate{entry->ifa_name, text};
        if (routeSource && candidate.address == *routeSource) return candidate;
        if (!fallback) fallback = std::move(candidate);
    }
    return fallback;
#endif
}

} // namespace netinfo
