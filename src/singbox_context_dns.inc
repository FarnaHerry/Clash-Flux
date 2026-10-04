// singbox_context_dns.inc — 编译上下文与 Clash DNS 转换
// 由 src/singbox.cpp 在同一匿名命名空间中包含。

struct SourceDefault { std::string tag; bool reject = false; bool drop = false; };

struct Context {
    const CompileOptions& opt;
    CompileResult result;
    bool providerSyntaxOnly = false; // HTTP body validation, never a published candidate.
    nlohmann::json config;
    nlohmann::json outbounds = nlohmann::json::array();
    nlohmann::json ruleSets = nlohmann::json::array();
    std::vector<std::string> knownTags;               // 节点 + 组 + DIRECT
    std::map<std::string, std::string> geoipTags;     // 国家码 → rule_set tag
    std::map<std::string, std::string> geositeTags;   // 站点类别 → rule_set tag
    std::map<std::string, std::string> dnsServerTags; // Clash DNS 地址 → typed server tag
    std::map<std::string, std::string> providerTags; // source name -> inline native tag
    std::set<std::string> declaredProviders;        // rejected declarations also shadow aliases
    std::string dnsBootstrapTag = "local";
    nlohmann::json hostsDomains = nlohmann::json::array(); // Main-only connection resolution.
    std::string hostsDnsTag;
    std::map<std::string, SourceDefault> sourceDefaults;
    std::string sourceId;
    std::string defaultFingerprint = "chrome";       // Scoped to this Clash source.
    std::string finalTarget;                          // MATCH 目标

    explicit Context(const CompileOptions& options) : opt(options) {}

    void warn(std::string message) { result.warnings.push_back(std::move(message)); }

    // 结构化保真度条目（见 docs/singbox-layers-and-fidelity.md §2）。detail 同时
    // 并入 warnings，保证历史消费方（测试、内核启动诊断、日志）看到的文本不缩水。
    void note(FidelityScope scope, Fidelity level, std::string subject,
              std::string detail, std::string action = {}) {
        result.fidelity.push_back(FidelityNote{scope, level, std::move(subject),
                                               std::move(detail),
                                               std::move(action), sourceId});
        result.warnings.push_back(result.fidelity.back().detail);
    }
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
    bool http3 = false;
    std::optional<bool> skipCertVerify;
};

int dnsDefaultPort(std::string_view scheme) {
    if (scheme == "tls" || scheme == "quic") return 853;
    if (scheme == "https" || scheme == "h3") return 443;
    return 53;
}

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
    bool hasH3 = false;
    if (const std::size_t fragment = value.find('#'); fragment != std::string::npos) {
        std::string_view parameters{value.data() + fragment + 1, value.size() - fragment - 1};
        if (parameters.empty()) return std::nullopt;
        while (!parameters.empty()) {
            const auto separator = parameters.find('&');
            const std::string parameter{parameters.substr(0, separator)};
            if (parameter != trimCopy(parameter)) return std::nullopt;
            if (parameter == "h3=true" || parameter == "h3=false") {
                if (hasH3) return std::nullopt;
                hasH3 = true;
                endpoint.http3 = parameter == "h3=true";
            } else if (parameter == "skip-cert-verify=true" || parameter == "skip-cert-verify=false") {
                if (endpoint.skipCertVerify.has_value()) return std::nullopt;
                endpoint.skipCertVerify = parameter == "skip-cert-verify=true";
            } else if (parameter.empty() || parameter.find('=') != std::string::npos ||
                       parameter == "RULES" || !endpoint.detour.empty()) {
                return std::nullopt;
            } else {
                endpoint.detour = parameter;
            }
            if (separator == std::string_view::npos) break;
            parameters.remove_prefix(separator + 1);
            if (parameters.empty()) return std::nullopt;
        }
        value.resize(fragment);
    }

    const std::size_t schemeEnd = value.find("://");
    endpoint.scheme = schemeEnd == std::string::npos ? "udp" : lowerCopy(value.substr(0, schemeEnd));
    if (hasH3 && endpoint.scheme != "https") return std::nullopt;
    if (endpoint.skipCertVerify.has_value() && endpoint.scheme != "tls" &&
        endpoint.scheme != "https" && endpoint.scheme != "quic") return std::nullopt;
    if (endpoint.http3) endpoint.scheme = "h3";
    std::string authorityAndPath = schemeEnd == std::string::npos ? value : value.substr(schemeEnd + 3);
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
        if (colon != std::string::npos && authority.find(':') == colon) {
            if (!isPortText(std::string_view{authority}.substr(colon + 1))) return std::nullopt;
            endpoint.host = authority.substr(0, colon);
            endpoint.port = std::stoi(authority.substr(colon + 1));
        } else {
            endpoint.host = authority;
        }
    }
    if (endpoint.host.empty()) return std::nullopt;
    if (endpoint.port == 0) {
        endpoint.port = dnsDefaultPort(endpoint.scheme);
    }
    return endpoint;
}

bool dnsHostIsDomain(std::string_view host) {
    if (host.find(':') != std::string_view::npos) return false;
    bool hasLetter = false;
    for (const unsigned char c : host) {
        if (std::isalpha(c)) hasLetter = true;
        if (std::isalpha(c) || std::isdigit(c) || c == '.' || c == '-' || c == '_') continue;
        return false;
    }
    return hasLetter;
}

std::optional<std::string> addressCidr(std::string_view raw, bool ipv4Only, bool requirePrefix);

std::string appendDnsServer(Context& ctx, std::string_view rawValue) {
    const std::string raw = trimCopy(rawValue);
    if (raw.empty()) {
        ctx.note(FidelityScope::Dns, Fidelity::Unsupported, "<空地址>", "DNS 服务器地址为空，已忽略");
        return {};
    }
    if (const auto known = ctx.dnsServerTags.find(raw); known != ctx.dnsServerTags.end()) {
        return known->second;
    }
    if (raw == "system") return "local";

    // Typed DHCP/FakeIP exist in the kernel but their Clash input conversion
    // is not implemented here. Do not describe an application gap as a core limit.
    const std::optional<DnsEndpoint> endpoint = parseDnsEndpoint(raw);
    if (!endpoint || endpoint->scheme == "rcode" || endpoint->scheme == "fakeip" ||
        endpoint->scheme == "dhcp") {
        ctx.note(FidelityScope::Dns, Fidelity::Unsupported, raw,
                 std::format("DNS 服务器「{}」格式暂不支持，已忽略", raw),
                 "该地址或附加参数尚未转换；支持 #出站名、HTTPS h3=true/false、"
                 "加密 DNS skip-cert-verify=true/false 及其组合，"
                 "其它参数不会被当成出站名后静默丢弃");
        return {};
    }
    const std::string& scheme = endpoint->scheme;
    if (scheme != "udp" && scheme != "tcp" && scheme != "tls" && scheme != "https" &&
        scheme != "quic" && scheme != "h3") {
        ctx.note(FidelityScope::Dns, Fidelity::Unsupported, raw,
                 std::format("DNS 服务器「{}」协议 {} 暂不支持，已忽略", raw, scheme),
                 "当前转换支持 udp/tcp/tls/https/quic 与 HTTPS #h3=true；"
                 "http:// 不会自动改成 HTTPS");
        return {};
    }
    if ((!endpoint->path.empty() && scheme != "https" && scheme != "h3") ||
        endpoint->host.find_first_of(" \t\r\n?@#") != std::string::npos) {
        ctx.note(FidelityScope::Dns, Fidelity::Unsupported, raw,
                 std::format("DNS 服务器「{}」地址无效，已忽略", raw),
                 "非 DoH 传输不接受 URL 路径；服务器地址不能包含用户信息或查询参数");
        return {};
    }

    const std::string tag = std::format("dns-{}", ctx.dnsServerTags.size());
    nlohmann::json server = {
        {"type", scheme},
        {"tag", tag},
        {"server", endpoint->host},
    };
    const int defaultPort = dnsDefaultPort(scheme);
    if (endpoint->port != defaultPort) server["server_port"] = endpoint->port;
    if (endpoint->skipCertVerify.has_value()) {
        // Only the explicit source flag may relax this endpoint's trust.
        // Never change server_name/SNI or the protected bootstrap resolver.
        server["tls"] = {{"enabled", true}, {"insecure", *endpoint->skipCertVerify}};
    }
    if ((scheme == "https" || scheme == "h3") && !endpoint->path.empty() && endpoint->path != "/dns-query") {
        server["path"] = endpoint->path;
    }
    if (dnsHostIsDomain(endpoint->host)) {
        // Endpoint bootstrap is independent of payload routing. The default
        // protected local resolver can be replaced by explicit IP DNS servers.
        server["domain_resolver"] = {{"server", ctx.dnsBootstrapTag}};
    }
    if (!endpoint->detour.empty()) {
        // knownTags also contains forward declarations for groups. Only an
        // actual compiled outbound is a valid DNS detour.
        if (std::ranges::any_of(ctx.outbounds, [&](const auto& outbound) {
                return outbound.value("tag", "") == endpoint->detour;
            })) {
            server["detour"] = endpoint->detour;
        } else {
            ctx.note(FidelityScope::Dns, Fidelity::Unsupported, raw,
                     std::format("DNS 服务器「{}」引用了未知出站「{}」，已忽略",
                                 raw, endpoint->detour),
                     "接口绑定暂未映射；确认代理名是否写错或出站是否被跳过");
            return {};
        }
    }
    ctx.config["dns"]["servers"].push_back(std::move(server));
    ctx.dnsServerTags.emplace(raw, tag);
    return tag;
}

std::string selectDnsServer(Context& ctx, const YAML::Node& value, const std::string& subject,
                            bool bootstrapOnly = false) {
    if (!value) return {};
    std::vector<std::string> addresses;
    if (value.IsScalar()) addresses.push_back(trimCopy(value.Scalar()));
    else if (value.IsSequence()) {
        for (const auto& address : value) {
            if (!address.IsScalar()) {
                ctx.note(FidelityScope::Dns, Fidelity::Unsupported, subject,
                         std::format("DNS「{}」必须为地址或地址数组，已忽略", subject));
                return {};
            }
            addresses.push_back(trimCopy(address.Scalar()));
        }
    } else {
        ctx.note(FidelityScope::Dns, Fidelity::Unsupported, subject,
                 std::format("DNS「{}」必须为地址或地址数组，已忽略", subject));
        return {};
    }
    std::string primary;
    for (const auto& raw : addresses) {
        if (bootstrapOnly) {
            const auto endpoint = parseDnsEndpoint(raw);
            if (!endpoint || !endpoint->detour.empty() ||
                endpoint->host.find('/') != std::string::npos ||
                !addressCidr(endpoint->host, false, false)) {
                ctx.note(FidelityScope::Dns, Fidelity::Unsupported, subject,
                         std::format("DNS bootstrap「{}」必须使用 IP 地址且不依赖代理，已忽略", raw),
                         "域名或代理链会引入未定义的解析依赖；保留加密 IP DNS 的证书校验");
                continue;
            }
        }
        const std::string tag = appendDnsServer(ctx, raw);
        if (primary.empty() && !tag.empty()) primary = tag;
    }
    if (addresses.size() > 1 && !primary.empty()) {
        ctx.note(FidelityScope::Dns, Fidelity::Approx, subject,
                 std::format("DNS「{}」使用首个可转换服务器，不执行并发查询或故障切换", subject),
                 "原生 DNS route/final 绑定单个服务器；数组中的其余服务器不会作为后备使用");
    }
    return primary;
}

std::optional<std::string> dnsPolicyDomain(std::string domain) {
    domain = lowerCopy(trimCopy(domain));
    if (!domain.empty() && domain.back() == '.') domain.pop_back();
    if (domain.empty() || domain.size() > 253) return std::nullopt;
    std::size_t labelSize = 0;
    for (const unsigned char c : domain) {
        if (c == '.') {
            if (labelSize == 0 || labelSize > 63) return std::nullopt;
            labelSize = 0;
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
            ++labelSize;
        } else return std::nullopt;
    }
    if (labelSize == 0 || labelSize > 63) return std::nullopt;
    return domain;
}

struct DnsPolicyPattern {
    std::vector<std::string> labels;
    bool subdomains = false; // One or more extra labels; never the root.

    std::vector<unsigned> priority() const {
        std::vector<unsigned> result;
        for (auto it = labels.rbegin(); it != labels.rend(); ++it)
            result.push_back(*it == "*" ? 1 : 2);
        if (subdomains) result.push_back(0);
        return result;
    }
    bool matches(const std::vector<std::string>& host) const {
        if (subdomains ? host.size() <= labels.size() : host.size() != labels.size()) return false;
        for (std::size_t i = 0; i < labels.size(); ++i) {
            const auto& label = labels[labels.size() - i - 1];
            if (label != "*" && label != host[host.size() - i - 1]) return false;
        }
        return true;
    }
    std::pair<std::string, std::string> fieldValue() const {
        const bool wildcard = std::ranges::find(labels, "*") != labels.end();
        std::string expression;
        for (const auto& label : labels) {
            if (!expression.empty()) expression += wildcard || subdomains ? "\\." : ".";
            expression += label == "*" ? "[^.]+" : label;
        }
        if (!wildcard && !subdomains) return {"domain", std::move(expression)};
        // Generated from validated literal labels, with no user regex syntax.
        return {"domain_regex", std::string{"^"} + (subdomains ? "([^.]+\\.)+" : "") + expression + "$"};
    }
    nlohmann::json condition() const {
        auto [field, value] = fieldValue();
        return {{field, nlohmann::json::array({std::move(value)})}};
    }
};

std::vector<std::string> dnsDomainLabels(std::string_view domain) {
    std::vector<std::string> labels;
    while (!domain.empty()) {
        const auto dot = domain.find('.');
        labels.emplace_back(domain.substr(0, dot));
        if (dot == std::string_view::npos) break;
        domain.remove_prefix(dot + 1);
    }
    return labels;
}

std::vector<DnsPolicyPattern> dnsPolicyPatterns(std::string pattern) {
    pattern = lowerCopy(trimCopy(pattern));
    const bool includeRoot = pattern.starts_with("+.");
    const bool subdomains = includeRoot || pattern.starts_with('.');
    if (subdomains) pattern.erase(0, includeRoot ? 2 : 1);
    if (!pattern.empty() && pattern.back() == '.') pattern.pop_back(); // Existing canonicalization.
    if (pattern.empty() || pattern.back() == '.' || pattern.size() > 253) return {};
    auto labels = dnsDomainLabels(pattern);
    for (const auto& label : labels) {
        if (label.empty() || label.size() > 63) return {};
        if (label == "*") continue;
        for (const unsigned char c : label)
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return {};
    }
    std::vector<DnsPolicyPattern> result;
    if (includeRoot) result.push_back({labels, false});
    result.push_back({std::move(labels), subdomains});
    return result;
}

struct DnsPolicyRule {
    DnsPolicyPattern pattern;
    std::vector<unsigned> priority;
    nlohmann::json rule;
};

std::vector<DnsPolicyRule> compileDnsPolicy(Context& ctx, const YAML::Node& policy,
                                           const std::string& subject) {
    if (!policy) return {};
    if (!policy.IsMap()) {
        ctx.note(FidelityScope::Dns, Fidelity::Unsupported, subject,
                 subject + " 必须为域名到 DNS 地址的映射，已忽略");
        return {};
    }
    std::vector<DnsPolicyRule> rules;
    std::map<std::pair<std::vector<std::string>, bool>, std::size_t> branches;
    for (const auto& item : policy) {
        const std::string pattern = item.first.IsScalar() ? trimCopy(item.first.Scalar()) : "<非标量>";
        auto patterns = dnsPolicyPatterns(pattern);
        if (patterns.empty()) {
            ctx.note(FidelityScope::Dns, Fidelity::Unsupported, subject + ":" + pattern,
                     std::format("DNS 策略「{}」匹配模式尚未转换，已忽略", pattern),
                     "支持完整域名、整层 *、前缀 . 与 +.；部分标签通配、geosite:、rule-set: "
                     "和其它格式不会改写成更宽的匹配");
            continue;
        }
        const std::string tag = selectDnsServer(ctx, item.second, subject + ":" + pattern);
        if (tag.empty()) {
            ctx.note(FidelityScope::Dns, Fidelity::Unsupported, subject + ":" + pattern,
                     std::format("DNS 策略「{}」没有可转换服务器，已忽略", pattern));
            continue;
        }
        for (auto& matcher : patterns) {
            auto rule = matcher.condition();
            rule["action"] = "route";
            rule["server"] = tag;
            auto priority = matcher.priority();
            const auto [slot, inserted] = branches.try_emplace(
                std::pair{matcher.labels, matcher.subdomains}, rules.size());
            DnsPolicyRule compiled{std::move(matcher), std::move(priority), std::move(rule)};
            if (!inserted) {
                // Clash Insert overwrites this trie branch in declaration order;
                // +. contributes independent root and subdomain branches.
                rules[slot->second] = std::move(compiled);
                ctx.note(FidelityScope::Dns, Fidelity::Exact, subject + ":" + pattern,
                         "后声明 DNS 策略覆盖同一规范化匹配分支，保持 Clash 的插入顺序语义");
            } else rules.push_back(std::move(compiled));
        }
    }
    // Trie DFS examines labels from right to left: literal, *, then dot.
    // Reverse-label lexicographic rank reproduces that choice among matching
    // patterns; total length or a global "exact first" sort cannot do so.
    std::stable_sort(rules.begin(), rules.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.priority > rhs.priority;
    });
    return rules;
}

// Both ordinary DNS and static node resolution use the same ordered matcher.
// Metadata stays outside emitted JSON; node selection does not reinterpret Go
// regex using a different regex engine or maintain an independent priority.
std::string policyResolver(const std::vector<DnsPolicyRule>& rules, const std::string& host,
                           std::string fallback) {
    const auto domain = dnsPolicyDomain(host);
    if (!domain) return fallback;
    const auto labels = dnsDomainLabels(*domain);
    for (const auto& rule : rules)
        if (rule.pattern.matches(labels)) return rule.rule.value("server", fallback);
    return fallback;
}

void applyClashDns(Context& ctx, const YAML::Node& dns) {
    if (!dns) return;
    if (!dns.IsMap()) {
        ctx.note(FidelityScope::Dns, Fidelity::Unsupported, "dns", "Clash dns 必须为映射，已忽略");
        return;
    }
    for (const auto* key : {"enable", "ipv6"}) {
        if (!dns[key]) continue;
        const std::string value = lowerCopy(ytext(dns, key));
        if (value != "true" && value != "yes" && value != "1" &&
            value != "false" && value != "no" && value != "0") {
            ctx.note(FidelityScope::Dns, Fidelity::Unsupported, std::string{"dns."} + key,
                     std::format("DNS 字段「{}」不是布尔值，已使用默认行为", key));
        }
    }
    const std::string enabled = lowerCopy(ytext(dns, "enable"));
    if (enabled == "false" || enabled == "no" || enabled == "0") {
        // The managed local server is the system resolver. DNS disabled in
        // the source must not leave its custom policy active.
        return;
    }
    for (const auto& item : dns) {
        const std::string key = item.first.IsScalar() ? item.first.Scalar() : "<非标量>";
        if (key == "enable" || key == "ipv6" || key == "nameserver" || key == "fallback" ||
            key == "nameserver-policy" || key == "default-nameserver" || key == "use-hosts" ||
            key == "proxy-server-nameserver" || key == "proxy-server-nameserver-policy" || key == "direct-nameserver" || (key == "enhanced-mode" && ytext(dns, "enhanced-mode") == "redir-host")) continue;
        ctx.note(FidelityScope::Dns, Fidelity::Unsupported, "dns." + key,
                 std::format("Clash DNS 字段「{}」尚未转换，已忽略", key),
                 "这是输入转换的缺口；需要完整原生策略时使用 sing-box JSON，不能据此认定内核不支持");
    }
    if (const auto bootstrap = selectDnsServer(ctx, dns["default-nameserver"], "default-nameserver", true);
        !bootstrap.empty()) ctx.dnsBootstrapTag = bootstrap;
    const bool hasNameserver = dns["nameserver"] &&
        (!dns["nameserver"].IsSequence() || dns["nameserver"].size() != 0);
    const auto primary = selectDnsServer(ctx, hasNameserver ? dns["nameserver"] : dns["fallback"],
                                         hasNameserver ? "nameserver" : "fallback");
    if (!primary.empty()) ctx.config["dns"]["final"] = primary;
    if (dns["fallback"] && (!dns["fallback"].IsSequence() || dns["fallback"].size() != 0)) {
        ctx.note(FidelityScope::Dns, Fidelity::Approx, "fallback",
                 hasNameserver ? "Clash fallback 未参与 DNS 查询；保留 nameserver 作为默认解析路径"
                               : "Clash fallback 被用作默认 DNS，不执行污染判断或后备查询",
                 "尚未实现 fallback-filter 与并发后备语义");
    }
    ctx.config["dns"]["strategy"] = ybool(dns, "ipv6") ? "prefer_ipv4" : "ipv4_only";
    const auto policy = compileDnsPolicy(ctx, dns["nameserver-policy"], "nameserver-policy");
    if (!policy.empty()) {
        auto rules = nlohmann::json::array();
        for (const auto& compiled : policy) rules.push_back(compiled.rule);
        ctx.config["dns"]["rules"] = std::move(rules);
    }
    const std::string proxyResolver = selectDnsServer(ctx, dns["proxy-server-nameserver"],
                                                     "proxy-server-nameserver");
    const auto proxyPolicy = proxyResolver.empty() ? std::vector<DnsPolicyRule>{}
        : compileDnsPolicy(ctx, dns["proxy-server-nameserver-policy"], "proxy-server-nameserver-policy");
    const std::string directResolver = selectDnsServer(ctx, dns["direct-nameserver"], "direct-nameserver");
    const std::string strategy = ybool(dns, "ipv6") ? "prefer_ipv4" : "ipv4_only";
    for (auto& outbound : ctx.outbounds) {
        if (outbound.value("type", "") == "direct") {
            if (!directResolver.empty()) outbound["domain_resolver"] = {
                {"server", directResolver}, {"strategy", strategy}};
            continue;
        }
        const std::string sourceHost = lowerCopy(outbound.value("server", ""));
        if (!dnsHostIsDomain(sourceHost)) continue;
        const std::string host = dnsPolicyDomain(sourceHost).value_or(sourceHost);
        std::string resolver;
        if (!proxyResolver.empty()) {
            resolver = policyResolver(proxyPolicy, host, proxyResolver);
        } else {
            resolver = policyResolver(policy, host, ctx.config["dns"].value("final", "local"));
        }
        outbound["domain_resolver"] = {{"server", resolver}, {"strategy", strategy}};
    }
    if (!directResolver.empty()) {
        ctx.note(FidelityScope::Dns, Fidelity::Approx, "direct-nameserver",
                 "已设置 DIRECT 的原生解析器；托管路由前置 resolve 已解析的连接不会再次经它查询",
                 "仅覆盖到达 DIRECT 时仍为域名的请求，不宣称复现全部 Clash 直连解析时机");
    }
}

void applyClashHosts(Context& ctx, const YAML::Node& root, bool exportOnly) {
    const auto dns = root["dns"];
    bool useHosts = true;
    if (dns && dns.IsMap() && dns["use-hosts"]) {
        const auto flag = dns["use-hosts"];
        const auto text = flag.IsScalar() ? lowerCopy(flag.Scalar()) : std::string{};
        if (text == "false" || text == "no" || text == "0") useHosts = false;
        else if (text != "true" && text != "yes" && text != "1") {
            useHosts = false;
            ctx.note(FidelityScope::Dns, Fidelity::Unsupported, "dns.use-hosts",
                     "dns.use-hosts 不是布尔值，不启用配置 hosts 的 DNS 应答");
        }
    }
    const auto hosts = root["hosts"];
    if (!hosts) return;
    const auto reject = [&](const std::string& subject, std::string reason) {
        ctx.note(FidelityScope::Dns, Fidelity::Unsupported, subject,
                 "Clash hosts 条目" + std::move(reason) + "，整条未转换",
                 "仅支持精确域名到 IP/完整 IP 数组；不裁掉坏地址或扩大通配匹配");
    };
    if (!hosts.IsMap()) { reject("hosts", "必须为映射"); return; }
    std::map<std::string, unsigned> counts;
    for (const auto& entry : hosts) if (entry.first.IsScalar()) {
        const auto domain = dnsPolicyDomain(entry.first.Scalar());
        if (domain) ++counts[*domain];
    }
    std::set<std::string> reported;
    nlohmann::json predefined = nlohmann::json::object();
    for (const auto& entry : hosts) {
        const auto raw = entry.first.IsScalar() ? entry.first.Scalar() : std::string{};
        const auto domain = dnsPolicyDomain(raw);
        const auto subject = raw.empty() ? "hosts" : "hosts." + raw;
        if (!domain || raw != trimCopy(raw) || addressCidr(raw, false, false)) {
            reject(subject, "名称或匹配模式未接入"); continue;
        }
        if (counts[*domain] != 1) {
            if (reported.insert(*domain).second) reject(subject, "规范化域名重复，所有同名映射拒绝");
            continue;
        }
        std::vector<YAML::Node> values;
        if (entry.second.IsScalar()) values.push_back(entry.second);
        else if (entry.second.IsSequence()) for (const auto& value : entry.second) values.push_back(value);
        nlohmann::json addresses = nlohmann::json::array();
        bool valid = !values.empty();
        for (const auto& value : values) {
            if (!value.IsScalar()) { valid = false; break; }
            const auto address = value.Scalar();
            if (address != trimCopy(address) || address.find('/') != std::string::npos ||
                // Mihomo unmaps mapped IPv6; this subset must not classify it
                // as native AAAA and silently change its address family.
                (address.find(':') != std::string::npos && address.find('.') != std::string::npos) ||
                !addressCidr(address, false, false)) { valid = false; break; }
            addresses.push_back(address);
        }
        if (!valid) { reject(subject, "地址或列表未接入（别名、lan、CIDR、异常成员等）"); continue; }
        predefined[*domain] = std::move(addresses);
    }
    if (predefined.empty()) return;
    constexpr auto tag = "clash-hosts";
    ctx.config["dns"]["servers"].push_back({{"type", "hosts"}, {"tag", tag}, {"predefined", predefined}});
    const bool dnsEnabled = !dns || !dns.IsMap() ||
        (lowerCopy(ytext(dns, "enable")) != "false" && lowerCopy(ytext(dns, "enable")) != "no" &&
         lowerCopy(ytext(dns, "enable")) != "0");
    nlohmann::json domains = nlohmann::json::array();
    for (auto entry = predefined.begin(); entry != predefined.end(); ++entry) domains.push_back(entry.key());
    if (!exportOnly) {
        ctx.hostsDomains = domains;
        ctx.hostsDnsTag = tag;
        if (useHosts && dnsEnabled) {
            auto& rules = ctx.config["dns"]["rules"];
            if (!rules.is_array()) rules = nlohmann::json::array();
            rules.insert(rules.begin(), nlohmann::json{{"domain", domains}, {"query_type", {"A", "AAAA"}},
                         {"action", "route"}, {"server", tag}, {"rewrite_ttl", 10}, {"disable_cache", true}});
        }
    }
    // Global Clash hosts is also consulted before node and DNS endpoint
    // resolution, independently of dns.use-hosts (which controls DNS replies).
    for (auto& outbound : ctx.outbounds) {
        const auto host = dnsPolicyDomain(outbound.value("server", ""));
        if (host && predefined.contains(*host)) outbound["domain_resolver"] = {
            {"server", tag}, {"strategy", ctx.config["dns"].value("strategy", "prefer_ipv4")}, {"disable_cache", true}};
    }
    for (auto& server : ctx.config["dns"]["servers"]) {
        const auto host = dnsPolicyDomain(server.value("server", ""));
        if (host && predefined.contains(*host)) server["domain_resolver"] = {{"server", tag}, {"disable_cache", true}};
    }
    ctx.note(FidelityScope::Dns, Fidelity::Approx, "hosts",
             "精确 hosts IP 映射已接入原生 hosts DNS、节点与 DNS 端点解析；显式映射绕过 DNS 缓存，多 IP 拨号选择与 Clash 不同",
             "DNS 应答限 A/AAAA 并重写 TTL 为 10 秒；未映射条目单独记账，系统 hosts 开关仍未转换");
    if (exportOnly) ctx.note(FidelityScope::Dns, Fidelity::Approx, "source.hosts",
        "次来源 hosts 仅保留出站引用的解析依赖，不导入主来源 DNS 应答或连接解析规则");
}

// ---- TLS / 传输层（vmess/vless/trojan 共用）--------------------------------


bool validateDnsDependencies(Context& ctx) {
    std::map<std::string, std::vector<std::string>> graph;
    for (const auto& server : ctx.config["dns"]["servers"]) {
        auto& edges = graph["dns/" + server.value("tag", "")];
        if (server.contains("domain_resolver"))
            edges.push_back("dns/" + server["domain_resolver"].value("server", ""));
        if (server.contains("detour")) edges.push_back("out/" + server["detour"].get<std::string>());
    }
    for (const auto& outbound : ctx.outbounds) {
        auto& edges = graph["out/" + outbound.value("tag", "")];
        if (outbound.contains("detour")) edges.push_back("out/" + outbound["detour"].get<std::string>());
        if (outbound.contains("outbounds")) {
            for (const auto& member : outbound["outbounds"]) edges.push_back("out/" + member.get<std::string>());
        }
        // DNS endpoint addresses reach DIRECT as IPs; its resolver only acts
        // on user domains, so it cannot create a bootstrap edge here.
        if (outbound.value("type", "") != "direct" && outbound.contains("domain_resolver"))
            edges.push_back("dns/" + outbound["domain_resolver"].value("server", ""));
    }
    std::map<std::string, unsigned char> colors;
    struct Frame { std::string node; std::size_t next = 0; };
    std::vector<Frame> stack;
    for (const auto& [start, unused] : graph) {
        if (colors[start] != 0) continue;
        colors[start] = 1;
        stack.push_back({start});
        while (!stack.empty()) {
            auto& frame = stack.back();
            const auto& edges = graph.at(frame.node);
            if (frame.next == edges.size()) {
                colors[frame.node] = 2;
                stack.pop_back();
                continue;
            }
            const std::string target = edges[frame.next++];
            if (!graph.contains(target) || colors[target] == 1) {
                std::string path;
                for (const auto& entry : stack) {
                    if (!path.empty()) path += " → ";
                    path += entry.node;
                }
                path += " → " + target;
                ctx.result.error = "DNS/出站解析依赖无效或形成循环：" + path;
                ctx.note(FidelityScope::Dns, Fidelity::Unsupported, target, ctx.result.error,
                         "为代理节点配置不依赖该代理链的 proxy-server-nameserver；不会改直连绕过循环");
                return false;
            }
            if (colors[target] == 0) {
                colors[target] = 1;
                stack.push_back({target});
            }
        }
    }
    return true;
}
