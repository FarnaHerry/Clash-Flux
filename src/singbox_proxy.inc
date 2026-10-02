// singbox_proxy.inc — 代理节点、传输层与代理组转换
// 由 src/singbox.cpp 在同一匿名命名空间中包含。

bool proxyValueInvalid(Context& ctx, const std::string& name,
                       std::string_view field, std::string_view reason) {
    // Do not include input values: these fields may carry authentication data.
    ctx.note(FidelityScope::Node, Fidelity::Unsupported, name,
             std::format("节点「{}」的 {} {}，已跳过", name, field, reason),
             "修正该字段后重新导入；不会删掉异常成员后保留部分线路配置");
    return false;
}

bool proxyString(Context& ctx, const YAML::Node& value, const std::string& name,
                 std::string_view field, std::string& result, bool nonempty = false) {
    if (!value.IsScalar()) return proxyValueInvalid(ctx, name, field, "必须为字符串");
    result = value.Scalar();
    if (nonempty && result.empty()) return proxyValueInvalid(ctx, name, field, "不能为空");
    return true;
}

bool proxyStringList(Context& ctx, const YAML::Node& map, const char* field,
                     const std::string& name, std::vector<std::string>& result,
                     bool nonempty = false) {
    const YAML::Node value = map[field];
    if (!value) return true;
    if (value.IsScalar()) {
        std::string text;
        if (!proxyString(ctx, value, name, field, text, nonempty)) return false;
        result.push_back(std::move(text));
        return true;
    }
    if (!value.IsSequence()) return proxyValueInvalid(ctx, name, field, "必须为字符串或字符串数组");
    for (std::size_t i = 0; i < value.size(); ++i) {
        std::string text;
        if (!proxyString(ctx, value[i], name, std::format("{}[{}]", field, i), text, nonempty)) return false;
        result.push_back(std::move(text));
    }
    return true;
}

bool proxyFingerprintSupported(std::string_view value) {
    // Fixed 1.14.2 common/tls/utls_client.go, not guessed browser names.
    static constexpr std::string_view fingerprints[]{
        "chrome", "firefox", "edge", "safari", "360", "qq", "ios", "android",
        "random", "randomized", "chrome_psk", "chrome_psk_shuffle",
        "chrome_padding_psk_shuffle", "chrome_pq", "chrome_pq_psk"};
    return std::ranges::find(fingerprints, value) != std::end(fingerprints);
}

bool applyClashFingerprint(Context& ctx, const YAML::Node& root) {
    const YAML::Node value = root["global-client-fingerprint"];
    if (!value) return true;
    if (!value.IsScalar() || !proxyFingerprintSupported(value.Scalar())) {
        ctx.result.error = "global-client-fingerprint 必须为固定内核支持的非空指纹字符串";
        ctx.note(FidelityScope::Field, Fidelity::Unsupported, "global-client-fingerprint",
                 ctx.result.error, "在节点中指定支持的 client-fingerprint；不会自动换成 chrome");
        return false;
    }
    ctx.defaultFingerprint = value.Scalar();
    return true;
}

bool appendTls(Context& ctx, const YAML::Node& item, nlohmann::json& out,
               bool required = false, bool allowUtls = true) {
    const std::string name = ytext(item, "name");
    std::string sni, servername;
    if (item["sni"] && !proxyString(ctx, item["sni"], name, "sni", sni)) return false;
    if (item["servername"] && !proxyString(ctx, item["servername"], name, "servername", servername)) return false;
    if (!servername.empty()) {
        if (!sni.empty() && sni != servername)
            ctx.note(FidelityScope::Node, Fidelity::Approx, name,
                     std::format("节点「{}」同时指定不同的 sni/servername，使用 servername", name));
        sni = std::move(servername);
    }
    const bool skipVerify = ybool(item, "skip-cert-verify");
    if (required && item["tls"] && !ybool(item, "tls"))
        return proxyValueInvalid(ctx, name, "tls", "不能关闭该协议必需的 TLS");
    // Preserve the existing implicit SNI/Reality activation only when tls was
    // omitted; an explicit tls:false must never turn into an encrypted node.
    const bool enabled = required || (item["tls"] ? ybool(item, "tls") :
        !sni.empty() || skipVerify || bool(item["reality-opts"]));
    if (!enabled) {
        bool valid = true;
        for (const char* field : {"sni", "servername", "skip-cert-verify", "alpn",
                                  "client-fingerprint", "reality-opts"})
            if (item[field]) valid = proxyValueInvalid(ctx, name, field, "不能在未启用 TLS 时应用") && valid;
        return valid;
    }
    nlohmann::json tls = {{"enabled", true}};
    if (!sni.empty()) tls["server_name"] = sni;
    if (skipVerify) tls["insecure"] = true;
    std::vector<std::string> alpn;
    if (!proxyStringList(ctx, item, "alpn", name, alpn, true)) return false;
    if (std::ranges::any_of(alpn, [](const auto& protocol) { return protocol.size() > 255; }))
        return proxyValueInvalid(ctx, name, "alpn", "协议名长度必须为 1–255 字节");
    if (item["alpn"]) tls["alpn"] = std::move(alpn);
    if (item["client-fingerprint"] && !allowUtls)
        return proxyValueInvalid(ctx, name, "client-fingerprint", "不适用于该协议的 TLS");
    std::string fingerprint = ctx.defaultFingerprint;
    if (item["client-fingerprint"] &&
        !proxyString(ctx, item["client-fingerprint"], name, "client-fingerprint", fingerprint, true)) return false;
    if (allowUtls && !proxyFingerprintSupported(fingerprint))
        return proxyValueInvalid(ctx, name, "client-fingerprint", "不是固定内核支持的指纹");
    if (allowUtls) {
        tls["utls"] = {{"enabled", true}, {"fingerprint", fingerprint}};
        if (fingerprint.starts_with("chrome_"))
            ctx.note(FidelityScope::Node, Fidelity::Approx, name,
                     std::format("节点「{}」的指纹别名在固定内核中按普通 chrome 处理", name),
                     "固定 1.14.2 不保留这些 PSK/PQ 别名差异；需要区别时请更换配置");
        else if (fingerprint == "random")
            ctx.note(FidelityScope::Node, Fidelity::Approx, name,
                     std::format("节点「{}」使用内核 random 指纹，其抽样分布与 Clash 不保证一致", name));
    }
    if (ybool(item, "disable-sni")) tls["disable_sni"] = true;
    if (const YAML::Node reality = item["reality-opts"]; reality) {
        if (!reality.IsMap()) return proxyValueInvalid(ctx, name, "reality-opts", "必须为对象");
        std::string publicKey, shortId;
        if (!proxyString(ctx, reality["public-key"], name, "reality-opts.public-key", publicKey, true)) return false;
        if (reality["short-id"] && !proxyString(ctx, reality["short-id"], name, "reality-opts.short-id", shortId)) return false;
        nlohmann::json realityOut = {{"enabled", true}};
        realityOut["public_key"] = std::move(publicKey);
        if (reality["short-id"]) realityOut["short_id"] = std::move(shortId);
        tls["reality"] = realityOut;
    }
    out["tls"] = std::move(tls);
    return true;
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
            if (opts["path"]) {
                std::string path;
                if (!proxyString(ctx, opts["path"], name, "ws-opts.path", path)) return false;
                transport["path"] = std::move(path);
            }
            if (const YAML::Node headers = opts["headers"]; headers) {
                if (!headers.IsMap()) return proxyValueInvalid(ctx, name, "ws-opts.headers", "必须为对象");
                nlohmann::json mapped = nlohmann::json::object();
                for (auto it = headers.begin(); it != headers.end(); ++it) {
                    std::string key, value;
                    if (!proxyString(ctx, it->first, name, "ws-opts.headers 的键", key, true) ||
                        !proxyString(ctx, it->second, name, "ws-opts.headers 的值", value)) return false;
                    if (mapped.contains(key)) return proxyValueInvalid(ctx, name, "ws-opts.headers", "不能包含重复键");
                    mapped[key] = std::move(value);
                }
                transport["headers"] = std::move(mapped);
            }
            if (opts["max-early-data"]) {
                const auto earlyData = yint(opts, "max-early-data");
                if (!earlyData || *earlyData < 0)
                    return proxyValueInvalid(ctx, name, "ws-opts.max-early-data", "必须为非负整数");
                transport["max_early_data"] = *earlyData;
            }
            if (opts["early-data-header-name"]) {
                std::string earlyHeader;
                if (!proxyString(ctx, opts["early-data-header-name"], name,
                                 "ws-opts.early-data-header-name", earlyHeader)) return false;
                transport["early_data_header_name"] = std::move(earlyHeader);
            }
        }
        out["transport"] = std::move(transport);
        return true;
    }
    if (network == "grpc") {
        nlohmann::json transport = {{"type", "grpc"}};
        std::string service;
        if (item["grpc-service-name"] &&
            !proxyString(ctx, item["grpc-service-name"], name, "grpc-service-name", service)) return false;
        if (const YAML::Node opts = item["grpc-opts"]; opts && opts.IsMap()) {
            if (opts["grpc-service-name"]) {
                std::string nested;
                if (!proxyString(ctx, opts["grpc-service-name"], name, "grpc-opts.grpc-service-name", nested)) return false;
                if (!service.empty() && service != nested)
                    ctx.note(FidelityScope::Node, Fidelity::Approx, name,
                             std::format("节点「{}」的嵌套 grpc-service-name 覆盖顶层值", name));
                service = std::move(nested);
            }
        }
        if (!service.empty()) transport["service_name"] = service;
        out["transport"] = std::move(transport);
        return true;
    }
    if (network == "h2" || network == "http") {
        nlohmann::json transport = {{"type", "http"}};
        const YAML::Node opts = network == "h2" ? item["h2-opts"] : item["http-opts"];
        if (opts && opts.IsMap()) {
            std::vector<std::string> paths, hosts;
            if (!proxyStringList(ctx, opts, "path", name, paths) ||
                !proxyStringList(ctx, opts, "host", name, hosts)) return false;
            if (!paths.empty()) {
                transport["path"] = paths.front();
                if (paths.size() > 1) ctx.note(FidelityScope::Node, Fidelity::Approx, name,
                    std::format("节点「{}」的 HTTP 传输 path 列表仅使用首项", name),
                    "sing-box 客户端只接受一个请求路径；请明确指定要使用的路径");
            }
            if (opts["host"]) transport["host"] = std::move(hosts);
        }
        out["transport"] = std::move(transport);
        return true;
    }
    if (network == "httpupgrade") {
        nlohmann::json transport = {{"type", "httpupgrade"}};
        if (const YAML::Node opts = item["httpupgrade-opts"]; opts && opts.IsMap()) {
            if (opts["path"]) {
                std::string path;
                if (!proxyString(ctx, opts["path"], name, "httpupgrade-opts.path", path)) return false;
                transport["path"] = std::move(path);
            }
            if (opts["host"]) {
                std::string host;
                if (!proxyString(ctx, opts["host"], name, "httpupgrade-opts.host", host)) return false;
                transport["host"] = std::move(host);
            }
        }
        out["transport"] = std::move(transport);
        return true;
    }
    ctx.note(FidelityScope::Node, Fidelity::Unsupported, name,
             std::format("节点「{}」的传输层 network:{} 暂不支持，已跳过", name, network),
             "把该节点的 network 换成 sing-box 支持的传输层（tcp/ws/grpc/http）");
    return false;
}

// ---- 节点转换 ---------------------------------------------------------------

bool proxyFieldsSupported(Context& ctx, const YAML::Node& item, const std::string& name,
                          std::initializer_list<std::string_view> supported) {
    bool valid = true;
    for (const auto& field : item) {
        const std::string key = field.first.IsScalar() ? field.first.Scalar() : "(无效字段名)";
        if (std::ranges::find(supported, key) != supported.end()) continue;
        ctx.note(FidelityScope::Node, Fidelity::Unsupported, name,
                 std::format("节点「{}」的 {} 尚未映射，已跳过", name, key),
                 "请用原生 sing-box JSON 表达该线路；编译器不会省略未映射字段后改变线路语义");
        valid = false;
    }
    return valid;
}

// Older protocol branches must enforce the same fidelity boundary as AnyTLS
// and Snell. In particular, certificate constraints and combination transports
// cannot disappear while the plain node is kept.
bool legacyProxyFieldsSupported(Context& ctx, const YAML::Node& item,
                                const std::string& name, const std::string& type) {
    std::vector<std::string_view> fields{
        "name", "type", "server", "port", "udp", "dialer-proxy",
        "interface-name", "routing-mark", "tfo", "mptcp"};
    const auto add = [&](std::initializer_list<std::string_view> keys) {
        fields.insert(fields.end(), keys.begin(), keys.end());
    };
    if (type == "shadowsocks") add({"cipher", "password", "plugin", "plugin-opts", "udp-over-tcp"});
    else if (type == "vmess") add({"uuid", "cipher", "alterId"});
    else if (type == "vless") add({"uuid", "flow"});
    else if (type == "trojan") add({"password"});
    else if (type == "hysteria2") add({"password", "up", "down", "obfs", "obfs-password", "ports"});
    else if (type == "tuic") add({"uuid", "password", "congestion-controller", "reduce-rtt", "disable-sni"});
    else if (type == "http" || type == "socks5") add({"username", "password"});
    else return true; // The protocol branch below reports an unknown type.
    if (type != "shadowsocks" && type != "socks5") {
        add({"tls", "sni", "servername", "skip-cert-verify", "alpn"});
        if (type != "hysteria2" && type != "tuic") add({"client-fingerprint", "reality-opts"});
    }
    const std::string network = lowerCopy(ytext(item, "network"));
    if (type == "vmess" || type == "vless" || type == "trojan") {
        add({"network"});
        if (network == "ws") add({"ws-opts"});
        else if (network == "grpc") add({"grpc-opts", "grpc-service-name"});
        else if (network == "h2") add({"h2-opts"});
        else if (network == "http") add({"http-opts"});
        else if (network == "httpupgrade") add({"httpupgrade-opts"});
    }
    bool valid = true;
    for (const auto& field : item) {
        const std::string key = field.first.IsScalar() ? field.first.Scalar() : "(无效字段名)";
        if (std::ranges::find(fields, key) != fields.end()) continue;
        ctx.note(FidelityScope::Node, Fidelity::Unsupported, name,
            std::format("节点「{}」的 {} 尚未映射，已跳过", name, key),
            "请用原生 sing-box JSON 表达该线路；不会省略证书约束或组合传输后改变线路语义");
        valid = false;
    }
    const auto nested = [&](const char* key, std::initializer_list<std::string_view> supported) {
        const auto value = item[key];
        if (!value) return true;
        if (!value.IsMap()) {
            ctx.note(FidelityScope::Node, Fidelity::Unsupported, name,
                std::format("节点「{}」的 {} 必须为对象，已跳过", name, key));
            return false;
        }
        return proxyFieldsSupported(ctx, value, name, supported);
    };
    valid = nested("reality-opts", {"public-key", "short-id"}) && valid;
    valid = nested("ws-opts", {"path", "headers", "max-early-data", "early-data-header-name"}) && valid;
    valid = nested("grpc-opts", {"grpc-service-name"}) && valid;
    valid = nested("h2-opts", {"path", "host"}) && valid;
    valid = nested("http-opts", {"path", "host"}) && valid;
    valid = nested("httpupgrade-opts", {"path", "host"}) && valid;
    if (item["plugin-opts"] && ytext(item, "plugin").empty()) {
        ctx.note(FidelityScope::Node, Fidelity::Unsupported, name,
            std::format("节点「{}」包含未启用插件的 plugin-opts，已跳过", name));
        valid = false;
    }
    if ((type == "trojan" || type == "hysteria2" || type == "tuic") &&
        item["tls"] && !ybool(item, "tls")) {
        ctx.note(FidelityScope::Node, Fidelity::Unsupported, name,
            std::format("节点「{}」的协议不能关闭 TLS，已跳过", name));
        valid = false;
    }
    return valid;
}

bool proxyBooleansValid(Context& ctx, const YAML::Node& item, const std::string& name,
                        std::initializer_list<const char*> fields) {
    for (const char* field : fields) {
        if (!item[field]) continue;
        const std::string value = lowerCopy(ytext(item, field));
        if (value == "true" || value == "false" || value == "yes" || value == "no" ||
            value == "1" || value == "0") continue;
        ctx.note(FidelityScope::Node, Fidelity::Unsupported, name,
                 std::format("节点「{}」的 {} 不是有效布尔值，已跳过", name, field),
                 "使用 true/false，避免无效字段被当成关闭状态");
        return false;
    }
    return true;
}

// Shared native dialer options. Validate presence before platform filtering so
// malformed values never silently turn into the defaults.
bool appendProxyDialer(Context& ctx, const YAML::Node& item, nlohmann::json& out,
                       const std::string& name) {
    const auto invalid = [&](std::string_view field, std::string_view reason) {
        ctx.note(FidelityScope::Node, Fidelity::Unsupported, name,
                 std::format("节点「{}」的 {} {}，已跳过", name, field, reason));
        return false;
    };
    if (!proxyBooleansValid(ctx, item, name, {"tfo", "mptcp"})) return false;
    if (const YAML::Node dialer = item["dialer-proxy"]; dialer) {
        if (!dialer.IsScalar() || trimCopy(dialer.Scalar()).empty())
            return invalid("dialer-proxy", "必须为非空出站名");
        out["detour"] = trimCopy(dialer.Scalar());
    }
    bool activeOptions = false;
    if (const YAML::Node interface = item["interface-name"]; interface) {
        if (!interface.IsScalar()) return invalid("interface-name", "必须为接口名字符串");
        const std::string value = trimCopy(interface.Scalar());
        if (!value.empty()) {
#if defined(__ANDROID__) || defined(CLASHFLUX_IOS)
            if (!out.contains("detour")) return invalid("interface-name", "在当前手机平台没有原生等价");
#endif
            out["bind_interface"] = value;
            activeOptions = true;
        }
    }
    if (const YAML::Node mark = item["routing-mark"]; mark) {
        if (!mark.IsScalar()) return invalid("routing-mark", "必须为 uint32 整数");
        const std::string text = trimCopy(mark.Scalar());
        unsigned long long value = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (text.empty() || error != std::errc{} || end != text.data() + text.size() ||
            value > 4294967295ULL) return invalid("routing-mark", "必须在 0–4294967295 之间");
#if !defined(__linux__) || defined(__ANDROID__)
        if (value != 0 && !out.contains("detour"))
            return invalid("routing-mark", "仅支持 Linux 桌面");
#endif
        out["routing_mark"] = value;
        activeOptions = activeOptions || value != 0;
    }
    for (const auto& [source, destination] : {
             std::pair{"tfo", "tcp_fast_open"}, std::pair{"mptcp", "tcp_multi_path"}}) {
        if (!item[source]) continue;
        const bool value = ybool(item, source);
        out[destination] = value;
        activeOptions = activeOptions || value;
    }
    if (out.contains("detour") && activeOptions) {
        ctx.note(FidelityScope::Node, Fidelity::Approx, name,
                 std::format("节点「{}」使用 dialer-proxy，内核会忽略本节点的接口、路由标记及 TCP 拨号选项", name),
                 "物理网络选项需要放到代理链最终拨号的出站；不会自动迁移或继承这些字段");
    } else if (ybool(item, "mptcp")) {
        ctx.note(FidelityScope::Node, Fidelity::Approx, name,
                 std::format("节点「{}」已启用原生 TCP Multi Path；1.14.2 普通 IPv6 拨号路径未启用此选项", name),
                 "实际多路径还依赖操作系统与对端；不能承诺所有地址族都启用 MPTCP");
    }
    return true;
}

std::optional<nlohmann::json> convertProxy(Context& ctx, const YAML::Node& item) {
    const std::string name = ytext(item, "name");
    const std::string type = lowerCopy(ytext(item, "type"));
    if (name.empty() || type.empty()) {
        ctx.note(FidelityScope::Node, Fidelity::Unsupported, "(未命名节点)",
                 "订阅包含无名称或类型的节点，已跳过",
                 "订阅里有一个没有 name/type 的条目，它不会出现在节点列表里");
        return std::nullopt;
    }
    if (ctx.tagKnown(name)) {
        ctx.note(FidelityScope::Node, Fidelity::Unsupported, name,
                 std::format("订阅包含重名节点「{}」，仅保留首个", name),
                 "同名节点只保留第一个；改名后两份才会都生效");
        return std::nullopt;
    }
    const std::string server = ytext(item, "server");
    const auto port = yint(item, "port");
    if (server.empty() || !port || *port <= 0 || *port > 65535) {
        ctx.note(FidelityScope::Node, Fidelity::Unsupported, name,
                 std::format("节点「{}」缺少有效的 server/port，已跳过", name),
                 "补上 server/port 之后该节点才会生效");
        return std::nullopt;
    }

    nlohmann::json out = {{"tag", name}};
    std::string singboxType = type;
    if (type == "ss") singboxType = "shadowsocks";
    if (type == "hy2") singboxType = "hysteria2";
    out["type"] = singboxType;
    out["server"] = server;
    out["server_port"] = *port;

    if (!legacyProxyFieldsSupported(ctx, item, name, singboxType)) return std::nullopt;
    if (singboxType != "anytls" && singboxType != "snell" &&
        !proxyBooleansValid(ctx, item, name,
            {"udp", "tls", "skip-cert-verify", "udp-over-tcp", "reduce-rtt", "disable-sni"}))
        return std::nullopt;

    for (const char* field : {"password", "username", "psk", "obfs-password"}) {
        if (item[field] && !item[field].IsScalar()) {
            proxyValueInvalid(ctx, name, field, "必须为凭据字符串");
            return std::nullopt;
        }
    }

    if (singboxType == "shadowsocks") {
        out["method"] = ytext(item, "cipher");
        if (item["password"]) out["password"] = yliteral(item, "password");
        if (!ytext(item, "plugin").empty()) {
            ctx.note(FidelityScope::Node, Fidelity::Unsupported, name,
                     std::format("节点「{}」使用 SS 插件（obfs/v2ray-plugin），暂不支持，已跳过", name),
                     "该节点已从所有组中移除；需要插件时请改用 sing-box 支持的协议");
            return std::nullopt;
        }
        if (ybool(item, "udp-over-tcp")) out["udp_over_tcp"] = true;
    } else if (singboxType == "anytls") {
        // AnyTLS 必须启用 TLS，即使订阅没有额外的 tls/sni 字段。
        // 组合伪装不能省掉后继续连普通 TLS，否则实际线路已改变。
        if (!proxyFieldsSupported(ctx, item, name,
                {"name", "type", "server", "port", "password", "udp", "tls",
                 "sni", "servername", "skip-cert-verify", "client-fingerprint", "alpn",
                 "idle-session-check-interval", "idle-session-timeout", "min-idle-session",
                 "client-metadata", "dialer-proxy", "interface-name", "routing-mark", "tfo", "mptcp"})) return std::nullopt;
        if (!proxyBooleansValid(ctx, item, name, {"udp", "tls", "skip-cert-verify"})) {
            return std::nullopt;
        }
        // 凭据与客户端元数据不能 trim：前后空白也是用户输入的一部分。
        const YAML::Node credential = item["password"];
        const std::string password = credential && credential.IsScalar() ? credential.Scalar() : "";
        if (password.empty()) {
            ctx.note(FidelityScope::Node, Fidelity::Unsupported, name,
                     std::format("AnyTLS 节点「{}」缺少 password，已跳过", name),
                     "补上 AnyTLS 密码后重新导入");
            return std::nullopt;
        }
        out["password"] = password;
        for (const auto& [source, destination] : {
                 std::pair{"idle-session-check-interval", "idle_session_check_interval"},
                 std::pair{"idle-session-timeout", "idle_session_timeout"},
                 std::pair{"min-idle-session", "min_idle_session"}}) {
            if (!item[source]) continue;
            const auto value = yint(item, source);
            if (!value || *value < 0) {
                ctx.note(FidelityScope::Node, Fidelity::Unsupported, name,
                         std::format("AnyTLS 节点「{}」的 {} 必须为非负整数，已跳过", name, source),
                         "会话时间使用秒数，最少空闲会话使用数量");
                return std::nullopt;
            }
            if (std::string_view(source) == "min-idle-session") out[destination] = *value;
            else out[destination] = std::format("{}s", *value);
        }
        if (const YAML::Node metadata = item["client-metadata"]; metadata) {
            if (!metadata.IsScalar()) {
                ctx.note(FidelityScope::Node, Fidelity::Unsupported, name,
                         std::format("AnyTLS 节点「{}」的 client-metadata 必须为字符串，已跳过", name),
                         "client-metadata 可为空字符串，但不能为对象或列表");
                return std::nullopt;
            }
            out["client_metadata"] = metadata.Scalar();
        }
        if (!appendTls(ctx, item, out, true)) return std::nullopt;
        if (!ybool(item, "udp")) {
            ctx.note(FidelityScope::Node, Fidelity::Approx, name,
                     std::format("AnyTLS 节点「{}」未启用 UDP，但 sing-box AnyTLS 出站固定支持 TCP/UDP", name),
                     "sing-box 没有该出站的 network 限制；如需禁止 UDP，请在路由中显式拒绝");
        }
    } else if (singboxType == "snell") {
        if (!proxyFieldsSupported(ctx, item, name,
                {"name", "type", "server", "port", "version", "psk", "udp", "reuse",
                 "obfs-opts", "dialer-proxy", "interface-name", "routing-mark", "tfo", "mptcp"})) return std::nullopt;
        if (!proxyBooleansValid(ctx, item, name, {"udp", "reuse"})) return std::nullopt;
        // 1.14 的 Snell 出站仅有 v4/v6；Clash 未指定版本时按旧版本处理，
        // 不能擅自改成 v4。v6 目前仅通过原生 JSON 使用。
        const auto version = yint(item, "version");
        const YAML::Node credential = item["psk"];
        const std::string psk = credential && credential.IsScalar() ? credential.Scalar() : "";
        if (!version || *version != 4 || psk.empty()) {
            ctx.note(FidelityScope::Node, Fidelity::Unsupported, name,
                     std::format("Snell 节点「{}」需要显式 version: 4 和非空 psk，已跳过", name),
                     "sing-box 1.14 不支持 Clash 的 Snell v1/v2/v3/v5；版本不能自动替换");
            return std::nullopt;
        }
        out["version"] = 4;
        out["psk"] = psk;
        out["reuse"] = ybool(item, "reuse");
        if (!ybool(item, "udp")) out["network"] = "tcp";
        if (const YAML::Node obfs = item["obfs-opts"]; obfs) {
            const std::string mode = obfs.IsMap() ? lowerCopy(ytext(obfs, "mode")) : "(无效)";
            if (!mode.empty() && mode != "http" && mode != "tls") {
                ctx.note(FidelityScope::Node, Fidelity::Unsupported, name,
                         std::format("Snell 节点「{}」的 obfs 模式 {} 尚未映射，已跳过", name, mode),
                         "仅支持 http/tls 伪装；shadow-tls/restls/jls 不会降成无伪装连接");
                return std::nullopt;
            }
            if (!proxyFieldsSupported(ctx, obfs, name, {"mode", "host"})) return std::nullopt;
            if (!mode.empty()) {
                out["obfs_mode"] = mode;
                const std::string host = ytext(obfs, "host");
                out["obfs_host"] = host.empty() ? "bing.com" : host;
            }
        }
    } else if (singboxType == "vmess") {
        out["uuid"] = ytext(item, "uuid");
        std::string security = ytext(item, "cipher");
        if (security.empty()) security = "auto";
        out["security"] = lowerCopy(security);
        if (const auto alterId = yint(item, "alterId"); alterId) {
            out["alter_id"] = *alterId;
        }
        if (!appendTransport(ctx, item, out, name)) return std::nullopt;
        if (!appendTls(ctx, item, out)) return std::nullopt;
    } else if (singboxType == "vless") {
        out["uuid"] = ytext(item, "uuid");
        if (const std::string flow = ytext(item, "flow"); !flow.empty()) {
            out["flow"] = flow;
        }
        if (!appendTransport(ctx, item, out, name)) return std::nullopt;
        if (!appendTls(ctx, item, out)) return std::nullopt;
    } else if (singboxType == "trojan") {
        if (item["password"]) out["password"] = yliteral(item, "password");
        if (!appendTransport(ctx, item, out, name)) return std::nullopt;
        if (!appendTls(ctx, item, out, true)) return std::nullopt;
    } else if (singboxType == "hysteria2") {
        if (item["password"]) out["password"] = yliteral(item, "password");
        // 整数 Mbps 可精确映射；不能把小数或其它单位截断后改变带宽。
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
            const std::string unit = lowerCopy(trimCopy(digits.substr(end)));
            if (!unit.empty() && unit != "mbps") return std::nullopt;
            int parsed = 0;
            const auto [ptr, ec] =
                std::from_chars(digits.data(), digits.data() + end, parsed);
            if (ec != std::errc()) return std::nullopt;
            return parsed;
        };
        for (const auto& [source, target] : {std::pair{"up", "up_mbps"}, std::pair{"down", "down_mbps"}}) {
            if (!item[source]) continue;
            const auto value = leadingNumber(ytext(item, source));
            if (!value || *value <= 0) {
                ctx.note(FidelityScope::Node, Fidelity::Unsupported, name,
                    std::format("节点「{}」的 {} 不是有效的正数带宽，已跳过", name, source));
                return std::nullopt;
            }
            out[target] = *value;
        }
        if (const std::string obfsPassword = yliteral(item, "obfs-password"); !obfsPassword.empty()) {
            out["obfs"] = {{"type", "salamander"}, {"password", obfsPassword}};
        } else if (const std::string obfs = ytext(item, "obfs");
                   !obfs.empty() && lowerCopy(obfs) != "none") {
            ctx.note(FidelityScope::Node, Fidelity::Approx, name,
                     std::format("节点「{}」的 obfs 配置缺少 obfs-password，已忽略", name),
                     "该节点会以不带 obfs 的方式连接；补上 obfs-password 才会带伪装");
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
        if (!appendTls(ctx, item, out, true, false)) return std::nullopt;
    } else if (singboxType == "tuic") {
        out["uuid"] = ytext(item, "uuid");
        if (item["password"]) out["password"] = yliteral(item, "password");
        if (const std::string congestion =
                ytext(item, "congestion-controller"); !congestion.empty()) {
            out["congestion_control"] = congestion;
        }
        if (ybool(item, "reduce-rtt")) out["zero_rtt_handshake"] = true;
        if (!appendTls(ctx, item, out, true, false)) return std::nullopt;
    } else if (singboxType == "http" || singboxType == "socks5") {
        if (singboxType == "socks5") out["type"] = "socks";
        if (item["username"]) out["username"] = yliteral(item, "username");
        if (item["password"]) out["password"] = yliteral(item, "password");
        if (!appendTls(ctx, item, out)) return std::nullopt;
    } else {
        ctx.note(FidelityScope::Node, Fidelity::Unsupported, name,
                 std::format("节点「{}」的协议 {} 暂不支持，已跳过", name, type),
                 "该节点已从所有组中移除；可在订阅里删除它，或等编译器补上该协议");
        return std::nullopt;
    }
    if (!appendProxyDialer(ctx, item, out, name)) return std::nullopt;
    if (item["udp"] && singboxType != "anytls" && singboxType != "snell") {
        if (singboxType == "http") {
            if (ybool(item, "udp")) ctx.note(FidelityScope::Node, Fidelity::Approx, name,
                std::format("HTTP 节点「{}」启用了 UDP，但原生 HTTP 出站仅转发 TCP", name));
        } else if (!ybool(item, "udp")) {
            out["network"] = "tcp";
        }
    }
    return out;
}

// ---- 代理组转换 --------------------------------------------------------------

std::optional<nlohmann::json> convertGroup(Context& ctx, const YAML::Node& item,
                                           const std::string& name,
                                           const std::vector<std::string>& members) {
    const std::string clashType = lowerCopy(ytext(item, "type"));
    for (const auto& field : item) {
        const std::string key = field.first.IsScalar() ? field.first.Scalar() : "(无效字段名)";
        if (key == "name" || key == "type" || key == "proxies" || key == "default-selected" ||
            key == "use" || key == "include-all" || key == "include-all-proxies" || key == "include-all-providers") continue;
        if ((clashType == "url-test" || clashType == "fallback" || clashType == "load-balance") &&
            (key == "url" || key == "interval" || key == "tolerance")) continue;
        ctx.note(FidelityScope::Group, Fidelity::Approx, name,
                 std::format("代理组「{}」的 {} 未映射，仍使用内核组行为", name, key),
                 "该字段不会写入内核；lazy/timeout/expected-status 等不能假装有等价设置");
    }
    nlohmann::json out = {{"tag", name}};
    if (clashType == "select") {
        out["type"] = "selector";
    } else if (clashType == "url-test" || clashType == "fallback" ||
               clashType == "load-balance") {
        if (clashType != "url-test") {
            ctx.note(FidelityScope::Group, Fidelity::Approx, name,
                     std::format("代理组「{}」类型 {} 在 sing-box 无对应语义，降级为 urltest",
                                 name, clashType),
                     "sing-box 没有 fallback / load-balance：该组现在是「最低延迟」，"
                     "不再是顺序兜底 / 轮询分发");
        }
        if (ctx.opt.speedTestOnly) {
            // A speed-only libbox instance performs explicit outbound tests
            // from Android.  Keeping imported URLTest groups here would make
            // sing-box start its own automatic sweep at the same time, which
            // doubles the connections and mixes its configured URL with the
            // CommandClient default URL.  Keep the group as a selector so the
            // page still exposes the same members without a second sweep.
            out["type"] = "selector";
        } else {
            out["type"] = "urltest";
            std::string url = ytext(item, "url");
            if (url.empty()) url = "https://www.gstatic.com/generate_204";
            out["url"] = url;
            if (const auto interval = yint(item, "interval"); interval && *interval > 0) {
                out["interval"] = std::format("{}s", *interval);
                if (*interval > 1800) {
                    out["idle_timeout"] = std::format("{}s", *interval);
                    ctx.note(FidelityScope::Group, Fidelity::Approx, name,
                             std::format("代理组「{}」保留 {}s 测速间隔，并把 idle_timeout 从默认 30m 延长到同一值", name, *interval),
                             "sing-box 要求 interval ≤ idle_timeout；空闲时停止巡检的时间因此延长");
                }
            } else if (item["interval"]) {
                ctx.note(FidelityScope::Group, Fidelity::Approx, name,
                         std::format("代理组「{}」的 interval 无效，使用 sing-box 默认 3m", name),
                         "interval 必须为正整数秒数");
            }
            if (item["tolerance"]) {
                const auto tolerance = yint(item, "tolerance");
                if (tolerance && *tolerance > 0 && *tolerance <= 65535) {
                    out["tolerance"] = *tolerance;
                } else {
                    ctx.note(FidelityScope::Group, Fidelity::Approx, name,
                             std::format("代理组「{}」的 tolerance 无法原样表达，使用 sing-box 默认 50ms", name),
                             "sing-box 接受 1–65535ms；0 会被内核替换成 50ms，不能表示零容差");
                }
            }
        }
    } else {
        ctx.note(FidelityScope::Group, Fidelity::Unsupported, name,
                 std::format("代理组「{}」类型 {} 暂不支持，已跳过", name, clashType),
                 "该组已从订阅里移除；把 type 改成 select / url-test 才会被编译");
        return std::nullopt;
    }
    // 成员必须是已知的节点/组/DIRECT；REJECT 等特殊目标在 1.14 无对应 outbound，
    // 过滤并提示（sing-box 对未知 outbound tag 直接拒绝启动）。
    nlohmann::json filtered = nlohmann::json::array();
    for (const std::string& member : members) {
        if (member == "REJECT" || member == "REJECT-DROP" || member == "PASS") {
            ctx.note(FidelityScope::Group, Fidelity::Approx, name,
                     std::format("代理组「{}」的成员 {} 在 sing-box 无对应 outbound，已移除",
                                 name, member),
                     "sing-box 1.14 的 REJECT 是规则动作而不是出站，因此该成员不会"
                     "出现在卡片列表里");
            continue;
        }
        if (member != "DIRECT" && !ctx.tagKnown(member)) {
            ctx.note(FidelityScope::Group, Fidelity::Approx, name,
                     std::format("代理组「{}」引用了未知的成员「{}」，已移除", name, member),
                     "检查订阅里被引用的成员名是否写错，或该节点是否因协议不支持被跳过");
            continue;
        }
        filtered.push_back(member);
    }
    if (filtered.empty()) filtered.push_back("DIRECT");
    if (const YAML::Node initial = item["default-selected"]; initial) {
        const std::string selected = ytext(item, "default-selected");
        if (out.value("type", "") == "selector" && initial.IsScalar() && !selected.empty() &&
            std::find(filtered.begin(), filtered.end(), nlohmann::json(selected)) != filtered.end()) {
            out["default"] = selected;
        } else {
            ctx.note(FidelityScope::Group, Fidelity::Approx, name,
                     std::format("代理组「{}」的 default-selected 未生效", name),
                     "仅 selector 支持默认选择，目标必须是该组编译后的成员；其它情况沿用内核默认行为");
        }
    }
    out["outbounds"] = std::move(filtered);
    return out;
}

// A detour may point forward to a node or group. Validate the completed graph,
// including every selectable member, so switching groups cannot create a loop.
// Use an explicit stack: untrusted subscriptions can contain long chains.
bool validateOutboundDependencies(Context& ctx) {
    const std::size_t count = ctx.outbounds.size();
    std::map<std::string, std::size_t> indices;
    std::vector<std::string> tags;
    tags.reserve(count);
    auto fail = [&](std::size_t index, const std::string& detail) {
        const bool group = ctx.outbounds[index].value("type", "") == "selector" ||
                           ctx.outbounds[index].value("type", "") == "urltest";
        ctx.note(group ? FidelityScope::Group : FidelityScope::Node, Fidelity::Unsupported,
                 tags[index], detail, "修正引用或代理链；编译器不会移除 detour 后改成直连");
        ctx.result.error = detail;
        return false;
    };
    for (std::size_t index = 0; index < count; ++index) {
        tags.push_back(ctx.outbounds[index].value("tag", ""));
        if (!indices.emplace(tags.back(), index).second) {
            return fail(index, std::format("出站「{}」重名，无法唯一解析依赖", tags.back()));
        }
    }
    std::vector<std::vector<std::size_t>> edges(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto& outbound = ctx.outbounds[index];
        std::vector<std::string> dependencies;
        if (outbound.contains("detour")) dependencies.push_back(outbound["detour"].get<std::string>());
        if (outbound.contains("outbounds")) {
            for (const auto& member : outbound["outbounds"]) dependencies.push_back(member.get<std::string>());
        }
        for (const auto& target : dependencies) {
            const auto found = indices.find(target);
            if (found == indices.end()) {
                return fail(index, std::format("出站「{}」依赖的「{}」未成功编译", tags[index], target));
            }
            edges[index].push_back(found->second);
        }
    }
    struct Frame { std::size_t index; std::size_t next = 0; };
    std::vector<unsigned char> colors(count, 0);
    std::vector<Frame> stack;
    for (std::size_t start = 0; start < count; ++start) {
        if (colors[start] != 0) continue;
        stack.push_back({start});
        colors[start] = 1;
        while (!stack.empty()) {
            auto& frame = stack.back();
            if (frame.next == edges[frame.index].size()) {
                colors[frame.index] = 2;
                stack.pop_back();
                continue;
            }
            const std::size_t target = edges[frame.index][frame.next++];
            if (colors[target] == 1) {
                std::string path;
                const auto beginning = std::find_if(stack.begin(), stack.end(),
                    [&](const Frame& entry) { return entry.index == target; });
                for (auto cursor = beginning; cursor != stack.end(); ++cursor) {
                    if (!path.empty()) path += " → ";
                    path += tags[cursor->index];
                }
                path += " → " + tags[target];
                return fail(frame.index, "出站依赖形成循环：" + path);
            }
            if (colors[target] == 0) {
                colors[target] = 1;
                stack.push_back({target});
            }
        }
    }
    return true;
}

// ---- 规则转换 -----------------------------------------------------------------
