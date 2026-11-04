#pragma once

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace clashflux::network {

struct Advanced {
    std::string auto_redirect; // empty/disabled: managed policy routing; enabled: nftables
    bool override_filters = false;
    std::string interface_mode, interfaces, uid_mode, uids, mac_mode, macs;
    bool override_route_sets = false;
    std::string route_sets, exclude_sets; // stable IDs in the global resource library
    std::string proxy_mode = "manual"; // manual / pac / pac_url
    int pac_port = 7900;
    std::string pac_url, pac_direct_domains;
    bool operator==(const Advanced&) const = default;
};

// One versioned cache value is committed for a whole network form. Empty
// advanced values inherit the source; they are not fabricated kernel defaults.
struct Settings {
    int format_version = 1;
    std::uint64_t revision = 0;
    int mixed_port = 7899, http_port = 0, socks_port = 0;
    bool allow_lan = false, ipv6 = false;
    int mtu = 1400;
    bool override_mtu = false, strict_route = true;
    std::string stack, dns_mode, interface_name, addresses;
    bool override_routes = false, override_exclusions = false;
    std::string routes, exclusions;
    std::string udp_timeout, udp_mapping, udp_filtering;
    int udp_nat_max = 0;
    bool override_udp_nat_max = false;
    std::string package_mode, packages;
    std::string listen_address; // empty: loopback / all IPv4, according to allow_lan
    std::string bind_interface;
    bool override_ports = false, override_listen = false;
    std::string allowed_sources;
    bool override_auth = false;
    std::string username, password;
    std::string proxy_target = "mixed";
    bool override_bypass = false, restore_proxy = true;
    std::string bypass;
    std::optional<Advanced> advanced; // absent in the original v1 contract
    bool operator==(const Settings&) const = default;
};

inline std::vector<std::string> Lines(std::string_view text) {
    std::vector<std::string> out;
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = text.substr(0, end);
        const auto first = line.find_first_not_of(" \t\r");
        if (first != std::string_view::npos) {
            line = line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
            out.emplace_back(line);
        }
        if (end == std::string_view::npos) break;
        text.remove_prefix(end + 1);
    }
    return out;
}

inline bool Integer(std::string_view text, int& value, int low, int high) {
    if (text.empty()) return false;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size() && value >= low && value <= high;
}

// Portable strict IP syntax checking, also used before native candidate checks.
inline bool IPv4(std::string_view text) {
    for (int i = 0; i < 4; ++i) {
        const auto end = text.find('.');
        const auto part = text.substr(0, end);
        int value = 0;
        if (!Integer(part, value, 0, 255) || (part.size() > 1 && part.front() == '0')) return false;
        if (i == 3) return end == std::string_view::npos;
        if (end == std::string_view::npos) return false;
        text.remove_prefix(end + 1);
    }
    return false;
}

inline bool IPv6(std::string_view text) {
    if (text.empty()) return false;
    const auto compression = text.find("::");
    if (compression != std::string_view::npos && text.find("::", compression + 2) != std::string_view::npos) return false;
    int groups = 0;
    const auto count = [&groups](std::string_view part, bool last) {
        if (part.empty()) return true;
        while (!part.empty()) {
            const auto end = part.find(':');
            const auto group = part.substr(0, end);
            if (group.empty()) return false;
            if (group.find('.') != std::string_view::npos) {
                if (!last || end != std::string_view::npos || !IPv4(group)) return false;
                groups += 2;
            } else {
                if (group.size() > 4 || group.find_first_not_of("0123456789abcdefABCDEF") != std::string_view::npos) return false;
                ++groups;
            }
            if (end == std::string_view::npos) break;
            part.remove_prefix(end + 1);
            if (part.empty()) return false;
        }
        return true;
    };
    if (compression == std::string_view::npos) return count(text, true) && groups == 8;
    return count(text.substr(0, compression), false) && count(text.substr(compression + 2), true) && groups < 8;
}

inline bool IP(std::string_view value) { return IPv4(value) || IPv6(value); }
inline bool CIDR(std::string_view value) {
    const auto slash = value.find('/');
    if (slash == std::string_view::npos) return false;
    const auto ip = value.substr(0, slash);
    int prefix = 0;
    return IP(ip) && Integer(value.substr(slash + 1), prefix, 0, IPv4(ip) ? 32 : 128);
}

inline bool OneOf(std::string_view value, std::initializer_list<std::string_view> choices) {
    return std::find(choices.begin(), choices.end(), value) != choices.end();
}

inline bool PacUrl(std::string_view url) {
    const auto start = url.starts_with("https://") ? 8U : url.starts_with("http://") ? 7U : 0U;
    if (!start || url.size() > 4096 || url.find_first_of("@#\\\"' <>`$\r\n\t") != std::string_view::npos ||
        std::any_of(url.begin(), url.end(), [](unsigned char c) { return c < 33 || c > 126; })) return false;
    auto authority = url.substr(start, url.find_first_of("/?", start) - start);
    std::string_view host, port;
    if (authority.starts_with('[')) {
        const auto end = authority.find(']');
        if (end == std::string_view::npos || !IPv6(authority.substr(1, end - 1))) return false;
        if (end + 1 == authority.size()) return true;
        if (authority[end + 1] != ':') return false;
        port = authority.substr(end + 2);
    } else {
        const auto colon = authority.find(':');
        host = authority.substr(0, colon);
        if (host.empty() || host.size() > 253 || host.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") != std::string_view::npos) return false;
        if (host.ends_with('.')) host.remove_suffix(1);
        if (host.empty()) return false;
        while (!host.empty()) {
            const auto dot = host.find('.');
            const auto label = host.substr(0, dot);
            if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') return false;
            if (dot == std::string_view::npos) break;
            host.remove_prefix(dot + 1);
            if (host.empty()) return false;
        }
        if (colon == std::string_view::npos) return true;
        port = authority.substr(colon + 1);
    }
    int value = 0;
    return Integer(port, value, 1, 65535);
}

inline std::string ValidateAdvanced(const Advanced& a) {
    if (!OneOf(a.auto_redirect, {"", "enabled", "disabled"}) ||
        !OneOf(a.proxy_mode, {"manual", "pac", "pac_url"})) return "网络高级选项无效";
    for (const auto* mode : {&a.interface_mode, &a.uid_mode, &a.mac_mode})
        if (!OneOf(*mode, {"", "include", "exclude"})) return "接管筛选方式无效";
    for (const auto* list : {&a.interfaces, &a.uids, &a.macs, &a.route_sets, &a.exclude_sets, &a.pac_direct_domains})
        if (list->size() > 16384 || Lines(*list).size() > 256) return "高级列表过长";
    if (a.override_filters) {
        if ((!a.interface_mode.empty() && Lines(a.interfaces).empty()) ||
            (!a.uid_mode.empty() && Lines(a.uids).empty()) || (!a.mac_mode.empty() && Lines(a.macs).empty()))
            return "接管筛选列表不能为空";
        if (!a.interface_mode.empty()) for (const auto& name : Lines(a.interfaces))
            if (name.size() > 15 || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.:-") != std::string::npos)
                return "网络接口名称无效";
        if (!a.uid_mode.empty()) for (const auto& uid : Lines(a.uids)) {
            const auto colon = uid.find(':');
            int first = 0, last = 0;
            if (colon == std::string::npos) {
                if (!Integer(uid, first, 0, 2147483647)) return "UID 需要整数或起始:结束范围";
            } else if (!Integer(std::string_view(uid).substr(0, colon), first, 0, 2147483647) ||
                       !Integer(std::string_view(uid).substr(colon + 1), last, first, 2147483647))
                return "UID 需要整数或起始:结束范围";
        }
        if (!a.mac_mode.empty()) {
            if (a.auto_redirect != "enabled") return "MAC 筛选需要启用 Linux 自动重定向";
            for (const auto& mac : Lines(a.macs)) {
                if (mac.size() != 17) return "MAC 地址需要六组十六进制数字";
                for (std::size_t i = 0; i < mac.size(); ++i)
                    if (i % 3 == 2 ? mac[i] != ':' : std::string_view("0123456789abcdefABCDEF").find(mac[i]) == std::string_view::npos)
                        return "MAC 地址需要六组十六进制数字";
            }
        }
    }
    if (a.override_route_sets) for (const auto* list : {&a.route_sets, &a.exclude_sets})
        for (const auto& id : Lines(*list))
            if (id.size() > 96 || id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-" ) != std::string::npos)
                return "规则集资源 ID 无效";
    if (a.pac_port < 1 || a.pac_port > 65535) return "PAC 端口需要 1–65535 的整数";
    if (a.proxy_mode == "pac_url" && !PacUrl(a.pac_url))
        return "PAC URL 需要有效的 HTTP(S) 地址，不能含凭据或片段";
    for (const auto& domain : Lines(a.pac_direct_domains))
        if (domain.size() > 253 || domain.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.*?_-:") != std::string::npos)
            return "PAC 直连项只支持主机、域名和通配符，不支持 CIDR";
    return {};
}

inline std::string Validate(const Settings& s, bool android = false) {
    if (s.format_version != 1) return "不支持的网络设置版本，保留原数据";
    if (s.mixed_port < 1 || s.mixed_port > 65535 || s.http_port < 0 || s.http_port > 65535 ||
        s.socks_port < 0 || s.socks_port > 65535) return "端口无效";
    if ((s.http_port && (s.http_port == s.mixed_port || s.http_port == s.socks_port)) ||
        (s.socks_port && s.socks_port == s.mixed_port)) return "不同类型的端口不能重复";
    if (s.mtu < 1280 || s.mtu > 9000) return "MTU 需要填写 1280–9000 的整数";
    if (!OneOf(s.stack, {"", "system", "gvisor", "mixed"}) ||
        !OneOf(s.dns_mode, {"", "native", "hijack", "disabled"}) ||
        !OneOf(s.udp_mapping, {"", "endpoint_independent", "address_dependent", "address_and_port_dependent"}) ||
        !OneOf(s.udp_filtering, {"", "endpoint_independent", "address_dependent", "address_and_port_dependent"}) ||
        !OneOf(s.package_mode, {"", "all", "include", "exclude"}) ||
        !OneOf(s.proxy_target, {"mixed", "http"})) return "网络设置选项无效";
    if (android && !s.stack.empty() && s.stack != "gvisor") return "Android 当前仅支持 gVisor 协议栈";
    for (const auto* text : {&s.addresses, &s.routes, &s.exclusions, &s.allowed_sources}) {
        const auto entries = Lines(*text);
        if (text->size() > 16384 || entries.size() > 256) return "网段列表过长";
        if ((text == &s.routes && !s.override_routes) || (text == &s.exclusions && !s.override_exclusions)) continue;
        for (const auto& entry : entries) if (!CIDR(entry)) return "网段需要填写有效的 IPv4/IPv6 CIDR";
    }
    if (!s.udp_timeout.empty()) {
        int seconds = 0;
        const auto unit = s.udp_timeout.back();
        if (!OneOf(std::string_view(&unit, 1), {"s", "m", "h"}) ||
            !Integer(std::string_view(s.udp_timeout).substr(0, s.udp_timeout.size() - 1), seconds, 1, 86400))
            return "UDP 超时需要正整数及 s、m 或 h 单位";
    }
    if (s.udp_nat_max < 0 || s.udp_nat_max > 1048576) return "UDP 会话上限需要填写 0–1048576 的整数";
    for (const auto* text : {&s.interface_name, &s.bind_interface})
        if (text->size() > 64 || text->find_first_of("\r\n\t/\\\"'") != std::string::npos)
            return "网络接口名称无效";
    if (s.override_listen && !s.listen_address.empty() && !IP(s.listen_address)) return "监听地址需要填写 IP 地址";
    if (s.override_auth && (s.username.empty() != s.password.empty())) return "代理认证需要同时填写用户名和密码";
    if (s.username.size() > 256 || s.password.size() > 1024) return "代理认证信息过长";
    if (s.packages.size() > 32768 || Lines(s.packages).size() > 512) return "应用列表过长";
    if ((s.package_mode == "include" || s.package_mode == "exclude") && Lines(s.packages).empty())
        return "请选择至少一个应用";
    for (const auto& package : Lines(s.packages))
        if (package.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._") != std::string::npos ||
            package.find('.') == std::string::npos) return "应用包名无效";
    if (s.bypass.size() > 16384 || Lines(s.bypass).size() > 256) return "系统代理绕过列表过长";
    for (const auto& entry : Lines(s.bypass))
        if (entry.find_first_of(" \t;,'\"\\/$`<>|") != std::string::npos) return "绕过项需要填写主机名、通配域名或 IP 地址";
    if (s.advanced) {
        if (const auto error = ValidateAdvanced(*s.advanced); !error.empty()) return error;
        if (s.advanced->proxy_mode == "pac" && (s.advanced->pac_port == s.mixed_port ||
            s.advanced->pac_port == s.http_port || s.advanced->pac_port == s.socks_port)) return "PAC 端口不能与代理监听端口重复";
        if (android && (s.advanced->proxy_mode != "manual" || !s.advanced->auto_redirect.empty() ||
            s.advanced->override_filters || s.advanced->override_route_sets)) return "Android 不支持桌面路由或 PAC 接管设置";
    }
    return {};
}

struct ProxyValue {
    std::string scope, key, value;
    bool present = true;
    bool operator==(const ProxyValue&) const = default;
};
struct ProxySnapshot {
    std::string desktop;
    std::vector<ProxyValue> values;
    bool operator==(const ProxySnapshot&) const = default;
};
// Original v1 KDE leases did not write or fingerprint the PAC URL. Compare
// only their original contract; new leases always include the extra field.
inline bool ProxySnapshotMatches(const ProxySnapshot& expected, ProxySnapshot current) {
    if (expected.desktop == "kde" && std::none_of(expected.values.begin(), expected.values.end(),
        [](const auto& item) { return item.key == "Proxy Config Script"; }))
        std::erase_if(current.values, [](const auto& item) { return item.key == "Proxy Config Script"; });
    return expected == current;
}

struct ProxyLease {
    int format_version = 1;
    ProxySnapshot original, applied;
    bool restore = true;
    std::optional<std::string> local_pac_url;
};
} // namespace clashflux::network
