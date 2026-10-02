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

bool proxyHeaders(Context& ctx, const YAML::Node& headers, const std::string& name,
                  std::string_view field, nlohmann::json& result) {
    if (!headers.IsMap()) return proxyValueInvalid(ctx, name, field, "必须为对象");
    result = nlohmann::json::object();
    for (auto it = headers.begin(); it != headers.end(); ++it) {
        std::string key, value;
        if (!proxyString(ctx, it->first, name, std::format("{} 的键", field), key, true) ||
            !proxyString(ctx, it->second, name, std::format("{} 的值", field), value)) return false;
        if (result.contains(key)) return proxyValueInvalid(ctx, name, field, "不能包含重复键");
        result[key] = std::move(value);
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
        if (item["ws-path"]) {
            std::string path;
            if (!proxyString(ctx, item["ws-path"], name, "ws-path", path)) return false;
            transport["path"] = std::move(path);
        }
        if (item["ws-headers"]) {
            nlohmann::json headers;
            if (!proxyHeaders(ctx, item["ws-headers"], name, "ws-headers", headers)) return false;
            transport["headers"] = std::move(headers);
        }
        const auto noteOverride = [&](const char* field, const nlohmann::json& value) {
            if (transport.contains(field) && transport[field] != value)
                ctx.note(FidelityScope::Node, Fidelity::Approx, name,
                    std::format("节点「{}」的 ws-opts.{} 覆盖不同的旧版 ws-{}", name, field, field),
                    "统一新旧 WebSocket 字段；相同的重复值不会导致节点降级");
        };
        if (const YAML::Node opts = item["ws-opts"]; opts && opts.IsMap()) {
            if (opts["path"]) {
                std::string path;
                if (!proxyString(ctx, opts["path"], name, "ws-opts.path", path)) return false;
                noteOverride("path", path);
                transport["path"] = std::move(path);
            }
            if (const YAML::Node headers = opts["headers"]; headers) {
                nlohmann::json mapped;
                if (!proxyHeaders(ctx, headers, name, "ws-opts.headers", mapped)) return false;
                noteOverride("headers", mapped);
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
    if (type == "shadowsocks") add({"cipher", "password", "plugin", "plugin-opts", "udp-over-tcp", "udp-over-tcp-version"});
    else if (type == "vmess") add({"uuid", "cipher", "alterId", "packet-encoding", "packet-addr", "xudp",
                                    "global-padding", "authenticated-length"});
    else if (type == "vless") add({"uuid", "flow", "encryption", "packet-encoding", "packet-addr", "xudp"});
    else if (type == "trojan") add({"password"});
    else if (type == "hysteria") add({"auth", "auth-str", "up", "down", "obfs", "protocol", "obfs-protocol",
                                     "ports", "hop-interval", "fast-open", "recv-window", "recv-window-conn", "disable-mtu-discovery"});
    else if (type == "hysteria2") add({"password", "up", "down", "obfs", "obfs-password", "ports", "hop-interval",
                                      "obfs-min-packet-size", "obfs-max-packet-size"});
    else if (type == "tuic") add({"uuid", "password", "congestion-controller", "reduce-rtt", "disable-sni"});
    else if (type == "http" || type == "socks5") add({"username", "password"});
    else if (type == "ssh") add({"username", "password", "private-key", "private-key-passphrase",
                                "host-key", "host-key-algorithms"});
    else return true; // The protocol branch below reports an unknown type.
    if (type != "shadowsocks" && type != "socks5" && type != "ssh") {
        add({"tls", "sni", "servername", "skip-cert-verify", "alpn"});
        if (type != "hysteria" && type != "hysteria2" && type != "tuic") add({"client-fingerprint", "reality-opts"});
    }
    const std::string network = lowerCopy(ytext(item, "network"));
    if (type == "vmess" || type == "vless" || type == "trojan") {
        add({"network"});
        if (network == "ws") add({"ws-opts", "ws-path", "ws-headers"});
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
    if ((type == "trojan" || type == "hysteria" || type == "hysteria2" || type == "tuic") &&
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

bool appendProxyUdpEncoding(Context& ctx, const YAML::Node& item, nlohmann::json& out,
                            const std::string& name, bool vless) {
    if (!proxyBooleansValid(ctx, item, name, {"packet-addr", "xudp"})) return false;
    if (!item["packet-encoding"] && !item["packet-addr"] && !item["xudp"]) return true;
    std::string encoding;
    if (item["packet-encoding"] &&
        !proxyString(ctx, item["packet-encoding"], name, "packet-encoding", encoding)) return false;
    if (encoding != "" && encoding != "packetaddr" && encoding != "packet" && encoding != "xudp")
        return proxyValueInvalid(ctx, name, "packet-encoding", "仅支持空字符串、packetaddr/packet 或 xudp");
    bool packetAddr = ybool(item, "packet-addr"), xudp = ybool(item, "xudp");
    // Mihomo NewVmess and NewVless intentionally differ. VLESS enables XUDP
    // by default, and its explicit packetaddr overrides the legacy XUDP flag.
    if (encoding == "packetaddr" || encoding == "packet") {
        packetAddr = true;
        if (vless) xudp = false;
    } else if (vless) {
        if (!packetAddr) xudp = true;
    } else if (encoding == "xudp") {
        xudp = true;
    }
    if (xudp) packetAddr = false;
    out["packet_encoding"] = xudp ? "xudp" : packetAddr ? "packetaddr" : "";
    return true;
}

std::optional<unsigned long long> proxyUnsigned(std::string_view value, unsigned long long max) {
    const std::string text = trimCopy(value);
    unsigned long long number = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size() || number > max)
        return std::nullopt;
    return number;
}

// Input bandwidth units are case-sensitive: MBps is bytes, Mbps is bits.
// Keep an integer Mbps subset and reject units/values we cannot express exactly.
std::optional<int> proxyMbps(std::string_view raw) {
    const std::string text = trimCopy(raw);
    std::size_t end = 0;
    while (end < text.size() && text[end] >= '0' && text[end] <= '9') ++end;
    const std::string unit = trimCopy(std::string_view(text).substr(end));
    if (unit != "" && unit != "Mbps" && unit != "MBps") return std::nullopt;
    const unsigned long long multiplier = unit == "MBps" ? 8 : 1;
    const auto number = proxyUnsigned(std::string_view(text).substr(0, end),
                                      static_cast<unsigned long long>(std::numeric_limits<int>::max()) / multiplier);
    if (!number || *number == 0) return std::nullopt;
    return static_cast<int>(*number * multiplier);
}

bool appendProxyBandwidth(Context& ctx, const YAML::Node& item, nlohmann::json& out,
                           const std::string& name, bool required) {
    for (const auto& [source, target] : {std::pair{"up", "up_mbps"}, std::pair{"down", "down_mbps"}}) {
        if (!required && !item[source]) continue;
        const auto value = item[source] && item[source].IsScalar() ? proxyMbps(item[source].Scalar()) : std::nullopt;
        if (!value) return proxyValueInvalid(ctx, name, source,
            "必须为正整数 Mbps（裸整数或 Mbps 单位），MBps 按字节转换；其它单位/小数尚未接入或数值超限");
        out[target] = *value;
    }
    return true;
}

// Go base64.StdEncoding ignores CR/LF, requires standard padding, and accepts
// nonzero unused padding bits. Preserve the encoded input for native []byte JSON.
std::optional<std::size_t> proxyBase64Size(std::string_view encoded) {
    std::string text;
    for (char c : encoded) if (c != '\r' && c != '\n') text.push_back(c);
    if (text.size() % 4 != 0) return std::nullopt;
    if (text.empty()) return 0;
    const std::size_t padding = text.ends_with("==") ? 2 : text.ends_with("=") ? 1 : 0;
    for (std::size_t i = 0; i < text.size() - padding; ++i) {
        const unsigned char c = text[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '+' || c == '/')) return std::nullopt;
    }
    return text.size() / 4 * 3 - padding;
}

std::string sip003Escape(std::string_view value) {
    std::string escaped;
    for (const char ch : value) {
        if (ch == '\\' || ch == ';' || ch == '=') escaped += '\\';
        escaped += ch;
    }
    return escaped;
}

bool appendShadowsocksPlugin(Context& ctx, const YAML::Node& item, nlohmann::json& out,
                             const std::string& name) {
    std::string plugin;
    if (item["plugin"] && !proxyString(ctx, item["plugin"], name, "plugin", plugin)) return false;
    if (plugin.empty()) return true;
    if (plugin != "obfs" && plugin != "v2ray-plugin")
        return proxyValueInvalid(ctx, name, "plugin", "尚未映射此插件，不能改成普通 SS 线路");
    const YAML::Node options = item["plugin-opts"];
    if (!options || !options.IsMap())
        return proxyValueInvalid(ctx, name, "plugin-opts", "插件需要选项对象和显式 mode");
    std::vector<std::string> keys;
    for (const auto& field : options) {
        if (!field.first.IsScalar()) return proxyValueInvalid(ctx, name, "plugin-opts", "键必须为字符串");
        const std::string key = field.first.Scalar();
        if (std::ranges::find(keys, key) != keys.end())
            return proxyValueInvalid(ctx, name, "plugin-opts", "不能包含重复键");
        keys.push_back(key);
    }
    if (plugin == "obfs") {
        if (!proxyFieldsSupported(ctx, options, name, {"mode", "host"})) return false;
    } else {
        if (!proxyFieldsSupported(ctx, options, name,
                {"mode", "host", "path", "tls", "mux", "headers", "skip-cert-verify",
                 "v2ray-http-upgrade", "v2ray-http-upgrade-fast-open"}) ||
            !proxyBooleansValid(ctx, options, name,
                {"tls", "mux", "skip-cert-verify", "v2ray-http-upgrade", "v2ray-http-upgrade-fast-open"})) return false;
        for (const char* field : {"skip-cert-verify", "v2ray-http-upgrade", "v2ray-http-upgrade-fast-open"})
            if (ybool(options, field)) return proxyValueInvalid(ctx, name, std::format("plugin-opts.{}", field),
                "固定内核的 SIP003 插件没有等价选项");
        if (options["headers"]) {
            nlohmann::json headers;
            if (!proxyHeaders(ctx, options["headers"], name, "plugin-opts.headers", headers)) return false;
            if (!headers.empty()) return proxyValueInvalid(ctx, name, "plugin-opts.headers",
                "固定内核的 SIP003 插件不能传入自定义 headers");
        }
    }
    std::string mode, host = "bing.com";
    if (!options["mode"] || !proxyString(ctx, options["mode"], name, "plugin-opts.mode", mode, true)) {
        if (!options["mode"]) proxyValueInvalid(ctx, name, "plugin-opts.mode", "必须显式指定");
        return false;
    }
    if (options["host"] && !proxyString(ctx, options["host"], name, "plugin-opts.host", host, true)) return false;
    if (plugin == "obfs") {
        if (mode != "http" && mode != "tls")
            return proxyValueInvalid(ctx, name, "plugin-opts.mode", "simple-obfs 仅支持 http/tls");
        out["plugin"] = "obfs-local";
        out["plugin_opts"] = "obfs=" + sip003Escape(mode) + ";obfs-host=" + sip003Escape(host);
    } else {
        if (mode != "websocket")
            return proxyValueInvalid(ctx, name, "plugin-opts.mode", "Clash v2ray-plugin 仅支持 websocket，不能改写模式");
        std::string path = "/";
        if (options["path"] && !proxyString(ctx, options["path"], name, "plugin-opts.path", path)) return false;
        const bool mux = options["mux"] ? ybool(options, "mux") : true;
        out["plugin"] = "v2ray-plugin";
        std::string args = "mode=websocket;host=" + sip003Escape(host) + ";path=" + sip003Escape(path) +
                           (mux ? ";mux=1" : ";mux=0");
        // Native TLS is enabled by key presence, even tls=false would enable it.
        if (ybool(options, "tls")) args += ";tls";
        out["plugin_opts"] = std::move(args);
    }
    return true;
}

bool appendShadowsocksUot(Context& ctx, const YAML::Node& item, nlohmann::json& out,
                          const std::string& name) {
    if (!item["udp-over-tcp"] && !item["udp-over-tcp-version"]) return true;
    int version = 1; // Clash defaults to legacy UOT, native bool defaults to v2.
    if (item["udp-over-tcp-version"]) {
        const auto value = yint(item, "udp-over-tcp-version");
        if (!value || *value < 0 || *value > 2)
            return proxyValueInvalid(ctx, name, "udp-over-tcp-version", "仅支持 0（Clash 默认 v1）、1 或 2");
        if (*value != 0) version = *value;
    }
    out["udp_over_tcp"] = {{"enabled", ybool(item, "udp-over-tcp")}, {"version", version}};
    return true;
}

bool appendHysteriaPorts(Context& ctx, const YAML::Node& item, nlohmann::json& out,
                          const std::string& name) {
    std::string ports;
    if (item["ports"] && !proxyString(ctx, item["ports"], name, "ports", ports)) return false;
    if (!ports.empty()) {
        nlohmann::json serverPorts = nlohmann::json::array();
        std::string_view rest{ports};
        while (true) {
            const auto comma = rest.find(',');
            const std::string piece = trimCopy(rest.substr(0, comma));
            const auto dash = piece.find('-');
            const auto first = proxyUnsigned(std::string_view(piece).substr(0, dash), 65535);
            const auto last = dash == std::string::npos ? first :
                proxyUnsigned(std::string_view(piece).substr(dash + 1), 65535);
            if (!first || !last || *first == 0 || *last == 0)
                return proxyValueInvalid(ctx, name, "ports", "每项必须为 1–65535 端口或端口范围，不能有空项");
            // Native ParsePorts requires a colon even for singleton ports.
            // Mihomo NewRange normalizes reversed endpoints.
            serverPorts.push_back(std::format("{}:{}", std::min(*first, *last), std::max(*first, *last)));
            if (comma == std::string_view::npos) break;
            rest.remove_prefix(comma + 1);
        }
        out["server_ports"] = std::move(serverPorts);
        // Clash ignores the single port whenever hopping ports are configured.
        out.erase("server_port");
    }
    return true;
}

bool appendHysteria1Options(Context& ctx, const YAML::Node& item, nlohmann::json& out,
                            const std::string& name) {
    if (!appendProxyBandwidth(ctx, item, out, name, true) ||
        !proxyBooleansValid(ctx, item, name, {"fast-open", "disable-mtu-discovery"})) return false;
    std::string protocol, obfsProtocol, auth, authString, obfs;
    for (const auto& [field, value] : {std::pair{"protocol", &protocol}, std::pair{"obfs-protocol", &obfsProtocol},
            std::pair{"auth", &auth}, std::pair{"auth-str", &authString}, std::pair{"obfs", &obfs}}) {
        if (item[field] && !proxyString(ctx, item[field], name, field, *value)) return false;
    }
    // Mihomo's Stash-compatible obfs-protocol overrides protocol when nonempty.
    if (!obfsProtocol.empty()) protocol = obfsProtocol;
    if (!protocol.empty() && protocol != "udp")
        return proxyValueInvalid(ctx, name, "protocol/obfs-protocol", "固定内核仅支持 UDP 传输，不能改成普通 UDP 线路");
    if (ybool(item, "fast-open"))
        return proxyValueInvalid(ctx, name, "fast-open", "固定内核没有等价的协议快速打开选项");
    if (!auth.empty()) {
        const auto size = proxyBase64Size(auth);
        if (!size || *size > 65535) return proxyValueInvalid(ctx, name, "auth", "必须为标准 Base64，解码后不能超过 65535 字节");
        out["auth"] = std::move(auth); // Auth bytes override auth-str in Clash.
    } else {
        if (authString.size() > 65535) return proxyValueInvalid(ctx, name, "auth-str", "不能超过 65535 字节");
        if (item["auth-str"]) out["auth_str"] = std::move(authString);
    }
    if (item["obfs"]) out["obfs"] = std::move(obfs);
    if (item["disable-mtu-discovery"]) out["disable_path_mtu_discovery"] = ybool(item, "disable-mtu-discovery");
    unsigned long long streamWindow = 0, connectionWindow = 0;
    // Follow the actual Mihomo axes, not the misleading legacy names: its
    // recv-window-conn sets the stream window, recv-window the connection window.
    for (const auto& [field, value] : {std::pair{"recv-window-conn", &streamWindow}, std::pair{"recv-window", &connectionWindow}}) {
        if (!item[field]) continue;
        const auto parsed = item[field].IsScalar() ? proxyUnsigned(item[field].Scalar(), 2147483647) : std::nullopt;
        if (!parsed) return proxyValueInvalid(ctx, name, field, "必须为 0–2147483647 的字节数");
        *value = *parsed;
    }
    if ((streamWindow == 0) != (connectionWindow == 0))
        return proxyValueInvalid(ctx, name, "recv-window/recv-window-conn", "本批仅支持两者同时非零或同时使用默认值，单边窗口尚未保真");
    if (streamWindow == 0) {
        streamWindow = 15728640;
        connectionWindow = 67108864;
        ctx.note(FidelityScope::Node, Fidelity::Approx, name,
                 std::format("Hysteria 节点「{}」保留 Clash 默认窗口上限，原生 QUIC 初始窗口与 Clash 的渐增分配不同", name),
                 "固定内核窗口字段不能分别设置初始值与上限；需要完全一致时请显式设置两侧非零窗口");
    }
    out["stream_receive_window"] = streamWindow;
    out["connection_receive_window"] = connectionWindow;
    if (item["alpn"] && item["alpn"].IsSequence() && item["alpn"].size() == 0)
        return proxyValueInvalid(ctx, name, "alpn", "原生 Hysteria 将空列表恢复为协议默认，不能保留显式空 ALPN");
    if (!appendHysteriaPorts(ctx, item, out, name)) return false;
    unsigned long long interval = 10; // HY1 Clash default, native default is 30s.
    if (item["hop-interval"]) {
        const auto parsed = item["hop-interval"].IsScalar() ? proxyUnsigned(item["hop-interval"].Scalar(), 9223372036ULL) : std::nullopt;
        if (!parsed) return proxyValueInvalid(ctx, name, "hop-interval", "必须为非负整数秒，不能超出内核时长上限");
        if (*parsed != 0) interval = *parsed;
        if (!out.contains("server_ports")) ctx.note(FidelityScope::Node, Fidelity::Approx, name,
            std::format("节点「{}」未启用 ports，hop-interval 不生效", name));
    }
    if (out.contains("server_ports")) {
        if (interval < 5) return proxyValueInvalid(ctx, name, "hop-interval", "固定内核最小为 5 秒，不能改变显式跳端口间隔");
        out["hop_interval"] = std::format("{}s", interval);
    }
    return true;
}

bool appendHysteria2Options(Context& ctx, const YAML::Node& item, nlohmann::json& out,
                            const std::string& name) {
    std::string obfs;
    if (item["obfs"] && !proxyString(ctx, item["obfs"], name, "obfs", obfs)) return false;
    const std::string password = yliteral(item, "obfs-password");
    if (!obfs.empty()) {
        if (obfs != "salamander" && obfs != "gecko")
            return proxyValueInvalid(ctx, name, "obfs", "仅支持 salamander 或 gecko");
        if (password.empty()) return proxyValueInvalid(ctx, name, "obfs-password", "启用混淆时不能为空");
        out["obfs"] = {{"type", obfs}, {"password", password}};
    } else if (!password.empty()) {
        ctx.note(FidelityScope::Node, Fidelity::Approx, name,
                 std::format("节点「{}」未启用 obfs，obfs-password 不生效", name),
                 "与 Clash 行为一致；启用混淆需要显式指定 obfs 类型");
    }
    unsigned long long minSize = 0, maxSize = 0;
    for (const auto& [source, target, size] : {
             std::tuple{"obfs-min-packet-size", "min_packet_size", &minSize},
             std::tuple{"obfs-max-packet-size", "max_packet_size", &maxSize}}) {
        if (!item[source]) continue;
        const auto value = item[source].IsScalar() ? proxyUnsigned(item[source].Scalar(), 2048) : std::nullopt;
        if (!value) return proxyValueInvalid(ctx, name, source, "必须为 0–2048 的整数");
        *size = *value;
        if (obfs == "gecko") out["obfs"][target] = *value;
        else ctx.note(FidelityScope::Node, Fidelity::Approx, name,
                      std::format("节点「{}」未启用 gecko，{} 不生效", name, source));
    }
    // Fixed sing-quic gecko defaults: 512/1200, on-wire maximum 2048.
    if (obfs == "gecko" && (minSize == 0 ? 512 : minSize) > (maxSize == 0 ? 1200 : maxSize))
        return proxyValueInvalid(ctx, name, "obfs-min/max-packet-size", "有效最小值不能大于最大值");

    if (!appendHysteriaPorts(ctx, item, out, name)) return false;
    unsigned long long hopMin = 30, hopMax = 30;
    if (item["hop-interval"]) {
        std::string interval;
        if (!proxyString(ctx, item["hop-interval"], name, "hop-interval", interval)) return false;
        if (trimCopy(interval).empty()) interval = "0";
        const auto dash = interval.find('-');
        // Go time.Duration is signed nanoseconds: larger seconds overflow.
        const auto first = proxyUnsigned(std::string_view(interval).substr(0, dash), 9223372036ULL);
        const auto last = dash == std::string::npos ? first :
            proxyUnsigned(std::string_view(interval).substr(dash + 1), 9223372036ULL);
        if (!first || !last)
            return proxyValueInvalid(ctx, name, "hop-interval", "必须为空、非负整数秒或秒数范围，且不能超出内核时长上限");
        const auto lower = std::min(*first, *last), upper = std::max(*first, *last);
        hopMin = lower == 0 ? 30 : std::max(5ULL, lower);
        hopMax = std::max(hopMin, upper);
        if (!out.contains("server_ports")) {
            ctx.note(FidelityScope::Node, Fidelity::Approx, name,
                     std::format("节点「{}」未启用 ports，hop-interval 不生效", name));
        } else if ((lower > 0 && lower < 5) || (dash != std::string::npos && upper < hopMin)) {
            ctx.note(FidelityScope::Node, Fidelity::Approx, name,
                     std::format("节点「{}」的 hop-interval 按 Clash 默认和最小 5 秒限制调整", name),
                     "0 为默认 30 秒；非零间隔小于 5 秒时使用 5 秒，范围上限不低于有效下限");
        }
    }
    if (out.contains("server_ports")) {
        out["hop_interval"] = std::format("{}s", hopMin);
        out["hop_interval_max"] = std::format("{}s", hopMax);
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

bool appendSshOptions(Context& ctx, const YAML::Node& item, nlohmann::json& out,
                      const std::string& name) {
    // Native SSH defaults an empty user to root, whereas Clash keeps it empty.
    std::string user;
    if (!item["username"]) return proxyValueInvalid(ctx, name, "username", "必须显式指定非空用户名");
    if (!proxyString(ctx, item["username"], name, "username", user, true)) return false;
    out["user"] = std::move(user);
    std::string password, key, passphrase;
    for (const auto& [field, value] : {
             std::pair{"password", &password}, std::pair{"private-key", &key},
             std::pair{"private-key-passphrase", &passphrase}}) {
        if (item[field] && !proxyString(ctx, item[field], name, field, *value)) return false;
    }
    if (item["password"]) out["password"] = password;
    if (!key.empty()) {
        // Clash also accepts local paths, with its own safe-path resolver.
        // Do not pass subscription-controlled paths to the native file reader.
        if (key.find("-----BEGIN ") == std::string::npos || key.find("PRIVATE KEY-----") == std::string::npos)
            return proxyValueInvalid(ctx, name, "private-key", "仅支持内联 PEM 私钥，文件路径尚未接入");
        out["private_key"] = std::move(key);
        if (item["private-key-passphrase"]) out["private_key_passphrase"] = std::move(passphrase);
        if (!password.empty()) ctx.note(FidelityScope::Node, Fidelity::Approx, name,
            std::format("SSH 节点「{}」同时配置密码和私钥，原生认证顺序为密码优先，Clash 为私钥优先", name),
            "需要固定认证顺序时只配置一种认证方式；两种凭据均保留，不重写内核认证流程");
    } else if (!passphrase.empty()) {
        ctx.note(FidelityScope::Node, Fidelity::Approx, name,
            std::format("SSH 节点「{}」未启用私钥，private-key-passphrase 不参与认证", name));
    }
    for (const auto& [source, destination] : {
             std::pair{"host-key", "host_key"}, std::pair{"host-key-algorithms", "host_key_algorithms"}}) {
        if (!item[source]) continue;
        std::vector<std::string> values;
        if (!proxyStringList(ctx, item, source, name, values, true)) return false;
        // An empty preference list must not accidentally restore library defaults.
        if (std::string_view(source) == "host-key-algorithms" && values.empty())
            return proxyValueInvalid(ctx, name, source, "显式空算法列表尚未支持");
        if (std::string_view(source) == "host-key-algorithms") {
            // Fixed sing-box go.mod: golang.org/x/crypto v0.54.0,
            // ssh/keys.go and ssh/certs.go. Never filter individual preferences.
            static constexpr std::string_view algorithms[]{
                "ssh-rsa", "ssh-dss", "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384", "ecdsa-sha2-nistp521",
                "sk-ecdsa-sha2-nistp256@openssh.com", "ssh-ed25519", "sk-ssh-ed25519@openssh.com",
                "rsa-sha2-256", "rsa-sha2-512", "ssh-rsa-cert-v01@openssh.com", "ssh-dss-cert-v01@openssh.com",
                "ecdsa-sha2-nistp256-cert-v01@openssh.com", "ecdsa-sha2-nistp384-cert-v01@openssh.com",
                "ecdsa-sha2-nistp521-cert-v01@openssh.com", "sk-ecdsa-sha2-nistp256-cert-v01@openssh.com",
                "ssh-ed25519-cert-v01@openssh.com", "sk-ssh-ed25519-cert-v01@openssh.com",
                "rsa-sha2-256-cert-v01@openssh.com", "rsa-sha2-512-cert-v01@openssh.com"};
            for (std::size_t i = 0; i < values.size(); ++i) {
                if (std::ranges::find(algorithms, values[i]) == std::end(algorithms))
                    return proxyValueInvalid(ctx, name, std::format("{}[{}]", source, i), "不是固定内核支持的主机公钥算法");
            }
        }
        out[destination] = std::move(values);
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
    const bool hopping = (type == "hysteria" || type == "hysteria2" || type == "hy2") &&
                         item["ports"] && item["ports"].IsScalar() && !item["ports"].Scalar().empty();
    const bool unusedPort = hopping && (!item["port"] || (port && *port == 0));
    if (server.empty() || (!unusedPort && (!port || *port <= 0 || *port > 65535))) {
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
    if (port && *port > 0) out["server_port"] = *port;

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
        if (!appendShadowsocksPlugin(ctx, item, out, name) ||
            !appendShadowsocksUot(ctx, item, out, name)) return std::nullopt;
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
        if (!appendProxyUdpEncoding(ctx, item, out, name, false) ||
            !proxyBooleansValid(ctx, item, name, {"global-padding", "authenticated-length"})) return std::nullopt;
        for (const auto& [source, target] : {std::pair{"global-padding", "global_padding"},
                                           std::pair{"authenticated-length", "authenticated_length"}})
            if (item[source]) out[target] = ybool(item, source);
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
        if (!appendProxyUdpEncoding(ctx, item, out, name, true)) return std::nullopt;
        if (item["encryption"]) {
            std::string encryption;
            if (!proxyString(ctx, item["encryption"], name, "encryption", encryption)) return std::nullopt;
            // Fixed sing-box VLESS implements the original unencrypted protocol;
            // these explicit defaults are equivalent to omitting the field.
            if (!encryption.empty() && encryption != "none") {
                proxyValueInvalid(ctx, name, "encryption", "在固定内核中没有对应的加密实现");
                return std::nullopt;
            }
        }
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
    } else if (singboxType == "hysteria") {
        if (!appendHysteria1Options(ctx, item, out, name) ||
            !appendTls(ctx, item, out, true, false)) return std::nullopt;
    } else if (singboxType == "hysteria2") {
        if (item["password"]) out["password"] = yliteral(item, "password");
        if (!appendProxyBandwidth(ctx, item, out, name, false)) return std::nullopt;
        if (!appendHysteria2Options(ctx, item, out, name)) return std::nullopt;
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
    } else if (singboxType == "ssh") {
        if (!appendSshOptions(ctx, item, out, name)) return std::nullopt;
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
        if (singboxType == "http" || singboxType == "ssh") {
            if (ybool(item, "udp")) ctx.note(FidelityScope::Node, Fidelity::Approx, name,
                std::format("{} 节点「{}」启用了 UDP，但原生出站仅转发 TCP", singboxType, name));
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
