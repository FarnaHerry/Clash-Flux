// singbox_context_dns.inc — 编译上下文与 Clash DNS 转换
// 由 src/singbox.cpp 在同一匿名命名空间中包含。

struct Context {
    const CompileOptions& opt;
    CompileResult result;
    nlohmann::json config;
    nlohmann::json outbounds = nlohmann::json::array();
    nlohmann::json ruleSets = nlohmann::json::array();
    std::vector<std::string> knownTags;               // 节点 + 组 + DIRECT
    std::map<std::string, std::string> geoipTags;     // 国家码 → rule_set tag
    std::map<std::string, std::string> geositeTags;   // 站点类别 → rule_set tag
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
