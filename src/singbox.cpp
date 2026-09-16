// singbox.cpp — clashflux.singbox 实现单元（yaml-cpp 解析订阅 + nlohmann 合成 JSON）。
//
// 产物形态对齐 sing-box 1.14：special outbounds（block/dns）已移除，REJECT
// 走 route rule action；sniff 走首条 rule action（不再写 inbound.sniff）；
// DNS 用 1.12+ 的 typed server。字段集保持严格最小——sing-box 对未知字段
// 直接拒绝启动。
module;

#include <yaml-cpp/yaml.h>

module clashflux.singbox;

import std;
import nlohmann.json;

namespace singbox {
namespace {

// ---- YAML 取值助手（缺键/类型不符一律安全回落）----------------------------

std::string trimCopy(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
                          s.front() == '\r' || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                          s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return std::string{s};
}

std::string lowerCopy(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    }
    return s;
}

std::string ytext(const YAML::Node& map, const char* key) {
    if (!map) return {};
    const YAML::Node value = map[key];
    if (!value || !value.IsScalar()) return {};
    return trimCopy(value.Scalar());
}

std::optional<int> yint(const YAML::Node& map, const char* key) {
    const std::string text = ytext(map, key);
    if (text.empty()) return std::nullopt;
    int parsed = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (ec != std::errc() || ptr != text.data() + text.size()) return std::nullopt;
    return parsed;
}

bool ybool(const YAML::Node& map, const char* key) {
    const std::string text = lowerCopy(ytext(map, key));
    return text == "true" || text == "yes" || text == "1";
}

std::vector<std::string> ylist(const YAML::Node& map, const char* key) {
    std::vector<std::string> out;
    if (!map) return out;
    const YAML::Node value = map[key];
    if (!value || !value.IsScalar()) {
        if (value && value.IsSequence()) {
            for (const YAML::Node& item : value) {
                if (item.IsScalar()) out.push_back(trimCopy(item.Scalar()));
            }
        }
        return out;
    }
    out.push_back(trimCopy(value.Scalar()));
    return out;
}

// ---- 编译上下文 -----------------------------------------------------------

struct Context {
    const CompileOptions& opt;
    CompileResult result;
    nlohmann::json config;
    nlohmann::json outbounds = nlohmann::json::array();
    nlohmann::json ruleSets = nlohmann::json::array();
    std::vector<std::string> knownTags;               // 节点 + 组 + DIRECT
    std::map<std::string, std::string> geoipTags;     // 国家码 → rule_set tag
    std::map<std::string, std::string> dnsServerTags; // Clash DNS 地址 → typed server tag
    std::string finalTarget;                          // MATCH 目标

    explicit Context(const CompileOptions& options) : opt(options) {}

    void warn(std::string message) { result.warnings.push_back(std::move(message)); }
    bool tagKnown(const std::string& tag) const {
        for (const std::string& known : knownTags) {
            if (known == tag) return true;
        }
        return false;
    }
};

struct DnsEndpoint {
    std::string scheme;
    std::string host;
    std::string path;
    std::string detour;
    int port = 0;
};

bool isPortText(std::string_view value) {
    if (value.empty()) return false;
    int port = 0;
    const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), port);
    return ec == std::errc() && ptr == value.data() + value.size() &&
           port > 0 && port <= 65535;
}

std::optional<DnsEndpoint> parseDnsEndpoint(std::string_view rawValue) {
    std::string value = trimCopy(rawValue);
    if (value.empty()) return std::nullopt;

    DnsEndpoint endpoint;
    if (const std::size_t fragment = value.find('#'); fragment != std::string::npos) {
        endpoint.detour = trimCopy(std::string_view{value}.substr(fragment + 1));
        value.resize(fragment);
    }

    const std::size_t schemeEnd = value.find("://");
    if (schemeEnd == std::string::npos) {
        endpoint.scheme = "udp";
        endpoint.host = value;
        endpoint.port = 53;
        return endpoint.host.empty() ? std::nullopt : std::optional{std::move(endpoint)};
    }

    endpoint.scheme = lowerCopy(value.substr(0, schemeEnd));
    std::string authorityAndPath = value.substr(schemeEnd + 3);
    const std::size_t pathStart = authorityAndPath.find('/');
    const std::string authority = pathStart == std::string::npos
                                      ? authorityAndPath
                                      : authorityAndPath.substr(0, pathStart);
    endpoint.path = pathStart == std::string::npos
                        ? std::string{}
                        : authorityAndPath.substr(pathStart);
    if (authority.empty()) return std::nullopt;

    if (authority.front() == '[') {
        const std::size_t closing = authority.find(']');
        if (closing == std::string::npos) return std::nullopt;
        endpoint.host = authority.substr(1, closing - 1);
        if (closing + 1 < authority.size()) {
            if (authority[closing + 1] != ':' ||
                !isPortText(std::string_view{authority}.substr(closing + 2))) {
                return std::nullopt;
            }
            endpoint.port = std::stoi(authority.substr(closing + 2));
        }
    } else {
        const std::size_t colon = authority.rfind(':');
        if (colon != std::string::npos && authority.find(':') == colon &&
            isPortText(std::string_view{authority}.substr(colon + 1))) {
            endpoint.host = authority.substr(0, colon);
            endpoint.port = std::stoi(authority.substr(colon + 1));
        } else {
            endpoint.host = authority;
        }
    }
    if (endpoint.host.empty()) return std::nullopt;
    if (endpoint.port == 0) {
        endpoint.port = endpoint.scheme == "https" || endpoint.scheme == "tls" ? 443 : 53;
    }
    return endpoint;
}

bool dnsHostIsDomain(std::string_view host) {
    if (host.find(':') != std::string_view::npos) return false;
    bool hasLetter = false;
    for (const unsigned char c : host) {
        if (std::isalpha(c)) hasLetter = true;
        if (std::isalpha(c) || std::isdigit(c) || c == '.' || c == '-') continue;
        return false;
    }
    return hasLetter;
}

std::string appendDnsServer(Context& ctx, std::string_view rawValue) {
    const std::string raw = trimCopy(rawValue);
    if (raw.empty()) return {};
    if (const auto known = ctx.dnsServerTags.find(raw); known != ctx.dnsServerTags.end()) {
        return known->second;
    }

    // rcode:// and other legacy Clash pseudo-servers are DNS rules in sing-box
    // 1.14, not transport servers. They are intentionally ignored here rather
    // than emitting an invalid typed server and preventing the whole core from
    // starting.
    const std::optional<DnsEndpoint> endpoint = parseDnsEndpoint(raw);
    if (!endpoint || endpoint->scheme == "rcode" || endpoint->scheme == "fakeip" ||
        endpoint->scheme == "dhcp") {
        ctx.warn(std::format("DNS 服务器「{}」格式暂不支持，已忽略", raw));
        return {};
    }
    const std::string scheme = endpoint->scheme == "http" ? "https" : endpoint->scheme;
    if (scheme != "udp" && scheme != "tcp" && scheme != "tls" && scheme != "https" &&
        scheme != "quic") {
        ctx.warn(std::format("DNS 服务器「{}」协议 {} 暂不支持，已忽略", raw, scheme));
        return {};
    }

    const std::string tag = std::format("dns-{}", ctx.dnsServerTags.size());
    nlohmann::json server = {
        {"type", scheme},
        {"tag", tag},
        {"server", endpoint->host},
    };
    const int defaultPort = scheme == "https" || scheme == "tls" ? 443 : 53;
    if (endpoint->port != defaultPort) server["server_port"] = endpoint->port;
    if (scheme == "https" && !endpoint->path.empty() && endpoint->path != "/dns-query") {
        server["path"] = endpoint->path;
    }
    if (dnsHostIsDomain(endpoint->host)) {
        // Resolve the DoH/DoT endpoint through the Android protected local
        // resolver. The DNS payload itself still follows the requested detour.
        server["domain_resolver"] = {{"server", "local"}};
    }
    if (!endpoint->detour.empty()) {
        if (ctx.tagKnown(endpoint->detour)) {
            server["detour"] = endpoint->detour;
        } else {
            ctx.warn(std::format("DNS 服务器「{}」引用了未知出站「{}」，已按直连处理",
                                 raw, endpoint->detour));
        }
    }
    ctx.config["dns"]["servers"].push_back(std::move(server));
    ctx.dnsServerTags.emplace(raw, tag);
    return tag;
}

void applyClashDns(Context& ctx, const YAML::Node& dns) {
    if (!dns || !dns.IsMap()) return;

    std::vector<std::string> nameservers = ylist(dns, "nameserver");
    if (nameservers.empty()) nameservers = ylist(dns, "fallback");
    std::string primary;
    for (const std::string& raw : nameservers) {
        const std::string tag = appendDnsServer(ctx, raw);
        if (primary.empty() && !tag.empty()) primary = tag;
    }
    if (primary.empty()) return;

    ctx.config["dns"]["final"] = primary;
    ctx.config["dns"]["strategy"] = ybool(dns, "ipv6") ? "prefer_ipv4" : "ipv4_only";
}

// ---- TLS / 传输层（vmess/vless/trojan 共用）--------------------------------

void appendTls(Context& ctx, const YAML::Node& item, nlohmann::json& out) {
    const std::string sni = [] (const YAML::Node& node) {
        std::string value = ytext(node, "servername");
        if (value.empty()) value = ytext(node, "sni");
        return value;
    }(item);
    const bool skipVerify = ybool(item, "skip-cert-verify");
    if (!ybool(item, "tls") && sni.empty() && !skipVerify) return;
    nlohmann::json tls = {{"enabled", true}};
    if (!sni.empty()) tls["server_name"] = sni;
    if (skipVerify) tls["insecure"] = true;
    if (const std::vector<std::string> alpn = ylist(item, "alpn"); !alpn.empty()) {
        tls["alpn"] = alpn;
    }
    // mihomo 全局 uTLS 指纹的历史行为：无节点级指纹时按 chrome 处理。
    std::string fingerprint = ytext(item, "client-fingerprint");
    if (fingerprint.empty()) fingerprint = "chrome";
    tls["utls"] = {{"enabled", true}, {"fingerprint", fingerprint}};
    if (const YAML::Node reality = item["reality-opts"]; reality && reality.IsMap()) {
        nlohmann::json realityOut = {{"enabled", true}};
        if (const std::string publicKey = ytext(reality, "public-key"); !publicKey.empty()) {
            realityOut["public_key"] = publicKey;
        }
        if (const std::string shortId = ytext(reality, "short-id"); !shortId.empty()) {
            realityOut["short_id"] = shortId;
        }
        tls["reality"] = realityOut;
    }
    out["tls"] = std::move(tls);
}

// Clash 把传输层放在 network + *-opts；sing-box 用 typed transport 对象。
bool appendTransport(Context& ctx, const YAML::Node& item, nlohmann::json& out,
                     const std::string& name) {
    const std::string network = lowerCopy(ytext(item, "network"));
    if (network.empty() || network == "tcp") return true;
    if (network == "ws") {
        // sing-box 的类型名是 "ws"（"websocket" 会直接解码失败）。
        nlohmann::json transport = {{"type", "ws"}};
        if (const YAML::Node opts = item["ws-opts"]; opts && opts.IsMap()) {
            if (const std::string path = ytext(opts, "path"); !path.empty()) {
                transport["path"] = path;
            }
            if (const YAML::Node headers = opts["headers"]; headers && headers.IsMap()) {
                nlohmann::json mapped = nlohmann::json::object();
                for (auto it = headers.begin(); it != headers.end(); ++it) {
                    const YAML::Node key = it->first;
                    const YAML::Node value = it->second;
                    if (key.IsScalar() && value.IsScalar()) {
                        mapped[key.Scalar()] = trimCopy(value.Scalar());
                    }
                }
                if (!mapped.empty()) transport["headers"] = std::move(mapped);
            }
            if (const auto earlyData = yint(opts, "max-early-data"); earlyData) {
                transport["max_early_data"] = *earlyData;
            }
            if (const std::string earlyHeader = ytext(opts, "early-data-header-name");
                !earlyHeader.empty()) {
                transport["early_data_header_name"] = earlyHeader;
            }
        }
        out["transport"] = std::move(transport);
        return true;
    }
    if (network == "grpc") {
        nlohmann::json transport = {{"type", "grpc"}};
        std::string service = ytext(item, "grpc-service-name");
        if (const YAML::Node opts = item["grpc-opts"]; opts && opts.IsMap()) {
            const std::string nested = ytext(opts, "grpc-service-name");
            if (!nested.empty()) service = nested;
        }
        if (!service.empty()) transport["service_name"] = service;
        out["transport"] = std::move(transport);
        return true;
    }
    if (network == "h2" || network == "http") {
        nlohmann::json transport = {{"type", "http"}};
        const YAML::Node opts = network == "h2" ? item["h2-opts"] : item["http-opts"];
        if (opts && opts.IsMap()) {
            if (const std::vector<std::string> paths = ylist(opts, "path"); !paths.empty()) {
                transport["path"] = paths.front();
            }
            if (const YAML::Node host = opts["host"]; host && host.IsSequence()) {
                std::vector<std::string> hosts;
                for (const YAML::Node& entry : host) {
                    if (entry.IsScalar()) hosts.push_back(trimCopy(entry.Scalar()));
                }
                if (!hosts.empty()) transport["host"] = std::move(hosts);
            }
        }
        out["transport"] = std::move(transport);
        return true;
    }
    if (network == "httpupgrade") {
        nlohmann::json transport = {{"type", "httpupgrade"}};
        if (const YAML::Node opts = item["httpupgrade-opts"]; opts && opts.IsMap()) {
            if (const std::string path = ytext(opts, "path"); !path.empty()) {
                transport["path"] = path;
            }
            if (const std::string host = ytext(opts, "host"); !host.empty()) {
                transport["host"] = host;
            }
        }
        out["transport"] = std::move(transport);
        return true;
    }
    ctx.warn(std::format("节点「{}」的传输层 network:{} 暂不支持，已跳过", name, network));
    return false;
}

// ---- 节点转换 ---------------------------------------------------------------

std::optional<nlohmann::json> convertProxy(Context& ctx, const YAML::Node& item) {
    const std::string name = ytext(item, "name");
    const std::string type = lowerCopy(ytext(item, "type"));
    if (name.empty() || type.empty()) {
        ctx.warn("订阅包含无名称或类型的节点，已跳过");
        return std::nullopt;
    }
    if (ctx.tagKnown(name)) {
        ctx.warn(std::format("订阅包含重名节点「{}」，仅保留首个", name));
        return std::nullopt;
    }
    const std::string server = ytext(item, "server");
    const auto port = yint(item, "port");
    if (server.empty() || !port || *port <= 0 || *port > 65535) {
        ctx.warn(std::format("节点「{}」缺少有效的 server/port，已跳过", name));
        return std::nullopt;
    }

    nlohmann::json out = {{"tag", name}};
    std::string singboxType = type;
    if (type == "ss") singboxType = "shadowsocks";
    if (type == "hy2") singboxType = "hysteria2";
    out["type"] = singboxType;
    out["server"] = server;
    out["server_port"] = *port;

    if (singboxType == "shadowsocks") {
        out["method"] = ytext(item, "cipher");
        if (const std::string password = ytext(item, "password"); !password.empty()) {
            out["password"] = password;
        }
        if (!ytext(item, "plugin").empty()) {
            ctx.warn(std::format("节点「{}」使用 SS 插件（obfs/v2ray-plugin），暂不支持，已跳过", name));
            return std::nullopt;
        }
        if (ybool(item, "udp-over-tcp")) out["udp_over_tcp"] = true;
    } else if (singboxType == "vmess") {
        out["uuid"] = ytext(item, "uuid");
        std::string security = ytext(item, "cipher");
        if (security.empty()) security = "auto";
        out["security"] = lowerCopy(security);
        if (const auto alterId = yint(item, "alterId"); alterId) {
            out["alter_id"] = *alterId;
        }
        if (!appendTransport(ctx, item, out, name)) return std::nullopt;
        appendTls(ctx, item, out);
    } else if (singboxType == "vless") {
        out["uuid"] = ytext(item, "uuid");
        if (const std::string flow = ytext(item, "flow"); !flow.empty()) {
            out["flow"] = flow;
        }
        if (!appendTransport(ctx, item, out, name)) return std::nullopt;
        appendTls(ctx, item, out);
    } else if (singboxType == "trojan") {
        if (const std::string password = ytext(item, "password"); !password.empty()) {
            out["password"] = password;
        }
        if (!appendTransport(ctx, item, out, name)) return std::nullopt;
        appendTls(ctx, item, out);
    } else if (singboxType == "hysteria2") {
        if (const std::string password = ytext(item, "password"); !password.empty()) {
            out["password"] = password;
        }
        // 带宽字段 "30 Mbps" / "30" 之类取前导数字；无效则省略（内核自适应）。
        const auto leadingNumber = [](std::string_view text) -> std::optional<int> {
            // 拷贝到稳定存储：trimCopy 返回的临时 string 赋回 string_view 会
            // 悬垂（libstdc++ 的 SSO 缓冲恰好能读到，libc++ 下即出错）。
            const std::string trimmed = trimCopy(text);
            std::string_view digits{trimmed};
            std::size_t end = 0;
            while (end < digits.size() && digits[end] >= '0' && digits[end] <= '9') {
                ++end;
            }
            if (end == 0) return std::nullopt;
            int parsed = 0;
            const auto [ptr, ec] =
                std::from_chars(digits.data(), digits.data() + end, parsed);
            if (ec != std::errc()) return std::nullopt;
            return parsed;
        };
        if (const auto up = leadingNumber(ytext(item, "up")); up) out["up_mbps"] = *up;
        if (const auto down = leadingNumber(ytext(item, "down")); down) out["down_mbps"] = *down;
        if (const std::string obfsPassword = ytext(item, "obfs-password"); !obfsPassword.empty()) {
            out["obfs"] = {{"type", "salamander"}, {"password", obfsPassword}};
        } else if (const std::string obfs = ytext(item, "obfs");
                   !obfs.empty() && lowerCopy(obfs) != "none") {
            ctx.warn(std::format("节点「{}」的 obfs 配置缺少 obfs-password，已忽略", name));
        }
        if (const std::string ports = ytext(item, "ports"); !ports.empty()) {
            // Clash "20000-30000,40000" → sing-box server_ports ["20000:30000","40000"]。
            nlohmann::json serverPorts = nlohmann::json::array();
            std::string_view rest{ports};
            while (!rest.empty()) {
                const auto comma = rest.find(',');
                std::string piece = trimCopy(rest.substr(0, comma));
                if (!piece.empty()) {
                    const auto dash = piece.find('-');
                    if (dash != std::string::npos) piece.replace(dash, 1, ":");
                    serverPorts.push_back(std::move(piece));
                }
                rest = comma == std::string_view::npos
                           ? std::string_view{}
                           : rest.substr(comma + 1);
            }
            if (!serverPorts.empty()) out["server_ports"] = std::move(serverPorts);
        }
        appendTls(ctx, item, out);
    } else if (singboxType == "tuic") {
        out["uuid"] = ytext(item, "uuid");
        if (const std::string password = ytext(item, "password"); !password.empty()) {
            out["password"] = password;
        }
        if (const std::string congestion =
                ytext(item, "congestion-controller"); !congestion.empty()) {
            out["congestion_control"] = congestion;
        }
        if (ybool(item, "reduce-rtt")) out["zero_rtt_handshake"] = true;
        if (ybool(item, "disable-sni")) out["disable_sni"] = true;
        if (const std::vector<std::string> alpn = ylist(item, "alpn"); !alpn.empty()) {
            nlohmann::json tls = {{"enabled", true}};
            if (const std::string sni = ytext(item, "sni"); !sni.empty()) {
                tls["server_name"] = sni;
            }
            if (ybool(item, "skip-cert-verify")) tls["insecure"] = true;
            tls["alpn"] = alpn;
            out["tls"] = std::move(tls);
        } else {
            appendTls(ctx, item, out);
        }
    } else if (singboxType == "http" || singboxType == "socks5") {
        if (singboxType == "socks5") out["type"] = "socks";
        if (const std::string username = ytext(item, "username"); !username.empty()) {
            out["username"] = username;
        }
        if (const std::string password = ytext(item, "password"); !password.empty()) {
            out["password"] = password;
        }
        appendTls(ctx, item, out);
    } else {
        ctx.warn(std::format("节点「{}」的协议 {} 暂不支持，已跳过", name, type));
        return std::nullopt;
    }
    return out;
}

// ---- 代理组转换 --------------------------------------------------------------

std::optional<nlohmann::json> convertGroup(Context& ctx, const YAML::Node& item,
                                           const std::string& name,
                                           const std::vector<std::string>& members) {
    const std::string clashType = lowerCopy(ytext(item, "type"));
    nlohmann::json out = {{"tag", name}};
    if (clashType == "select") {
        out["type"] = "selector";
    } else if (clashType == "url-test" || clashType == "fallback" ||
               clashType == "load-balance") {
        if (clashType != "url-test") {
            ctx.warn(std::format("代理组「{}」类型 {} 在 sing-box 无对应语义，降级为 urltest",
                                 name, clashType));
        }
        out["type"] = "urltest";
        std::string url = ytext(item, "url");
        if (url.empty()) url = "https://www.gstatic.com/generate_204";
        out["url"] = url;
        if (const auto interval = yint(item, "interval"); interval && *interval > 0) {
            out["interval"] = std::format("{}s", *interval);
        }
    } else {
        ctx.warn(std::format("代理组「{}」类型 {} 暂不支持，已跳过", name, clashType));
        return std::nullopt;
    }
    // 成员必须是已知的节点/组/DIRECT；REJECT 等特殊目标在 1.14 无对应 outbound，
    // 过滤并提示（sing-box 对未知 outbound tag 直接拒绝启动）。
    nlohmann::json filtered = nlohmann::json::array();
    for (const std::string& member : members) {
        if (member == "REJECT" || member == "REJECT-DROP" || member == "PASS") {
            ctx.warn(std::format("代理组「{}」的成员 {} 在 sing-box 无对应 outbound，已移除",
                                 name, member));
            continue;
        }
        if (member != "DIRECT" && !ctx.tagKnown(member)) {
            ctx.warn(std::format("代理组「{}」引用了未知的成员「{}」，已移除", name, member));
            continue;
        }
        filtered.push_back(member);
    }
    if (filtered.empty()) filtered.push_back("DIRECT");
    out["outbounds"] = std::move(filtered);
    return out;
}

// ---- 规则转换 -----------------------------------------------------------------

bool ensureGeoipRuleSet(Context& ctx, std::string country) {
    country = lowerCopy(country);
    if (ctx.geoipTags.contains(country)) {
        const std::string tag = "geoip-" + country;
        for (const auto& ruleSet : ctx.ruleSets) {
            if (ruleSet.is_object() && ruleSet.value("tag", "") == tag) {
                return true;
            }
        }
        return false;
    }
    const std::string tag = "geoip-" + country;
    ctx.geoipTags.emplace(country, tag);
    // MetaCubeX 的 geoip 目录只提供国家/地区代码。某些 Clash 订阅会把
    // TELEGRAM 等类别误写成 GEOIP；继续拼出不存在的 URL 会让 sing-box
    // 在启动阶段直接 FATAL，而不是把规则温和地降级。
    if (country.size() != 2 ||
        !std::all_of(country.begin(), country.end(), [](unsigned char c) {
            return c >= 'a' && c <= 'z';
        })) {
        ctx.warn(std::format("GEOIP 代码「{}」不是国家/地区代码，已跳过", country));
        return false;
    }
    // 本地缓存命中 → local rule_set（首启不依赖代理/网络；core_store 负责
    // 预取与按周刷新）；未命中 → remote，由内核在启动时经默认出站拉取。
    if (!ctx.opt.ruleSetDir.empty()) {
        const std::filesystem::path local =
            std::filesystem::path(ctx.opt.ruleSetDir) / (tag + ".srs");
        std::error_code ec;
        if (std::filesystem::exists(local, ec) && !ec) {
            ctx.ruleSets.push_back({
                {"type", "local"},
                {"tag", tag},
                {"format", "binary"},
                {"path", local.string()},
            });
            return true;
        }
    }
#if defined(__ANDROID__)
    // Android 的远程 GEOIP 规则集会在 VpnService 建立 TUN 的同一阶段下载。
    // 此时 libbox 还没有可用的物理网络接口，下载失败会使整个数据面启动
    // 失败。节点/策略组仍可正常使用，GEOIP 规则只在本地缓存存在时启用。
    ctx.warn(std::format("Android 暂无本地 GEOIP 规则集「{}」，已跳过在线下载", country));
    return false;
#else
    ctx.ruleSets.push_back({
        {"type", "remote"},
        {"tag", tag},
        {"format", "binary"},
        // meta-rules-dat（mihomo geodata 同源）：国家码与 telegram/netflix/
        // cloudflare 等类别全覆盖；SagerNet/sing-geoip 只有国家码，GEOIP,
        // TELEGRAM 这类别会 404 并在启动期 FATAL。
        {"url", std::format("https://raw.githubusercontent.com/MetaCubeX/meta-rules-dat/sing/geo/geoip/{}.srs", country)},
        {"update_interval", "24h"},
    });
    return true;
#endif
}

// 单条 Clash 规则 → sing-box route rule；MATCH 返回 false 表示已写入 final。
bool convertRule(Context& ctx, std::string_view rawLine) {
    // 先拷贝到稳定存储：trimCopy 返回的临时 string 一旦赋回 string_view 就
    // 悬垂（这里真实踩过的坑）。
    const std::string line = trimCopy(rawLine);
    if (line.empty() || line.front() == '#') return true;
    std::vector<std::string> parts;
    std::string_view rest{line};
    while (!rest.empty()) {
        const auto comma = rest.find(',');
        std::string_view piece = rest.substr(0, comma);
        parts.push_back(trimCopy(piece));
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
        if (parts.size() >= 4) break;  // 余下的属性（no-resolve 等）忽略
    }
    if (parts.empty()) return true;
    const std::string kind = lowerCopy(parts[0]);
    if (kind == "match") {
        if (parts.size() >= 2) ctx.finalTarget = parts[1];
        return false;
    }
    if (parts.size() < 3) return true;
    const std::string value = parts[1];
    const std::string target = parts[2];

    nlohmann::json rule = nlohmann::json::object();
    if (target == "REJECT" || target == "REJECT-DROP") {
        rule["action"] = "reject";
    } else if (target == "PASS") {
        ctx.warn(std::format("规则 {} 的目标 PASS 暂不支持，已跳过", line));
        return true;
    } else if (target != "DIRECT" && !ctx.tagKnown(target)) {
        ctx.warn(std::format("规则 {} 引用了未知目标「{}」，已跳过", line, target));
        return true;
    } else {
        rule["outbound"] = target;
    }

    if (kind == "domain") {
        rule["domain"] = {value};
    } else if (kind == "domain-suffix") {
        rule["domain_suffix"] = {value};
    } else if (kind == "domain-keyword") {
        rule["domain_keyword"] = {value};
    } else if (kind == "domain-regex") {
        rule["domain_regex"] = {value};
    } else if (kind == "ip-cidr" || kind == "ip-cidr6") {
        rule["ip_cidr"] = {value};
    } else if (kind == "geoip") {
        const std::string code = lowerCopy(value);
        if (code == "private" || code == "lan") {
            rule["ip_is_private"] = true;
        } else {
            if (!ensureGeoipRuleSet(ctx, code)) return true;
            rule["rule_set"] = {ctx.geoipTags.at(code)};
        }
    } else if (kind == "rule-set") {
        ctx.warn(std::format("规则集 RULE-SET {} 暂不支持（计划后续迭代），已跳过", value));
        return true;
    } else {
        ctx.warn(std::format("规则类型 {} 暂不支持，已跳过", parts[0]));
        return true;
    }
    ctx.config["route"]["rules"].push_back(std::move(rule));
    return true;
}

// ---- 骨架与收尾 ----------------------------------------------------------------

bool validIpv4(std::string_view value) {
    for (int index = 0; index < 4; ++index) {
        const auto separator = value.find('.');
        const auto part = value.substr(0, separator);
        unsigned int octet = 0;
        const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), octet);
        if (part.empty() || (part.size() > 1 && part.front() == '0') ||
            error != std::errc{} || end != part.data() + part.size() || octet > 255 ||
            (index == 3) != (separator == std::string_view::npos)) return false;
        if (index != 3) value.remove_prefix(separator + 1);
    }
    return true;
}

bool validIpv6(std::string_view value) {
    // Portable parser for literal addresses only (no scoped interface or DNS).
    // A dotted IPv4 tail occupies two 16-bit groups.
    const auto compressed = value.find("::");
    if (compressed != std::string_view::npos &&
        value.find("::", compressed + 2) != std::string_view::npos) return false;
    const auto countGroups = [](std::string_view part, bool allowIpv4) {
        int count = 0;
        if (part.empty()) return count;
        while (!part.empty()) {
            const auto separator = part.find(':');
            const auto group = part.substr(0, separator);
            if (group.empty()) return -1;
            if (group.find('.') != std::string_view::npos) {
                if (!allowIpv4 || separator != std::string_view::npos || !validIpv4(group)) return -1;
                return count + 2;
            }
            if (group.size() > 4 || !std::ranges::all_of(group, [](unsigned char ch) {
                    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
                           (ch >= 'A' && ch <= 'F');
                })) return -1;
            ++count;
            if (separator == std::string_view::npos) return count;
            part.remove_prefix(separator + 1);
            if (part.empty()) return -1;
        }
        return count;
    };
    if (compressed == std::string_view::npos) return countGroups(value, true) == 8;
    const int left = countGroups(value.substr(0, compressed), false);
    const int right = countGroups(value.substr(compressed + 2), true);
    return left >= 0 && right >= 0 && left + right < 8;
}

std::optional<std::string> addressCidr(std::string_view raw, bool ipv4Only,
                                     bool requirePrefix = false) {
    const std::string value = trimCopy(raw);
    const auto slash = value.find('/');
    const std::string_view address = std::string_view(value).substr(0, slash);
    // Keep this validation independent from platform name resolution. A host
    // name must never enter sing-box route_exclude_address as if it were an IP.
    const bool ipv4 = validIpv4(address) &&
        std::ranges::count(address, '.') == 3;
    const int bits = ipv4 ? 32 : (!ipv4Only && validIpv6(address) ? 128 : 0);
    if (bits == 0) return std::nullopt;
    if (slash == std::string::npos) {
        return requirePrefix ? std::nullopt : std::optional{value + "/" + std::to_string(bits)};
    }
    unsigned int prefix = 0;
    const auto [end, error] = std::from_chars(value.data() + slash + 1,
                                             value.data() + value.size(), prefix);
    if (error != std::errc{} || end != value.data() + value.size() ||
        prefix > static_cast<unsigned int>(bits)) return std::nullopt;
    return value;
}

int ruleSpecificity(vpn::MatchKind kind) {
    switch (kind) {
    case vpn::MatchKind::ExactDomain:
    case vpn::MatchKind::ExactIp: return 4;
    case vpn::MatchKind::DomainSuffix: return 3;
    case vpn::MatchKind::Ipv4Cidr: return 2;
    case vpn::MatchKind::Any: return 1;
    }
    return 0;
}

std::optional<nlohmann::json> globalRuleMatch(Context& ctx, const vpn::RouteRule& rule) {
    nlohmann::json match = nlohmann::json::object();
    std::string pattern = trimCopy(rule.pattern);
    switch (rule.match) {
    case vpn::MatchKind::Any:
        if (pattern.empty()) return match;
        break;
    case vpn::MatchKind::ExactDomain:
    case vpn::MatchKind::DomainSuffix:
        if (const auto host = vpn::NormalizeRuleDomain(pattern)) {
            match[rule.match == vpn::MatchKind::ExactDomain ? "domain" : "domain_suffix"] = {*host};
            return match;
        }
        break;
    case vpn::MatchKind::ExactIp:
    case vpn::MatchKind::Ipv4Cidr: {
        const bool cidr = rule.match == vpn::MatchKind::Ipv4Cidr;
        if (!cidr && pattern.find('/') != std::string::npos) break;
        if (const auto address = addressCidr(pattern, cidr, cidr)) {
            match["ip_cidr"] = {*address};
            return match;
        }
        break;
    }
    }
    ctx.result.error = std::format("全局规则「{}」的匹配地址无效，未启动内核", rule.pattern);
    return std::nullopt;
}

std::string unusedOutboundTag(const nlohmann::json& config, std::string preferred) {
    const auto used = [&](const std::string& tag) {
        for (const char* section : {"outbounds", "endpoints"}) {
            if (!config.contains(section) || !config[section].is_array()) continue;
            for (const auto& item : config[section]) {
                if (item.is_object() && item.value("tag", "") == tag) return true;
            }
        }
        return false;
    };
    const std::string base = preferred;
    for (int index = 1; used(preferred); ++index) preferred = base + "-" + std::to_string(index);
    return preferred;
}

bool applyConnectionRules(Context& ctx) {
#if (!defined(__linux__) && !defined(_WIN32)) || defined(__ANDROID__)
    if (ctx.opt.tunInbound && std::ranges::any_of(ctx.opt.nativeConnections,
            [](const auto& native) { return native.connected; })) {
        ctx.result.error = "当前平台尚未实现原生 VPN 与主 TUN 的补偿路由；请先关闭 TUN";
        return false;
    }
#endif
#ifdef _WIN32
    if (ctx.opt.tunInbound && std::ranges::any_of(ctx.opt.nativeConnections,
            [](const auto& native) { return native.connected && native.kind != vpn::ConnectionKind::Pptp; })) {
        ctx.result.error = "Windows 主 TUN 共存目前支持 PPTP；其他原生 VPN 请先关闭 TUN";
        return false;
    }
#endif
    auto& config = ctx.config;
    if (!config.contains("outbounds")) config["outbounds"] = nlohmann::json::array();
    if (!config["outbounds"].is_array()) {
        ctx.result.error = "sing-box outbounds 必须为数组";
        return false;
    }
    std::string directTag;
    for (const auto& outbound : config["outbounds"]) {
        if (outbound.is_object() && outbound.value("type", "") == "direct" &&
            !outbound.contains("bind_interface") && !outbound.contains("detour")) {
            directTag = outbound.value("tag", "");
            if (!directTag.empty()) break;
        }
    }
    if (directTag.empty()) {
        directTag = unusedOutboundTag(config, "DIRECT");
        config["outbounds"].push_back({{"type", "direct"}, {"tag", directTag}});
    }
    const std::string finalOutbound = config["route"].value("final", directTag);
    std::map<std::string, std::string> nativeTags;
    std::set<std::string> unavailableWarnings;
    for (const auto& native : ctx.opt.nativeConnections) {
        if (native.id.empty() || nativeTags.contains(native.id)) {
            ctx.result.error = "原生 VPN 连接 ID 为空或重复";
            return false;
        }
        std::string tag;
        if (native.connected && !native.interfaceName.empty()) {
            tag = unusedOutboundTag(config, "clash-flux-native-" + std::to_string(nativeTags.size()));
            config["outbounds"].push_back({{"type", "direct"}, {"tag", tag},
                                          {"bind_interface", native.interfaceName}});
        }
        nativeTags.emplace(native.id, std::move(tag));
    }
    const auto applyTarget = [&](nlohmann::json& rule, const std::string& connectionId) {
        if (const auto native = nativeTags.find(connectionId); native != nativeTags.end()) {
            if (!native->second.empty()) {
                rule["outbound"] = native->second;
                return true;
            }
        } else if (!connectionId.empty() && connectionId == ctx.opt.mainConnectionId) {
            rule["outbound"] = finalOutbound;
            return true;
        }
        if (unavailableWarnings.insert(connectionId).second) {
            ctx.warn(std::format("目标连接「{}」未连接或不可用；对应规则已暂停", connectionId));
        }
        return false;
    };

    nlohmann::json precedence = nlohmann::json::array({
        {{"action", "sniff"}}, {{"protocol", "dns"}, {"action", "hijack-dns"}},
    });
    auto globalRules = ctx.opt.globalRules;
    std::stable_sort(globalRules.begin(), globalRules.end(), [](const auto& left, const auto& right) {
        if (left.priority != right.priority) return left.priority > right.priority;
        return ruleSpecificity(left.match) > ruleSpecificity(right.match);
    });
    for (const auto& global : globalRules) {
        if (global.match == vpn::MatchKind::ExactIp &&
            global.pattern.find(':') != std::string::npos &&
            nativeTags.contains(global.connectionId)) {
            ctx.result.error = "原生 VPN 补偿路由当前仅支持 IPv4，不能使用 IPv6 目标规则";
            return false;
        }
        auto rule = globalRuleMatch(ctx, global);
        if (!rule) return false;
        if (applyTarget(*rule, global.connectionId)) precedence.push_back(std::move(*rule));
    }

    struct InternalRule { std::string cidr; std::string connectionId; int prefix; };
    std::vector<InternalRule> internalRules;
    for (const auto& native : ctx.opt.nativeConnections) {
        for (const auto& route : native.internalRoutes) {
            const auto cidr = addressCidr(route, true);
            if (!cidr) {
                ctx.result.error = std::format("原生 VPN「{}」的内网地址「{}」无效；当前仅支持 IPv4/CIDR", native.id, route);
                return false;
            }
            internalRules.push_back({*cidr, native.id, std::stoi(cidr->substr(cidr->find('/') + 1))});
        }
    }
    std::stable_sort(internalRules.begin(), internalRules.end(), [](const auto& left, const auto& right) {
        return left.prefix > right.prefix;
    });
    for (const auto& internal : internalRules) {
        nlohmann::json rule = {{"ip_cidr", {internal.cidr}}};
        if (applyTarget(rule, internal.connectionId)) precedence.push_back(std::move(rule));
    }
    precedence.push_back({{"clash_mode", "direct"}, {"outbound", directTag}});
    precedence.push_back({{"clash_mode", "global"}, {"outbound", finalOutbound}});
    for (auto& rule : config["route"]["rules"]) precedence.push_back(std::move(rule));
    config["route"]["rules"] = std::move(precedence);
    return true;
}

std::string mappedLogLevel(const std::string& level) {
    const std::string lowered = lowerCopy(trimCopy(level));
    if (lowered == "warning") return "warn";
    if (lowered.empty()) return "info";
    return lowered;  // silent/error/info/debug 原样（silent 由调用方转 disabled）
}

void applyManagedSkeleton(Context& ctx, const CompileOptions& opt) {
    nlohmann::json& config = ctx.config;

    // Validate policy input even on platforms that cannot create a managed
    // TUN, so host names never cross the routing boundary as address values.
    for (const auto& address : opt.tunExcludeAddresses) {
        if (!addressCidr(address, false)) {
            ctx.result.error = std::format(
                "TUN 补偿地址「{}」无效，必须为 IP/CIDR", address);
            return;
        }
    }

    for (const auto& native : opt.nativeConnections) {
        if (native.connected && !native.transportAddress.empty() &&
            (!addressCidr(native.transportAddress, false) || native.transportAddress.find_first_of(":/") != std::string::npos)) {
            ctx.result.error = "原生 VPN 传输服务器必须是预解析的 IPv4 地址";
            return;
        }
    }

    nlohmann::json log = nlohmann::json::object();
    if (lowerCopy(trimCopy(opt.logLevel)) == "silent") {
        log["disabled"] = true;
    } else {
        log["level"] = mappedLogLevel(opt.logLevel);
    }
    log["timestamp"] = true;
    config["log"] = std::move(log);

    if (!config.contains("dns")) {
        config["dns"] = {
            {"servers", nlohmann::json::array({
                            {{"type", "local"}, {"tag", "local"}},
                        })},
            {"final", "local"},
            {"strategy", opt.ipv6 ? "prefer_ipv4" : "ipv4_only"},
        };
        if (!config.contains("route")) config["route"] = nlohmann::json::object();
        config["route"]["default_domain_resolver"] = {{"server", "local"}};
    }

    // inbounds：mixed 常驻（系统代理 + 经内核下载订阅），tun 按需。
    if (!config.contains("inbounds") || !config["inbounds"].is_array()) {
        config["inbounds"] = nlohmann::json::array();
    }
    if (!opt.tunInbound) {
        auto& inbounds = config["inbounds"];
        inbounds.erase(std::remove_if(inbounds.begin(), inbounds.end(), [](const auto& inbound) {
            return inbound.is_object() && inbound.value("type", "") == "tun";
        }), inbounds.end());
    }
    bool hasMixed = false;
    bool hasTun = false;
    for (const auto& inbound : config["inbounds"]) {
        const std::string type = inbound.value("type", "");
        if (type == "mixed") hasMixed = true;
        if (type == "tun") {
            if (hasTun) {
                ctx.result.error = "主 VPN 仅允许一个托管 TUN；请移除配置中的额外 TUN 入站";
                return;
            }
            hasTun = true;
        }
    }
    if (!hasMixed) {
        config["inbounds"].push_back({
            {"type", "mixed"},
            {"tag", "mixed-in"},
            {"listen", opt.allowLan ? "0.0.0.0" : "127.0.0.1"},
            {"listen_port", opt.mixedPort},
        });
    }
    if (opt.tunInbound && !hasTun) {
        nlohmann::json tun = {
            {"type", "tun"},
            {"tag", "tun-in"},
            {"address", nlohmann::json::array({"172.19.0.1/30"})},
            {"mtu", 1400},
            {"auto_route", true},
            {"strict_route", opt.tunStrictRoute},
        };
#if defined(__ANDROID__)
        // Android's VpnService hands libbox a userspace TUN descriptor.  The
        // system TCP stack can observe the connection and run sniffing, but
        // on the test device it never forwards the TCP stream to the selected
        // outbound (the port proxy works with the same node).  Use the
        // userspace stack for both TCP and UDP so the TUN data plane is
        // independent of the ROM's kernel-network integration.
        tun["stack"] = "gvisor";
#endif
        config["inbounds"].push_back(std::move(tun));
    }
    // Apply ownership and transport compensation to imported TUNs as well.
    // Data destinations stay inside TUN so explicit global rules win over a
    // native connection's implicit CIDRs. Only transport addresses bypass it.
    for (auto& inbound : config["inbounds"]) {
        if (!inbound.is_object() || inbound.value("type", "") != "tun") continue;
        inbound["auto_route"] = true;
        inbound["strict_route"] = opt.tunStrictRoute;
#ifdef _WIN32
        if (inbound.value("interface_name", "").empty()) inbound["interface_name"] = "ClashFlux";
        if (!opt.nativeConnections.empty()) {
            if (std::ranges::any_of(opt.nativeConnections, [](const auto& native) { return native.connected; }) &&
                (inbound.contains("route_address") || inbound.contains("route_address_set") ||
                 inbound.contains("route_exclude_address_set") || inbound.contains("inet4_route_address") ||
                 inbound.contains("inet4_route_exclude_address"))) {
                ctx.result.error = "Windows PPTP 共存需要默认全流量 TUN；请移除自定义 route_address/地址规则集/旧版地址字段";
                return;
            }
        }
#endif
#if defined(__ANDROID__)
        inbound["stack"] = "gvisor";
#elif defined(__linux__)
        // Native transport rules use priorities before 9000; native data
        // routes after it. auto_redirect's nftables interception would bypass
        // that ordering, so all managed Linux TUNs use policy routing.
        inbound["iproute2_rule_index"] = 9000;
        if (inbound.value("auto_redirect", false)) {
            ctx.warn("托管 TUN 已关闭 auto_redirect，以保证原生 VPN 补偿路由优先级");
        }
        inbound["auto_redirect"] = false;
#endif
        nlohmann::json exclusions = nlohmann::json::array();
        if (inbound.contains("route_exclude_address")) {
            const auto& existing = inbound["route_exclude_address"];
            if (existing.is_string()) exclusions.push_back(existing);
            else if (existing.is_array()) exclusions = existing;
            else {
                ctx.result.error = "TUN route_exclude_address 必须为地址字符串或数组";
                return;
            }
        }
        auto appendExclusion = [&](const std::string& address) {
            if (std::find(exclusions.begin(), exclusions.end(), nlohmann::json(address)) == exclusions.end()) {
                exclusions.push_back(address);
            }
        };
#if !defined(__ANDROID__)
        appendExclusion("127.0.0.0/8");
#endif
        for (const auto& address : opt.tunExcludeAddresses) {
            appendExclusion(*addressCidr(address, false));
        }
        for (const auto& native : opt.nativeConnections) {
            if (native.connected && !native.transportAddress.empty())
                appendExclusion(*addressCidr(native.transportAddress, false));
        }
        if (!exclusions.empty()) inbound["route_exclude_address"] = std::move(exclusions);
    }

    if (!config.contains("route") || !config["route"].is_object()) {
        config["route"] = nlohmann::json::object();
    }
    if (!config["route"].contains("final")) {
        // Native JSON without final uses the first outbound, matching
        // sing-box's own default instead of referencing a missing DIRECT.
        std::string final = "DIRECT";
        if (config.contains("outbounds") && config["outbounds"].is_array() &&
            !config["outbounds"].empty() && config["outbounds"][0].is_object()) {
            final = config["outbounds"][0].value("tag", final);
        }
        config["route"]["final"] = std::move(final);
    }
    if (!config["route"].contains("rules") || !config["route"]["rules"].is_array()) {
        config["route"]["rules"] = nlohmann::json::array();
    }
    if (opt.tunInbound) config["route"]["auto_detect_interface"] = true;

    if (!config.contains("experimental") || !config["experimental"].is_object()) {
        config["experimental"] = nlohmann::json::object();
    }
#if defined(__ANDROID__)
    // Android libbox is controlled through CommandServer/CommandClient.  It
    // has no desktop REST controller on 127.0.0.1:9097, so remove a
    // clash_api left by a native sing-box profile as well as the managed
    // desktop skeleton.  Keeping this out of the Android config also avoids
    // starting an unnecessary second controller inside the libbox service.
    config["experimental"].erase("clash_api");
#else
    nlohmann::json clashApi = config["experimental"].contains("clash_api") &&
                                      config["experimental"]["clash_api"].is_object()
                                  ? config["experimental"]["clash_api"]
                                  : nlohmann::json::object();
    clashApi["external_controller"] = opt.controller;
    if (!opt.secret.empty()) clashApi["secret"] = opt.secret;
    // mode_list 不能写进配置（1.14 里是 json:"-"，写了直接拒绝启动）：
    // 内核从 route rules 的 clash_mode 值自动推导模式列表，default_mode
    // 不在列表里时会被自动补头——因此小写 rule/global/direct 全部可用。
    clashApi["default_mode"] = opt.mode;
    config["experimental"]["clash_api"] = std::move(clashApi);
#endif
    config["experimental"]["cache_file"] = {{"enabled", true}};
}

// 解析 Clash YAML 文档并转换节点/组/规则（调用方负责异常捕获）。
void compileClashDocument(Context& ctx, const std::string& trimmed) {
    YAML::Node root = YAML::Load(trimmed);
    if (!root.IsMap()) {
        ctx.result.error = "订阅不是有效的 Clash YAML 配置";
        return;
    }

    std::vector<YAML::Node> groups;
    if (const YAML::Node proxyGroups = root["proxy-groups"];
        proxyGroups && proxyGroups.IsSequence()) {
        for (const YAML::Node& group : proxyGroups) {
            if (group.IsMap()) groups.push_back(group);
        }
    }
    // 先登记组名：组可以引用其后的组，规则目标校验也要用。
    for (const YAML::Node& group : groups) {
        const std::string name = ytext(group, "name");
        if (!name.empty() && !ctx.tagKnown(name)) ctx.knownTags.push_back(name);
    }

    // Clash 的 nameserver 不是 sing-box 1.14 可接受的 legacy 字符串格式。
    // 先登记代理组，再转换 `#出站组` 绑定，保证 DoH/DoT 能沿指定代理出站。
    applyClashDns(ctx, root["dns"]);

    if (const YAML::Node proxies = root["proxies"]; proxies && proxies.IsSequence()) {
        for (const YAML::Node& proxy : proxies) {
            if (!proxy.IsMap()) continue;
            if (auto converted = convertProxy(ctx, proxy)) {
                ctx.knownTags.push_back(converted->at("tag").get<std::string>());
                ctx.outbounds.push_back(std::move(*converted));
            }
        }
    }

    for (const YAML::Node& group : groups) {
        const std::string name = ytext(group, "name");
        if (name.empty()) continue;
        std::vector<std::string> members = ylist(group, "proxies");
        if (ybool(group, "include-all") || !ytext(group, "use").empty()) {
            ctx.warn(std::format("代理组「{}」使用了 provider（use/include-all），"
                                 "sing-box 编译暂不支持，仅保留显式成员",
                                 name));
        }
        if (auto converted = convertGroup(ctx, group, name, members)) {
            ctx.outbounds.push_back(std::move(*converted));
        } else {
            ctx.warn(std::format("代理组「{}」编译失败，已跳过", name));
        }
    }

    if (const YAML::Node rules = root["rules"]; rules && rules.IsSequence()) {
        for (const YAML::Node& rule : rules) {
            if (rule.IsScalar()) convertRule(ctx, rule.Scalar());
        }
    }
}

} // namespace

CompileResult compileConfig(const CompileOptions& options) {
    Context ctx{options};
    const std::string trimmed = trimCopy(options.profileYaml);

    if (!trimmed.empty() && trimmed.front() == '{') {
        // 原生 sing-box JSON：直通，只合并托管设置。
        nlohmann::json parsed = nlohmann::json::parse(trimmed, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            ctx.result.error = "原生 sing-box 配置不是合法的 JSON 对象";
            return std::move(ctx.result);
        }
        ctx.config = std::move(parsed);
        try {
            applyManagedSkeleton(ctx, options);
            if (!ctx.result.error.empty() || !applyConnectionRules(ctx)) return std::move(ctx.result);
        } catch (const std::exception& error) {
            ctx.result.error = std::format("原生 sing-box 配置合并失败：{}", error.what());
            return std::move(ctx.result);
        }
        ctx.result.json = ctx.config.dump();
        return std::move(ctx.result);
    }

    ctx.config = nlohmann::json::object();
    applyManagedSkeleton(ctx, options);
    if (!ctx.result.error.empty()) return std::move(ctx.result);
    // DIRECT 是普通 outbound；REJECT 在 1.14 已移除，由规则 action 承担。
    ctx.outbounds.push_back({{"type", "direct"}, {"tag", "DIRECT"}});
    ctx.knownTags.push_back("DIRECT");

    if (!trimmed.empty()) {
        // 订阅内容不可控：任何 yaml-cpp 访问异常都降级为编译错误，不崩应用。
        try {
            compileClashDocument(ctx, trimmed);
        } catch (const std::exception& error) {
            ctx.result.error = std::format("订阅 YAML 解析失败：{}", error.what());
            return std::move(ctx.result);
        }
    }

    // final：MATCH 目标优先；REJECT 目标转成兜底 reject 规则；缺省回落首个组。
    std::string finalOutbound = ctx.finalTarget;
    if (finalOutbound == "REJECT" || finalOutbound == "REJECT-DROP") {
        ctx.config["route"]["rules"].push_back({{"action", "reject"}});
        finalOutbound = "DIRECT";
    }
    if (finalOutbound.empty() || finalOutbound == "PASS") finalOutbound = "DIRECT";
    if (finalOutbound != "DIRECT" && !ctx.tagKnown(finalOutbound)) {
        ctx.warn(std::format("MATCH 目标「{}」不存在，回落 DIRECT", finalOutbound));
        finalOutbound = "DIRECT";
    }
    if (finalOutbound == "DIRECT") {
        // 无订阅/无 MATCH：默认全局指向首个 selector 组，保持「有订阅即可用代理」。
        for (const auto& outbound : ctx.outbounds) {
            if (outbound.value("type", "") == "selector") {
                finalOutbound = outbound.value("tag", "DIRECT");
                break;
            }
        }
    }
    ctx.config["route"]["final"] = finalOutbound;

    if (!ctx.ruleSets.empty()) {
        ctx.config["route"]["rule_set"] = std::move(ctx.ruleSets);
    }
    ctx.config["outbounds"] = std::move(ctx.outbounds);
    if (!applyConnectionRules(ctx)) return std::move(ctx.result);
    ctx.result.json = ctx.config.dump();
    return std::move(ctx.result);
}

} // namespace singbox
