// netinfo.cppm — 本机网络接口信息（首页「内网 IP」卡片的数据源）。
//
// 只枚举活动、非 loopback 接口的 IPv4 地址；不联网、不写盘，调用是便宜的
// 系统调用，但按约定仍由调用方放到任务线程执行。
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
    std::string narrow(static_cast<std::size_t>(length - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow.data(), length, nullptr,
                        nullptr);
    return narrow;
}

#endif

// 活动、非 loopback 接口的 IPv4 地址（每接口一条）。失败时返回空表，
// 调用方据此显示「未检测到内网地址」。
export std::vector<LanAddress> lanIpv4Addresses() {
    std::vector<LanAddress> result;
#if defined(_WIN32)
    ULONG size = 0;
    constexpr ULONG kFlags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                             GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, kFlags, nullptr, nullptr, &size) !=
            ERROR_BUFFER_OVERFLOW ||
        size == 0) {
        return result;
    }
    std::vector<char> buffer(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_INET, kFlags, nullptr, adapters, &size) !=
        NO_ERROR) {
        return result;
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
            result.push_back({narrowAdapterName(adapter->FriendlyName), text});
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0 || list == nullptr) return result;
    // getifaddrs 的链表必须整体 freeifaddrs，中途 continue 不能漏。
    struct IfaddrsGuard {
        ifaddrs* list;
        ~IfaddrsGuard() { freeifaddrs(list); }
    } guard{list};
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
        result.push_back({entry->ifa_name, text});
    }
#endif
    return result;
}

} // namespace netinfo
