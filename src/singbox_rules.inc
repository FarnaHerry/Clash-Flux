// singbox_rules.inc — 规则集、规则匹配与原生连接规则
// 由 src/singbox.cpp 在同一匿名命名空间中包含。

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
    // 命中前必须校验：0 字节/被写入错误内容的缓存会让内核启动期 FATAL
    // （parse rule-set: read rule: unexpected EOF）。
    if (!ctx.opt.ruleSetDir.empty()) {
        const std::filesystem::path local =
            std::filesystem::path(ctx.opt.ruleSetDir) / (tag + ".srs");
        std::error_code ec;
        if (std::filesystem::exists(local, ec) && !ec) {
            if (RuleSetCacheValid(local)) {
                ctx.ruleSets.push_back({
                    {"type", "local"},
                    {"tag", tag},
                    {"format", "binary"},
                    {"path", local.string()},
                });
                return true;
            }
            std::error_code removeError;
            std::filesystem::remove(local, removeError);
            ctx.warn(std::format("本地 GEOIP 规则集缓存「{}」不可用，已删除并改为在线拉取", tag));
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
bool ensureGeositeRuleSet(Context& ctx, std::string category) {
    category = lowerCopy(category);
    if (ctx.geositeTags.contains(category)) {
        const std::string tag = "geosite-" + category;
        return std::ranges::any_of(ctx.ruleSets, [&](const auto& ruleSet) {
            return ruleSet.is_object() && ruleSet.value("tag", "") == tag;
        });
    }
    const std::string tag = "geosite-" + category;
    ctx.geositeTags.emplace(category, tag);
    if (category.empty() ||
        !std::all_of(category.begin(), category.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                   c == '-' || c == '_';
        })) {
        ctx.warn(std::format("GEOSITE 类别「{}」无效，已跳过", category));
        return false;
    }
    if (!ctx.opt.ruleSetDir.empty()) {
        const std::filesystem::path local =
            std::filesystem::path(ctx.opt.ruleSetDir) / (tag + ".srs");
        std::error_code ec;
        if (std::filesystem::exists(local, ec) && !ec) {
            if (RuleSetCacheValid(local)) {
                ctx.ruleSets.push_back({
                    {"type", "local"},
                    {"tag", tag},
                    {"format", "binary"},
                    {"path", local.string()},
                });
                return true;
            }
            std::error_code removeError;
            std::filesystem::remove(local, removeError);
            ctx.warn(std::format("本地 GEOSITE 规则集缓存「{}」不可用，已删除并改为在线拉取", tag));
        }
    }
#if defined(__ANDROID__)
    ctx.warn(std::format("Android 暂无本地 GEOSITE 规则集「{}」，已跳过在线下载", category));
    return false;
#else
    ctx.ruleSets.push_back({
        {"type", "remote"},
        {"tag", tag},
        {"format", "binary"},
        {"url", std::format("https://raw.githubusercontent.com/MetaCubeX/meta-rules-dat/sing/geo/geosite/{}.srs", category)},
        {"update_interval", "24h"},
    });
    return true;
#endif
}

void ensureChinaDnsRule(Context& ctx, const std::string& geositeTag) {
    if (!ctx.config["dns"].contains("rules") ||
        !ctx.config["dns"]["rules"].is_array()) {
        ctx.config["dns"]["rules"] = nlohmann::json::array();
    }
    const bool exists = std::ranges::any_of(ctx.config["dns"]["rules"],
        [&](const auto& rule) {
            return rule.is_object() && rule.value("server", "") == "local" &&
                   rule.value("rule_set", nlohmann::json::array()) ==
                       nlohmann::json::array({geositeTag});
        });
    if (!exists) {
        ctx.config["dns"]["rules"].push_back({
            {"rule_set", {geositeTag}},
            {"action", "route"},
            {"server", "local"},
        });
    }
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
    } else if (kind == "geosite") {
        const std::string category = lowerCopy(value);
        if (!ensureGeositeRuleSet(ctx, category)) return true;
        const std::string& tag = ctx.geositeTags.at(category);
        rule["rule_set"] = {tag};
        if (category == "cn" && target == "DIRECT") {
            ensureChinaDnsRule(ctx, tag);
        }
    } else if (kind == "rule-set") {
        // Mihomo 订阅普遍用 MRS provider 表达国内分流。sing-box
        // 不能直读 MRS，但 cn / cn-ip 的语义与我们内置的
        // geosite-cn / geoip-cn 一致；显式映射这两个高频别名，
        // 避免国内直连规则被丢弃后全部落入 MATCH,PROXY。
        const std::string provider = lowerCopy(value);
        if (provider == "cn" || provider == "china" ||
            provider == "geosite-cn") {
            if (!ensureGeositeRuleSet(ctx, "cn")) return true;
            const std::string& tag = ctx.geositeTags.at("cn");
            rule["rule_set"] = {tag};
            if (target == "DIRECT") ensureChinaDnsRule(ctx, tag);
        } else if (provider == "cn-ip" || provider == "china-ip" ||
                   provider == "geoip-cn") {
            if (!ensureGeoipRuleSet(ctx, "cn")) return true;
            rule["rule_set"] = {ctx.geoipTags.at("cn")};
        } else {
            ctx.warn(std::format(
                "规则集 RULE-SET {} 的格式暂不支持，已跳过", value));
            return true;
        }
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
            [](const auto& native) {
                return native.connected && native.kind != vpn::ConnectionKind::OpenVpn;
            })) {
        ctx.result.error = "当前平台尚未实现原生 VPN 与主 TUN 的补偿路由；请先关闭 TUN";
        return false;
    }
#endif
#ifdef _WIN32
    if (ctx.opt.tunInbound && std::ranges::any_of(ctx.opt.nativeConnections,
            [](const auto& native) {
                return native.connected && native.kind != vpn::ConnectionKind::Pptp &&
                    native.kind != vpn::ConnectionKind::OpenVpn;
            })) {
        ctx.result.error = "Windows 主 TUN 共存目前支持 PPTP 和 sing-box OpenVPN；其他原生 VPN 请先关闭 TUN";
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
    if (!config.contains("endpoints")) config["endpoints"] = nlohmann::json::array();
    if (!config["endpoints"].is_array()) {
        ctx.result.error = "sing-box endpoints 必须为数组";
        return false;
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
        if (native.connected && native.kind == vpn::ConnectionKind::OpenVpn) {
            tag = unusedOutboundTag(config, "clash-flux-openvpn-" +
                                             std::to_string(nativeTags.size()));
            std::string endpointError;
            auto endpoint = compileOpenVpnEndpoint(native, tag, endpointError);
            if (!endpoint) {
                ctx.result.error = std::format("OpenVPN「{}」配置转换失败：{}",
                                               native.id, endpointError);
                return false;
            }
            config["endpoints"].push_back(std::move(*endpoint));
        } else if (native.connected && !native.interfaceName.empty()) {
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
        {{"action", "sniff"}},
        {{"protocol", "dns"}, {"action", "hijack-dns"}},
        {{"action", "resolve"}},
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
