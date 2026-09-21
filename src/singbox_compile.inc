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
