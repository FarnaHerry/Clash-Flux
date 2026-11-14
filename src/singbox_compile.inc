// singbox_compile.inc — 托管骨架与 Clash 文档编译
// 由 src/singbox.cpp 在同一匿名命名空间中包含。

std::string mappedLogLevel(const std::string& level) {
    const std::string lowered = lowerCopy(trimCopy(level));
    if (lowered == "warning") return "warn";
    if (lowered.empty()) return "info";
    return lowered;  // silent/error/info/debug 原样（silent 由调用方转 disabled）
}
void applyManagedSkeleton(Context& ctx, const CompileOptions& opt) {
    nlohmann::json& config = ctx.config;
    if (!clashflux::tun_settings::ValidMtu(opt.tunMtu)) {
        ctx.result.error = "虚拟网卡 MTU 需要填写 1280–9000 的整数";
        return;
    }

#if defined(__ANDROID__)
    constexpr bool androidNetwork = true;
#else
    constexpr bool androidNetwork = false;
#endif
    if (const auto failure = clashflux::network::Validate(opt.network, androidNetwork); !failure.empty()) {
        ctx.result.error = failure;
        return;
    }
    if (!opt.proxyListening && opt.tunInbound) {
        ctx.result.error = "待命内核不能启用 TUN；请先连接代理";
        return;
    }

    const auto validPort = [](int port) { return port >= 1 && port <= 65535; };
    if (!validPort(opt.mixedPort) || opt.httpPort < 0 || opt.httpPort > 65535 ||
        opt.socksPort < 0 || opt.socksPort > 65535) {
        ctx.result.error = "托管代理入站端口无效，端口范围必须为 1–65535";
        return;
    }
    if ((opt.httpPort > 0 &&
         (opt.httpPort == opt.mixedPort || opt.httpPort == opt.socksPort)) ||
        (opt.socksPort > 0 && opt.socksPort == opt.mixedPort)) {
        ctx.result.error = "混合、HTTP 与 SOCKS 入站端口不能重复";
        return;
    }

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
            // Managed real-IP DNS must retain names for IP-only TUN traffic.
            // Native JSON with its own DNS remains authoritative.
            {"reverse_mapping", true},
            {"strategy", opt.ipv6 ? "prefer_ipv4" : "ipv4_only"},
        };
        if (!config.contains("route")) config["route"] = nlohmann::json::object();
        config["route"]["default_domain_resolver"] = {{"server", "local"}};
    }

    // Standby is an explicit runtime override, never a change to the source.
    // Keep outbounds/controllers for URL tests, but accept no application traffic.
    if (!config.contains("inbounds") || !config["inbounds"].is_array()) {
        config["inbounds"] = nlohmann::json::array();
    }
    if (!opt.tunInbound) {
        auto& inbounds = config["inbounds"];
        inbounds.erase(std::remove_if(inbounds.begin(), inbounds.end(), [](const auto& inbound) {
            return inbound.is_object() && inbound.value("type", "") == "tun";
        }), inbounds.end());
    }
    if (opt.network.override_ports) {
        auto& inbounds = config["inbounds"];
        inbounds.erase(std::remove_if(inbounds.begin(), inbounds.end(), [&](const auto& inbound) {
            return (inbound.value("type", "") == "http" && !opt.httpPort) ||
                   (inbound.value("type", "") == "socks" && !opt.socksPort);
        }), inbounds.end());
        std::set<std::string> rewritten;
        for (auto& inbound : inbounds) {
            const auto type = inbound.value("type", "");
            if (type != "mixed" && type != "http" && type != "socks") continue;
            if (!rewritten.insert(type).second) {
                ctx.result.error = "覆盖端口需要每种类型仅有一个监听"; return;
            }
            inbound["listen_port"] = type == "mixed" ? opt.mixedPort : type == "http" ? opt.httpPort : opt.socksPort;
        }
    }
    bool hasMixed = false;
    bool hasHttp = false;
    bool hasSocks = false;
    bool hasTun = false;
    for (const auto& inbound : config["inbounds"]) {
        const std::string type = inbound.value("type", "");
        if (type == "mixed") hasMixed = true;
        if (type == "http") hasHttp = true;
        if (type == "socks") hasSocks = true;
        if (type == "tun") {
            if (hasTun) {
                ctx.result.error = "主 VPN 仅允许一个托管 TUN；请移除配置中的额外 TUN 入站";
                return;
            }
            hasTun = true;
        }
    }
    if (opt.proxyListening && !hasMixed) {
        config["inbounds"].push_back({
            {"type", "mixed"},
            {"tag", "mixed-in"},
            {"listen", opt.allowLan ? "0.0.0.0" : "127.0.0.1"},
            {"listen_port", opt.mixedPort},
        });
    }
    if (opt.proxyListening && opt.httpPort > 0 && !hasHttp) {
        config["inbounds"].push_back({
            {"type", "http"},
            {"tag", "http-in"},
            {"listen", opt.allowLan ? "0.0.0.0" : "127.0.0.1"},
            {"listen_port", opt.httpPort},
        });
    }
    if (opt.proxyListening && opt.socksPort > 0 && !hasSocks) {
        config["inbounds"].push_back({
            {"type", "socks"},
            {"tag", "socks-in"},
            {"listen", opt.allowLan ? "0.0.0.0" : "127.0.0.1"},
            {"listen_port", opt.socksPort},
        });
    }
    if (!opt.proxyListening) config["inbounds"] = nlohmann::json::array();
    if (opt.proxyListening && opt.tunInbound && !hasTun) {
        nlohmann::json tun = {
            {"type", "tun"},
            {"tag", "tun-in"},
            {"address", opt.ipv6 ? nlohmann::json::array({"172.19.0.1/30", "fdfe:dcba:9876::1/126"})
                                 : nlohmann::json::array({"172.19.0.1/30"})},
            {"mtu", opt.tunMtu},
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
    // Apply explicit global overrides only. Source credentials, DNS addresses
    // and custom inbounds survive when no corresponding override was selected.
    const auto& network = opt.network;
#if !defined(__linux__) || defined(__ANDROID__)
    if (network.advanced && (!network.advanced->auto_redirect.empty() || network.advanced->override_filters)) {
        ctx.result.error = "自动重定向及接口、UID、MAC 筛选仅支持 Linux 桌面";
        return;
    }
#endif

    for (auto& inbound : config["inbounds"]) {
        const auto type = inbound.value("type", "");
        if (type == "mixed" || type == "http" || type == "socks") {
            if (network.override_listen) inbound["listen"] = network.listen_address.empty()
                ? (opt.allowLan ? "0.0.0.0" : "127.0.0.1") : network.listen_address;
            if (!network.bind_interface.empty()) inbound["bind_interface"] = network.bind_interface;
            if (network.override_auth) {
                inbound["users"] = nlohmann::json::array();
                if (!network.username.empty()) inbound["users"].push_back({
                    {"username", network.username}, {"password", network.password}});
            }
            // The user session owns OS proxy settings, including imported configs.
            if (inbound.value("set_system_proxy", false)) {
                inbound["set_system_proxy"] = false;
                ctx.note(FidelityScope::Field, Fidelity::Approx, "set_system_proxy",
                    "系统代理由应用用户会话统一管理", "使用首页系统代理开关");
            }
        }
        if (type != "tun") continue;
        if (!network.stack.empty()) inbound["stack"] = network.stack;
        if (!network.dns_mode.empty()) {
            inbound["dns_mode"] = network.dns_mode;
            // Imported dns_address stays authoritative; a new mode never
            // invents a DNS server or rewrites the source's DNS rules.
        }
        if (!network.interface_name.empty()) inbound["interface_name"] = network.interface_name;
        if (!network.addresses.empty()) inbound["address"] = clashflux::network::Lines(network.addresses);
        if (network.override_routes) {
            inbound.erase("route_address");
            inbound.erase("route_address_set");
            inbound.erase("inet4_route_address");
            inbound.erase("inet6_route_address");
            if (!clashflux::network::Lines(network.routes).empty())
                inbound["route_address"] = clashflux::network::Lines(network.routes);
        }
        if (network.override_exclusions) {
            inbound.erase("route_exclude_address_set");
            inbound.erase("inet4_route_exclude_address");
            inbound.erase("inet6_route_exclude_address");
            inbound["route_exclude_address"] = clashflux::network::Lines(network.exclusions);
        }
        if (!network.udp_timeout.empty()) inbound["udp_timeout"] = network.udp_timeout;
        if (!network.udp_mapping.empty()) inbound["udp_mapping"] = network.udp_mapping;
        if (!network.udp_filtering.empty()) inbound["udp_filtering"] = network.udp_filtering;
        if (network.override_udp_nat_max) inbound["udp_nat_max"] = network.udp_nat_max;
#if defined(__ANDROID__)
        if (!network.package_mode.empty()) {
            inbound.erase("include_package");
            inbound.erase("exclude_package");
            if (network.package_mode == "include" || network.package_mode == "exclude") {
                const auto key = network.package_mode == "include" ? "include_package" : "exclude_package";
                inbound[key] = clashflux::network::Lines(network.packages);
            }
        }
#else
        if (!network.package_mode.empty()) {
            ctx.result.error = "应用接管列表仅支持 Android";
            return;
        }
#endif
    }

    // Apply ownership and transport compensation to imported TUNs as well.
    // Data destinations stay inside TUN so explicit global rules win over a
    // native connection's implicit CIDRs. Only transport addresses bypass it.
    for (auto& inbound : config["inbounds"]) {
        if (!inbound.is_object() || inbound.value("type", "") != "tun") continue;
        inbound["auto_route"] = true;
        inbound["strict_route"] = opt.tunStrictRoute;
        if (opt.overrideTunMtu) inbound["mtu"] = opt.tunMtu;
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
        // Native VPN compensation has rules on both sides of this boundary.
        // nftables interception is opt-in and must never bypass those rules.
        inbound["iproute2_rule_index"] = 9000;
        const auto advanced = opt.network.advanced.value_or(clashflux::network::Advanced{});
        const bool redirect = advanced.auto_redirect == "enabled";
        if (redirect && std::ranges::any_of(opt.nativeConnections, [](const auto& native) { return native.connected; })) {
            ctx.result.error = "Linux 自动重定向不能与原生 VPN 补偿路由同时启用";
            ctx.note(FidelityScope::Field, Fidelity::Unsupported, "auto_redirect", ctx.result.error, "关闭自动重定向后重试");
            return;
        }
        if (inbound.value("auto_redirect", false) && !redirect)
            ctx.note(FidelityScope::Field, Fidelity::Approx, "auto_redirect",
                "托管 TUN 使用策略路由，来源的自动重定向未启用", "可在虚拟网卡高级设置显式启用");
        inbound["auto_redirect"] = redirect;
        if (advanced.override_filters) {
            for (const auto key : {"include_interface", "exclude_interface", "include_uid", "exclude_uid",
                "include_uid_range", "exclude_uid_range", "include_mac_address", "exclude_mac_address"}) inbound.erase(key);
            if (!advanced.interface_mode.empty()) inbound[advanced.interface_mode + "_interface"] = clashflux::network::Lines(advanced.interfaces);
            if (!advanced.uid_mode.empty()) {
                nlohmann::json ids = nlohmann::json::array(), ranges = nlohmann::json::array();
                for (const auto& uid : clashflux::network::Lines(advanced.uids)) {
                    if (uid.find(':') != std::string::npos) ranges.push_back(uid);
                    else ids.push_back(std::stoi(uid));
                }
                if (!ids.empty()) inbound[advanced.uid_mode + "_uid"] = std::move(ids);
                if (!ranges.empty()) inbound[advanced.uid_mode + "_uid_range"] = std::move(ranges);
            }
            if (!advanced.mac_mode.empty()) inbound[advanced.mac_mode + "_mac_address"] = clashflux::network::Lines(advanced.macs);
        }
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
    if (!config["route"].contains("final") || config["route"].value("final", "").empty()) {
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
    // Android libbox is controlled through CommandServer/CommandClient, so it
    // must not listen on the desktop REST port.  A clash_api object is still
    // required: libbox's internal Clash server reads default_mode from it and
    // exposes that mode through CommandClient without opening an HTTP socket.
    config["experimental"]["clash_api"] = {{"default_mode", opt.mode}};
#else
    nlohmann::json clashApi = config["experimental"].contains("clash_api") &&
                                      config["experimental"]["clash_api"].is_object()
                                  ? config["experimental"]["clash_api"]
                                  : nlohmann::json::object();
    clashApi["external_controller"] = opt.controller;
    if (!opt.secret.empty()) clashApi["secret"] = opt.secret;
    // mode_list 不是可写配置字段（1.14.2 已移除旧内部 ModeList 成员）：
    // 内核从 route rules 的 clash_mode 值自动推导模式列表；规则编译末尾另有
    // 与 route.final 等价的 rule 兜底，保证 default/cache 任意组合下三模式均可选。
    clashApi["default_mode"] = opt.mode;
    config["experimental"]["clash_api"] = std::move(clashApi);
    if (opt.nativeControllerPort != 0) {
        if (opt.secret.empty() || opt.nativeControllerPort < 1 || opt.nativeControllerPort > 65535) {
            ctx.result.error = "原生控制接口必须使用有效端口和非空密钥"; return;
        }
        if (!config.contains("services")) config["services"] = nlohmann::json::array();
        if (!config["services"].is_array() || std::ranges::any_of(config["services"], [](const auto& service) {
            return service.is_object() && service.value("tag", "") == "clash-flux-control";
        })) { ctx.result.error = "原生控制服务标签 clash-flux-control 冲突"; return; }
        config["services"].push_back({{"type", "api"}, {"tag", "clash-flux-control"},
            {"listen", "127.0.0.1"}, {"listen_port", opt.nativeControllerPort},
            {"secret", opt.secret}, {"dashboard", false}});
    }
#endif
    // 原生配置的 FakeIP/RDRC 持久化、路径和 cache_id 属于用户策略，不能
    // 为启用应用缓存而把整个对象覆盖掉。应用只管理 enabled。
    nlohmann::json cacheFile = config["experimental"].contains("cache_file") &&
                                      config["experimental"]["cache_file"].is_object()
                                  ? config["experimental"]["cache_file"]
                                  : nlohmann::json::object();
    cacheFile["enabled"] = true;
    config["experimental"]["cache_file"] = std::move(cacheFile);
}

// Expand only this source's successfully compiled top-level proxies. Forward
// group declarations, built-ins and objects from other sources are excluded.
bool expandGroupProxies(Context& ctx, const YAML::Node& group, const std::string& name,
                        const std::vector<std::string>& proxies, bool rejectedProxy,
                        std::vector<std::string>& members) {
    const auto reject = [&](const std::string& field, const std::string& detail) {
        ctx.note(FidelityScope::Group, Fidelity::Unsupported, name,
                 std::format("代理组「{}」的 {} 无法安全展开：{}", name, field, detail),
                 "保留原订阅和运行配置；修正声明或改为显式 proxies，不能忽略筛选后扩大成员范围");
        ctx.result.error = std::format("代理组「{}」成员展开失败：{}", name, field);
        return false;
    };
    std::set<std::string> declarations;
    for (const auto& item : group) {
        if (!item.first.IsScalar()) continue;
        const auto field = item.first.Scalar();
        if (field != "include-all" && field != "include-all-proxies" && field != "include-all-providers" &&
            field != "proxies" && field != "use" && field != "filter" && field != "exclude-filter" &&
            field != "exclude-type") continue;
        if (!declarations.insert(field).second) return reject(field, "重复声明，不能猜测生效值");
    }
    for (const char* field : {"include-all", "include-all-proxies", "include-all-providers"}) {
        const auto declared = group[field];
        if (!declared.IsDefined()) continue;
        const auto value = lowerCopy(ytext(group, field));
        if (!declared.IsScalar() || (value != "true" && value != "false" &&
            value != "yes" && value != "no" && value != "1" && value != "0"))
            return reject(field, "必须是有效布尔值");
    }
    if (!ybool(group, "include-all-proxies")) return true;
    const auto use = group["use"];
    if (ybool(group, "include-all") || ybool(group, "include-all-providers") ||
        (use.IsDefined() && (!use.IsSequence() || use.size() != 0)))
        return reject("use/include-all", "include-all-proxies 与代理集合混用尚未支持，不提交部分展开");
    for (const char* field : {"filter", "exclude-filter", "exclude-type"}) {
        const auto declared = group[field];
        if (declared.IsDefined() && (!declared.IsScalar() || !declared.Scalar().empty()))
            return reject(field, "筛选转换尚未接入，不改成无筛选的全部节点");
    }
    if (const auto explicitMembers = group["proxies"]; explicitMembers.IsDefined()) {
        if (!explicitMembers.IsSequence()) return reject("proxies", "必须为完整成员数组");
        for (const auto& member : explicitMembers)
            if (!member.IsScalar() || trimCopy(member.Scalar()).empty())
                return reject("proxies", "数组包含无效成员，不保留合法前缀");
    }
    members.insert(members.end(), proxies.begin(), proxies.end());
    if (members.empty())
        return reject("include-all-proxies", "没有可用成员；空组回退尚未转换，不隐式改为 DIRECT");
    ctx.note(FidelityScope::Group, Fidelity::Exact, name,
             "include-all-proxies 按名称排序追加当前来源节点，显式成员在前，不包含内置出站或其它组");
    if (rejectedProxy)
        ctx.note(FidelityScope::Group, Fidelity::Approx, name,
                 "自动展开成员中有无法转换的节点，已随节点拒绝明细从该组移除",
                 "检查 Node/Unsupported 明细；该组只包含实际编译成功的本来源节点");
    return true;
}

// 解析 Clash YAML 文档并转换节点/组/规则（调用方负责异常捕获）。
void compileClashDocument(Context& ctx, const std::string& trimmed, bool exportOnly = false) {
    YAML::Node root;
    const auto nodeText = nodeSubscriptionText(trimmed);
    if (nodeText) {
        try {
            auto document = parseNodeSubscription(*nodeText);
            root = document.root;
            ctx.note(FidelityScope::Field, Fidelity::Approx, "node-subscription.policy",
                "节点订阅不含路由规则；自动生成包含全部节点的手动选择组和 MATCH 默认出口",
                "可在个人规则或全局规则中配置分流");
            for (const auto& name : document.tlsTransportAliases)
                ctx.note(FidelityScope::Node, Fidelity::Approx, name,
                    "非标准 net=tls 兼容映射为 TCP，并保留显式 TLS 设置",
                    "建议服务商使用标准 net=tcp、tls=tls 字段");
        } catch (const std::exception& error) {
            ctx.result.error = error.what();
            ctx.note(FidelityScope::Field, Fidelity::Unsupported, "node-subscription", ctx.result.error);
            return;
        }
    } else {
        bool reusedParse = false;
        root = loadCachedClashYaml(trimmed, &reusedParse);
        ctx.result.sourceParseCacheHits += reusedParse ? 1 : 0;
    }
    if (ctx.cancelled()) return;
    if (!root.IsMap()) {
        ctx.result.error = "订阅不是有效的 Clash YAML 配置";
        return;
    }
    if (!applyClashFingerprint(ctx, root)) return;
    if (!configureGeoData(ctx, root)) return;

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

    std::vector<std::string> localProxies;
    bool rejectedProxy = false;
    if (const YAML::Node proxies = root["proxies"]; proxies && proxies.IsSequence()) {
        for (const YAML::Node& proxy : proxies) {
            if (ctx.cancelled()) return;
            if (!proxy.IsMap()) { rejectedProxy = true; continue; }
            if (auto converted = convertProxy(ctx, proxy)) {
                localProxies.push_back(converted->at("tag").get<std::string>());
                ctx.knownTags.push_back(converted->at("tag").get<std::string>());
                ctx.outbounds.push_back(std::move(*converted));
            } else {
                rejectedProxy = true;
                if (nodeText) {
                    ctx.result.error = "节点订阅包含无法完整转换的节点；保留原订阅，不提交部分结果";
                    return;
                }
            }
        }
    }
    std::ranges::sort(localProxies); // Mihomo AllProxies uses bytewise name order.
    auto proxyProviders = compileProxyProviders(ctx, root["proxy-providers"]);
    if (!proxyProviders) return;

    // Discover all referenced HTTP inputs before the first incomplete group
    // stops compilation, so independent provider downloads can share a batch.
    for (const YAML::Node& group : groups) {
        const auto type = lowerCopy(ytext(group, "type"));
        if (type != "select" && type != "url-test") continue;
        const auto use = group["use"];
        for (const auto& [provider, index] : proxyProviders->resources) {
            const bool explicitUse = use && use.IsSequence() && std::ranges::any_of(use, [&](const auto& item) {
                return item.IsScalar() && item.Scalar() == provider;
            });
            if (ybool(group, "include-all-providers") || explicitUse)
                ctx.result.httpProxyProviders[index].required = true;
        }
    }

    for (const YAML::Node& group : groups) {
        if (ctx.cancelled()) return;
        const std::string name = ytext(group, "name");
        if (name.empty()) continue;
        std::vector<std::string> members = ylist(group, "proxies");
        std::string providerTestUrl;
        if (!expandGroupProxies(ctx, group, name, localProxies, rejectedProxy, members) ||
            !expandProxyProviderGroup(ctx, group, name, *proxyProviders, members, providerTestUrl)) return;
        if (auto converted = convertGroup(ctx, group, name, members, providerTestUrl)) {
            ctx.outbounds.push_back(std::move(*converted));
        } else {
            ctx.note(FidelityScope::Group, Fidelity::Unsupported, name,
                     std::format("代理组「{}」编译失败，已跳过", name),
                     "该组已从订阅里移除；到日志页看具体编译错误");
        }
    }

    if (!validateOutboundDependencies(ctx)) return;

    if (!exportOnly) compileRuleProviders(ctx, root["rule-providers"]);
    if (!ctx.result.error.empty()) return;

    // DNS detours must resolve against actual converted nodes/groups, including
    // nodes declared after dns and excluding rejected group declarations.
    applyClashDns(ctx, root["dns"]);
    applyClashHosts(ctx, root, exportOnly);
    if (!validateDnsDependencies(ctx)) return;

    if (exportOnly) {
        // MATCH 只声明来源的默认导出对象，不进入全局规则。
        if (root["rules"] || root["rule-providers"])
            ctx.note(FidelityScope::Field, Fidelity::Approx, "source.rules",
                     "次来源不导入全局订阅规则或 rule-providers；MATCH 仅定义默认导出目标",
                     "请在编排规则中显式定义此来源的匹配范围");
        if (root["dns"])
            ctx.note(FidelityScope::Dns, Fidelity::Approx, "source.dns",
                     "次来源仅保留被出站引用的 DNS 依赖，不覆盖主来源的全局 DNS 策略");
        if (const auto rules = root["rules"]; rules && rules.IsSequence()) {
            for (const auto& item : rules) {
                if (!item.IsScalar()) continue;
                const auto text = trimCopy(item.Scalar());
                if (lowerCopy(text.substr(0, text.find(','))) == "match" && !convertRule(ctx, text)) break;
            }
        }
        return;
    }
    if (!ctx.opt.profileRuleEdits.rules.empty()) {
        compilePersonalClashRules(ctx, root);
        return;
    }
    if (const YAML::Node rules = root["rules"]; rules) {
        if (!rules.IsSequence()) {
            ctx.note(FidelityScope::Rule, Fidelity::Unsupported, "rules",
                     "Clash rules 必须为字符串数组，已忽略");
            return;
        }
        std::size_t index = 0;
        for (const YAML::Node& rule : rules) {
            if (ctx.cancelled()) return;
            if (!rule.IsScalar()) {
                ctx.note(FidelityScope::Rule, Fidelity::Unsupported,
                         "rules[" + std::to_string(index) + "]", "规则不是字符串，已跳过");
            } else if (!convertRule(ctx, rule.Scalar())) {
                break; // First valid MATCH terminates the source rule list.
            }
            ++index;
        }
    }
}

// Resource IDs are resolved after source namespacing and personal-set injection.
// This is capture routing: the kernel extracts destination IP prefixes only.
bool applyNetworkRouteSets(Context& ctx) {
    const auto advanced = ctx.opt.network.advanced.value_or(clashflux::network::Advanced{});
    if (advanced.auto_redirect == "enabled") {
        const auto hasMark = [&](const auto& self, const nlohmann::json& value) -> bool {
            if (value.is_object()) for (auto it = value.begin(); it != value.end(); ++it) {
                if ((it.key() == "routing_mark" || it.key() == "default_mark") && it.value() != 0) return true;
                if (self(self, it.value())) return true;
            }
            if (value.is_array()) for (const auto& child : value) if (self(self, child)) return true;
            return false;
        };
        if (hasMark(hasMark, ctx.config)) {
            ctx.result.error = "Linux 自动重定向与 routing_mark/default_mark 冲突";
            return false;
        }
    }
    if (!advanced.override_route_sets) return true;
#ifdef _WIN32
    if ((!clashflux::network::Lines(advanced.route_sets).empty() || !clashflux::network::Lines(advanced.exclude_sets).empty()) &&
        std::ranges::any_of(ctx.opt.nativeConnections, [](const auto& native) { return native.connected; })) {
        ctx.result.error = "Windows PPTP 共存不支持规则集路由接管";
        return false;
    }
#endif

#if (!defined(__linux__) || defined(__ANDROID__)) && !defined(_WIN32)
    ctx.result.error = "IP 规则集路由接管仅支持 Linux 和 Windows 桌面";
    return false;
#else
    for (auto& inbound : ctx.config["inbounds"]) {
        if (inbound.value("type", "") != "tun") continue;
        inbound.erase("route_address_set"); inbound.erase("route_exclude_address_set");
        if (!clashflux::network::Lines(advanced.route_sets).empty()) {
            // Empty IP extraction must not trigger sing-tun's default-all
            // fallback for either address family (e.g. a domain-only set).
            auto& addresses = inbound["route_address"];
            if (addresses.is_string()) addresses = nlohmann::json::array({addresses});
            if (addresses.is_null()) addresses = nlohmann::json::array();
            for (const auto prefix : {"0.0.0.0/32", "::/128"})
                if (std::ranges::find(addresses, nlohmann::json(prefix)) == addresses.end()) addresses.push_back(prefix);
        }

        for (const auto& [key, ids] : std::vector<std::pair<std::string, std::string>>{
            {"route_address_set", advanced.route_sets}, {"route_exclude_address_set", advanced.exclude_sets}}) {
            const auto list = clashflux::network::Lines(ids);
            if (!list.empty()) {
                inbound[key] = nlohmann::json::array();
                for (const auto& id : list) inbound[key].push_back(personalRuleSetTag(ctx.opt.ruleSetLibrary, id));
            }
        }
    }
    return true;
#endif
}
