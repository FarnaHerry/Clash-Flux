namespace protocol {

bool ReadString(const YAML::Node& value, std::string_view field, std::string& result,
        Diagnostics& diagnostics, bool nonempty = false) {
    if (!value.IsScalar()) return diagnostics.ValueError(field, "必须为字符串");
    result = value.Scalar();
    if (nonempty && result.empty()) return diagnostics.ValueError(field, "不能为空");
    return true;
}

bool ReadClashTls(const YAML::Node& item, const SourceDefaults& defaults,
        TlsOptions& tls, Diagnostics& diagnostics, bool required = false, bool allowUtls = true) {
    std::string sni, servername;
    if (item["sni"] && !ReadString(item["sni"], "sni", sni, diagnostics)) return false;
    if (item["servername"] && !ReadString(item["servername"], "servername", servername, diagnostics)) return false;
    if (!servername.empty()) {
        if (!sni.empty() && sni != servername)
            diagnostics.Note(Fidelity::Approx, "servername",
                std::format("节点「{}」同时指定不同的 sni/servername，使用 servername", diagnostics.origin.name));
        sni = std::move(servername);
    }
    if (item["sni"] || item["servername"]) tls.serverName = std::move(sni);
    if (item["tls"]) tls.declaredEnabled = ybool(item, "tls");
    if (item["skip-cert-verify"]) tls.verifyPeer = !ybool(item, "skip-cert-verify");
    if (required && tls.declaredEnabled == false)
        return diagnostics.ValueError("tls", "不能关闭该协议必需的 TLS");
    tls.enabled = required || tls.declaredEnabled.value_or(
        (tls.serverName && !tls.serverName->empty()) || !tls.verifyPeer.value_or(true) || bool(item["reality-opts"]));
    if (!tls.enabled) {
        bool valid = true;
        for (const char* field : {"sni", "servername", "skip-cert-verify", "alpn",
                                  "client-fingerprint", "reality-opts"})
            if (item[field]) valid = diagnostics.ValueError(field, "不能在未启用 TLS 时应用") && valid;
        return valid;
    }
    if (const auto alpn = item["alpn"]; alpn) {
        std::vector<std::string> protocols;
        const auto read = [&](const YAML::Node& value, const std::string& field) {
            std::string text;
            if (!ReadString(value, field, text, diagnostics, true)) return false;
            protocols.push_back(std::move(text));
            return true;
        };
        if (alpn.IsScalar()) {
            if (!read(alpn, "alpn")) return false;
        } else if (alpn.IsSequence()) {
            for (std::size_t index = 0; index < alpn.size(); ++index)
                if (!read(alpn[index], std::format("alpn[{}]", index))) return false;
        } else return diagnostics.ValueError("alpn", "必须为字符串或字符串数组");
        if (std::ranges::any_of(protocols, [](const auto& value) { return value.size() > 255; }))
            return diagnostics.ValueError("alpn", "协议名长度必须为 1–255 字节");
        tls.alpn = std::move(protocols);
    }
    if (item["client-fingerprint"] && !allowUtls)
        return diagnostics.ValueError("client-fingerprint", "不适用于该协议的 TLS");
    if (allowUtls) tls.fingerprint = defaults.fingerprint;
    if (item["client-fingerprint"] &&
        !ReadString(item["client-fingerprint"], "client-fingerprint", tls.fingerprint, diagnostics, true)) return false;
    // Validate stages while reading to retain diagnostics before a later malformed field.
    if (allowUtls && !ValidateFingerprint(tls.fingerprint, diagnostics)) return false;
    tls.disableServerName = ybool(item, "disable-sni");
    if (const auto reality = item["reality-opts"]; reality) {
        if (!reality.IsMap()) return diagnostics.ValueError("reality-opts", "必须为对象");
        RealityOptions value;
        if (!ReadString(reality["public-key"], "reality-opts.public-key", value.publicKey, diagnostics, true)) return false;
        if (reality["short-id"]) {
            std::string shortId;
            if (!ReadString(reality["short-id"], "reality-opts.short-id", shortId, diagnostics)) return false;
            value.shortId = std::move(shortId);
        }
        tls.reality = std::move(value);
    }
    return true;
}

bool ReadClashDial(const YAML::Node& item, const TargetCapabilities& target,
        DialOptions& dial, Diagnostics& diagnostics) {
    for (const char* field : {"tfo", "mptcp"}) {
        if (!item[field]) continue;
        const auto value = lowerCopy(ytext(item, field));
        if (value == "true" || value == "false" || value == "yes" || value == "no" ||
            value == "1" || value == "0") continue;
        diagnostics.Note(Fidelity::Unsupported, field,
            std::format("节点「{}」的 {} 不是有效布尔值，已跳过", diagnostics.origin.name, field),
            "使用 true/false，避免无效字段被当成关闭状态");
        return false;
    }
    if (const auto value = item["dialer-proxy"]; value) {
        if (!value.IsScalar() || trimCopy(value.Scalar()).empty())
            return diagnostics.DialError("dialer-proxy", "必须为非空出站名");
        dial.detour = trimCopy(value.Scalar());
    }
    if (const auto value = item["interface-name"]; value) {
        if (!value.IsScalar()) return diagnostics.DialError("interface-name", "必须为接口名字符串");
        dial.interfaceName = trimCopy(value.Scalar());
        if (!ValidateInterface(dial, target, diagnostics)) return false;
    }
    if (const auto value = item["routing-mark"]; value) {
        if (!value.IsScalar()) return diagnostics.DialError("routing-mark", "必须为 uint32 整数");
        const auto text = trimCopy(value.Scalar());
        unsigned long long number = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
        if (text.empty() || error != std::errc{} || end != text.data() + text.size() || number > 4294967295ULL)
            return diagnostics.DialError("routing-mark", "必须在 0–4294967295 之间");
        dial.routingMark = static_cast<std::uint32_t>(number);
        if (!ValidateRoutingMark(dial, target, diagnostics)) return false;
    }
    if (item["tfo"]) dial.tcpFastOpen = ybool(item, "tfo");
    if (item["mptcp"]) dial.multiPathTcp = ybool(item, "mptcp");
    return true;
}

bool ReadBooleanFields(const YAML::Node& item, std::initializer_list<const char*> fields, Diagnostics& diagnostics) {
    for (const char* field : fields) {
        if (!item[field]) continue;
        const auto value = lowerCopy(ytext(item, field));
        if (value == "true" || value == "false" || value == "yes" || value == "no" || value == "1" || value == "0") continue;
        diagnostics.Note(Fidelity::Unsupported, field,
            std::format("节点「{}」的 {} 不是有效布尔值，已跳过", diagnostics.origin.name, field),
            "使用 true/false，避免无效字段被当成关闭状态");
        return false;
    }
    return true;
}

bool ReadStringList(const YAML::Node& item, const char* field, std::vector<std::string>& values, Diagnostics& diagnostics,
        bool nonempty = false) {
    const auto input = item[field];
    if (!input) return true;
    const auto read = [&](const YAML::Node& value, std::string_view name) {
        std::string text;
        if (!ReadString(value, name, text, diagnostics, nonempty)) return false;
        values.push_back(std::move(text));
        return true;
    };
    if (input.IsScalar()) return read(input, field);
    if (!input.IsSequence()) return diagnostics.ValueError(field, "必须为字符串或字符串数组");
    for (std::size_t n = 0; n < input.size(); ++n)
        if (!read(input[n], std::format("{}[{}]", field, n))) return false;
    return true;
}

bool ReadHeaders(const YAML::Node& input, std::string_view field, std::map<std::string, std::string>& values, Diagnostics& diagnostics) {
    if (!input.IsMap()) return diagnostics.ValueError(field, "必须为对象");
    for (const auto& entry : input) {
        std::string key, value;
        if (!ReadString(entry.first, std::format("{} 的键", field), key, diagnostics, true) ||
            !ReadString(entry.second, std::format("{} 的值", field), value, diagnostics)) return false;
        if (!values.emplace(std::move(key), std::move(value)).second)
            return diagnostics.ValueError(field, "不能包含重复键");
    }
    return true;
}

bool ReadFieldsSupported(const YAML::Node& input, std::initializer_list<std::string_view> supported,
        Diagnostics& diagnostics) {
    bool valid = true;
    for (const auto& field : input) {
        const std::string key = field.first.IsScalar() ? field.first.Scalar() : "(无效字段名)";
        if (std::ranges::find(supported, key) != supported.end()) continue;
        diagnostics.Note(Fidelity::Unsupported, key,
            std::format("节点「{}」的 {} 尚未映射，已跳过", diagnostics.origin.name, key),
            "请用原生 sing-box JSON 表达该线路；编译器不会省略未映射字段后改变线路语义");
        valid = false;
    }
    return valid;
}

// Clash structural gates run before protocol fields, preserving failure order.
bool ReadClashFields(const YAML::Node& item, const std::string& type, Diagnostics& diagnostics) {
    const auto& name = diagnostics.origin.name;
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
        diagnostics.Note(Fidelity::Unsupported, key,
            std::format("节点「{}」的 {} 尚未映射，已跳过", name, key),
            "请用原生 sing-box JSON 表达该线路；不会省略证书约束或组合传输后改变线路语义");
        valid = false;
    }
    const auto nested = [&](const char* key, std::initializer_list<std::string_view> supported) {
        const auto value = item[key];
        if (!value) return true;
        if (!value.IsMap()) {
            diagnostics.Note(Fidelity::Unsupported, key,
                std::format("节点「{}」的 {} 必须为对象，已跳过", name, key));
            return false;
        }
        return ReadFieldsSupported(value, supported, diagnostics);
    };
    valid = nested("reality-opts", {"public-key", "short-id"}) && valid;
    valid = nested("ws-opts", {"path", "headers", "max-early-data", "early-data-header-name"}) && valid;
    valid = nested("grpc-opts", {"grpc-service-name"}) && valid;
    valid = nested("h2-opts", {"path", "host"}) && valid;
    valid = nested("http-opts", {"path", "host"}) && valid;
    valid = nested("httpupgrade-opts", {"path", "host"}) && valid;
    if (item["plugin-opts"] && ytext(item, "plugin").empty()) {
        diagnostics.Note(Fidelity::Unsupported, "plugin-opts",
            std::format("节点「{}」包含未启用插件的 plugin-opts，已跳过", name));
        valid = false;
    }
    if ((type == "trojan" || type == "hysteria" || type == "hysteria2" || type == "tuic") &&
        item["tls"] && !ybool(item, "tls")) {
        diagnostics.Note(Fidelity::Unsupported, "tls",
            std::format("节点「{}」的协议不能关闭 TLS，已跳过", name));
        valid = false;
    }
    return valid;
}

bool ReadShadowsocksPlugin(const YAML::Node& item, ShadowsocksOptions& result, Diagnostics& diagnostics) {
    std::string plugin;
    if (item["plugin"] && !ReadString(item["plugin"], "plugin", plugin, diagnostics)) return false;
    if (plugin.empty()) return true;
    if (plugin != "obfs" && plugin != "v2ray-plugin")
        return diagnostics.ValueError("plugin", "尚未映射此插件，不能改成普通 SS 线路");
    const auto options = item["plugin-opts"];
    if (!options || !options.IsMap())
        return diagnostics.ValueError("plugin-opts", "插件需要选项对象和显式 mode");
    std::set<std::string> keys;
    for (const auto& field : options) {
        if (!field.first.IsScalar()) return diagnostics.ValueError("plugin-opts", "键必须为字符串");
        if (!keys.insert(field.first.Scalar()).second)
            return diagnostics.ValueError("plugin-opts", "不能包含重复键");
    }
    if (plugin == "obfs") {
        if (!ReadFieldsSupported(options, {"mode", "host"}, diagnostics)) return false;
    } else {
        if (!ReadFieldsSupported(options, {"mode", "host", "path", "tls", "mux", "headers", "skip-cert-verify",
                "v2ray-http-upgrade", "v2ray-http-upgrade-fast-open"}, diagnostics) ||
            !ReadBooleanFields(options, {"tls", "mux", "skip-cert-verify", "v2ray-http-upgrade",
                "v2ray-http-upgrade-fast-open"}, diagnostics)) return false;
        for (const char* field : {"skip-cert-verify", "v2ray-http-upgrade", "v2ray-http-upgrade-fast-open"})
            if (ybool(options, field)) return diagnostics.ValueError(std::format("plugin-opts.{}", field),
                "固定内核的 SIP003 插件没有等价选项");
        if (options["headers"]) {
            std::map<std::string, std::string> headers;
            if (!ReadHeaders(options["headers"], "plugin-opts.headers", headers, diagnostics)) return false;
            if (!headers.empty()) return diagnostics.ValueError("plugin-opts.headers",
                "固定内核的 SIP003 插件不能传入自定义 headers");
        }
    }
    std::string mode, host = "bing.com";
    if (!options["mode"]) return diagnostics.ValueError("plugin-opts.mode", "必须显式指定");
    if (!ReadString(options["mode"], "plugin-opts.mode", mode, diagnostics, true)) return false;
    if (options["host"] && !ReadString(options["host"], "plugin-opts.host", host, diagnostics, true)) return false;
    if (plugin == "obfs") {
        if (mode != "http" && mode != "tls")
            return diagnostics.ValueError("plugin-opts.mode", "simple-obfs 仅支持 http/tls");
        result.plugin = SimpleObfsPlugin{mode == "http" ? SimpleObfsMode::Http : SimpleObfsMode::Tls, std::move(host)};
    } else {
        if (mode != "websocket")
            return diagnostics.ValueError("plugin-opts.mode", "Clash v2ray-plugin 仅支持 websocket，不能改写模式");
        std::string path = "/";
        if (options["path"] && !ReadString(options["path"], "plugin-opts.path", path, diagnostics)) return false;
        result.plugin = V2RayWebSocketPlugin{std::move(host), std::move(path), ybool(options, "tls"),
            options["mux"] ? ybool(options, "mux") : true};
    }
    return true;
}

bool ReadShadowsocksUot(const YAML::Node& item, ShadowsocksOptions& result, Diagnostics& diagnostics) {
    if (!item["udp-over-tcp"] && !item["udp-over-tcp-version"]) return true;
    UdpOverTcpOptions uot;
    uot.version = 1; // Clash defaults to legacy UOT; native bool defaults to v2.
    if (item["udp-over-tcp-version"]) {
        const auto version = yint(item, "udp-over-tcp-version");
        if (!version || *version < 0 || *version > 2)
            return diagnostics.ValueError("udp-over-tcp-version", "仅支持 0（Clash 默认 v1）、1 或 2");
        if (*version != 0) uot.version = *version;
    }
    uot.enabled = ybool(item, "udp-over-tcp");
    result.udpOverTcp = uot;
    return true;
}

bool ReadClashTransport(const YAML::Node& item, TransportOptions& result, Diagnostics& diagnostics) {
    const auto network = lowerCopy(ytext(item, "network"));
    if (network.empty() || network == "tcp") return true;
    if (network == "ws") {
        WebSocketTransport transport;
        if (item["ws-path"]) {
            std::string path;
            if (!ReadString(item["ws-path"], "ws-path", path, diagnostics)) return false;
            transport.path = std::move(path);
        }
        if (item["ws-headers"]) {
            std::map<std::string, std::string> headers;
            if (!ReadHeaders(item["ws-headers"], "ws-headers", headers, diagnostics)) return false;
            transport.headers = std::move(headers);
        }
        const auto noteOverride = [&](const char* field) {
            diagnostics.Note(Fidelity::Approx, std::string("ws-opts.") + field,
                std::format("节点「{}」的 ws-opts.{} 覆盖不同的旧版 ws-{}", diagnostics.origin.name, field, field),
                "统一新旧 WebSocket 字段；相同的重复值不会导致节点降级");
        };
        if (const auto opts = item["ws-opts"]; opts && opts.IsMap()) {
            if (opts["path"]) {
                std::string path;
                if (!ReadString(opts["path"], "ws-opts.path", path, diagnostics)) return false;
                if (transport.path && *transport.path != path) noteOverride("path");
                transport.path = std::move(path);
            }
            if (opts["headers"]) {
                std::map<std::string, std::string> headers;
                if (!ReadHeaders(opts["headers"], "ws-opts.headers", headers, diagnostics)) return false;
                if (transport.headers && *transport.headers != headers) noteOverride("headers");
                transport.headers = std::move(headers);
            }
            if (opts["max-early-data"]) {
                const auto early = yint(opts, "max-early-data");
                if (!early || *early < 0) return diagnostics.ValueError("ws-opts.max-early-data", "必须为非负整数");
                transport.maxEarlyData = *early;
            }
            if (opts["early-data-header-name"]) {
                std::string header;
                if (!ReadString(opts["early-data-header-name"], "ws-opts.early-data-header-name", header, diagnostics)) return false;
                transport.earlyDataHeader = std::move(header);
            }
        }
        result = std::move(transport);
        return true;
    }
    if (network == "grpc") {
        std::string service;
        if (item["grpc-service-name"] && !ReadString(item["grpc-service-name"], "grpc-service-name", service, diagnostics)) return false;
        if (const auto opts = item["grpc-opts"]; opts && opts.IsMap() && opts["grpc-service-name"]) {
            std::string nested;
            if (!ReadString(opts["grpc-service-name"], "grpc-opts.grpc-service-name", nested, diagnostics)) return false;
            if (!service.empty() && service != nested)
                diagnostics.Note(Fidelity::Approx, "grpc-service-name",
                    std::format("节点「{}」的嵌套 grpc-service-name 覆盖顶层值", diagnostics.origin.name));
            service = std::move(nested);
        }
        GrpcTransport transport;
        if (!service.empty()) transport.serviceName = std::move(service);
        result = std::move(transport);
        return true;
    }
    if (network == "h2" || network == "http") {
        HttpTransport transport;
        const auto opts = network == "h2" ? item["h2-opts"] : item["http-opts"];
        if (opts && opts.IsMap()) {
            std::vector<std::string> paths, hosts;
            if (!ReadStringList(opts, "path", paths, diagnostics) || !ReadStringList(opts, "host", hosts, diagnostics)) return false;
            if (!paths.empty()) {
                transport.path = paths.front();
                if (paths.size() > 1)
                    diagnostics.Note(Fidelity::Approx, "path",
                        std::format("节点「{}」的 HTTP 传输 path 列表仅使用首项", diagnostics.origin.name),
                        "sing-box 客户端只接受一个请求路径；请明确指定要使用的路径");
            }
            if (opts["host"]) transport.hosts = std::move(hosts);
        }
        result = std::move(transport);
        return true;
    }
    if (network == "httpupgrade") {
        HttpUpgradeTransport transport;
        if (const auto opts = item["httpupgrade-opts"]; opts && opts.IsMap()) {
            if (opts["path"]) {
                std::string path;
                if (!ReadString(opts["path"], "httpupgrade-opts.path", path, diagnostics)) return false;
                transport.path = std::move(path);
            }
            if (opts["host"]) {
                std::string host;
                if (!ReadString(opts["host"], "httpupgrade-opts.host", host, diagnostics)) return false;
                transport.host = std::move(host);
            }
        }
        result = std::move(transport);
        return true;
    }
    diagnostics.Note(Fidelity::Unsupported, "network",
        std::format("节点「{}」的传输层 network:{} 暂不支持，已跳过", diagnostics.origin.name, network),
        "把该节点的 network 换成 sing-box 支持的传输层（tcp/ws/grpc/http）");
    return false;
}

std::optional<unsigned long long> ReadUnsigned(std::string_view value, unsigned long long max) {
    const std::string text = trimCopy(value);
    unsigned long long number = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size() || number > max)
        return std::nullopt;
    return number;
}

// Input bandwidth units are case-sensitive: MBps is bytes, Mbps is bits.
// Keep an integer Mbps subset and reject units/values we cannot express exactly.
std::optional<int> ReadMbps(std::string_view raw) {
    const std::string text = trimCopy(raw);
    std::size_t end = 0;
    while (end < text.size() && text[end] >= '0' && text[end] <= '9') ++end;
    const std::string unit = trimCopy(std::string_view(text).substr(end));
    if (unit != "" && unit != "Mbps" && unit != "MBps") return std::nullopt;
    const unsigned long long multiplier = unit == "MBps" ? 8 : 1;
    const auto number = ReadUnsigned(std::string_view(text).substr(0, end),
                                      static_cast<unsigned long long>(std::numeric_limits<int>::max()) / multiplier);
    if (!number || *number == 0) return std::nullopt;
    return static_cast<int>(*number * multiplier);
}

bool ReadBandwidth(const YAML::Node& item, BandwidthOptions& options, bool required, Diagnostics& diagnostics) {
    for (const auto& [source, target] : {std::pair{"up", &options.upMbps}, {"down", &options.downMbps}}) {
        if (!required && !item[source]) continue;
        const auto value = item[source] && item[source].IsScalar() ? ReadMbps(item[source].Scalar()) : std::nullopt;
        if (!value) return diagnostics.ValueError(source,
            "必须为正整数 Mbps（裸整数或 Mbps 单位），MBps 按字节转换；其它单位/小数尚未接入或数值超限");
        *target = *value;
    }
    return true;
}

bool ReadHysteriaPorts(const YAML::Node& item, std::vector<PortRange>& ports, Diagnostics& diagnostics) {
    std::string text;
    if (item["ports"] && !ReadString(item["ports"], "ports", text, diagnostics)) return false;
    if (text.empty()) return true;
    std::string_view rest{text};
    while (true) {
        const auto comma = rest.find(',');
        const std::string piece = trimCopy(rest.substr(0, comma));
        const auto dash = piece.find('-');
        const auto first = ReadUnsigned(std::string_view(piece).substr(0, dash), 65535);
        const auto last = dash == std::string::npos ? first : ReadUnsigned(std::string_view(piece).substr(dash + 1), 65535);
        if (!first || !last || *first == 0 || *last == 0)
            return diagnostics.ValueError("ports", "每项必须为 1–65535 端口或端口范围，不能有空项");
        ports.push_back({static_cast<std::uint16_t>(std::min(*first, *last)), static_cast<std::uint16_t>(std::max(*first, *last))});
        if (comma == std::string_view::npos) break;
        rest.remove_prefix(comma + 1);
    }
    return true;
}

bool ReadHysteria1(const YAML::Node& item, Hysteria1Options& options, Diagnostics& diagnostics) {
    const auto& name = diagnostics.origin.name;
    if (!ReadBandwidth(item, options.bandwidth, true, diagnostics) ||
        !ReadBooleanFields(item, {"fast-open", "disable-mtu-discovery"}, diagnostics)) return false;
    std::string protocol, obfsProtocol, auth, authString, obfs;
    for (const auto& [field, value] : {std::pair{"protocol", &protocol}, std::pair{"obfs-protocol", &obfsProtocol},
            std::pair{"auth", &auth}, std::pair{"auth-str", &authString}, std::pair{"obfs", &obfs}}) {
        if (item[field] && !ReadString(item[field], field, *value, diagnostics)) return false;
    }
    // Mihomo's Stash-compatible obfs-protocol overrides protocol when nonempty.
    if (!obfsProtocol.empty()) protocol = obfsProtocol;
    if (!protocol.empty() && protocol != "udp")
        return diagnostics.ValueError("protocol/obfs-protocol", "固定内核仅支持 UDP 传输，不能改成普通 UDP 线路");
    if (ybool(item, "fast-open"))
        return diagnostics.ValueError("fast-open", "固定内核没有等价的协议快速打开选项");
    if (!auth.empty()) {
        const auto size = HysteriaBase64Size(auth);
        if (!size || *size > 65535) return diagnostics.ValueError("auth", "必须为标准 Base64，解码后不能超过 65535 字节");
        options.auth = HysteriaBytesAuth{std::move(auth)}; // Auth bytes override auth-str in Clash.
    } else {
        if (authString.size() > 65535) return diagnostics.ValueError("auth-str", "不能超过 65535 字节");
        if (item["auth-str"]) options.auth = HysteriaTextAuth{std::move(authString)};
    }
    if (item["obfs"]) options.obfs = std::move(obfs);
    if (item["disable-mtu-discovery"]) options.disableMtuDiscovery = ybool(item, "disable-mtu-discovery");
    unsigned long long streamWindow = 0, connectionWindow = 0;
    // Follow the actual Mihomo axes, not the misleading legacy names: its
    // recv-window-conn sets the stream window, recv-window the connection window.
    for (const auto& [field, value] : {std::pair{"recv-window-conn", &streamWindow}, std::pair{"recv-window", &connectionWindow}}) {
        if (!item[field]) continue;
        const auto parsed = item[field].IsScalar() ? ReadUnsigned(item[field].Scalar(), 2147483647) : std::nullopt;
        if (!parsed) return diagnostics.ValueError(field, "必须为 0–2147483647 的字节数");
        *value = *parsed;
    }
    if ((streamWindow == 0) != (connectionWindow == 0))
        return diagnostics.ValueError("recv-window/recv-window-conn", "本批仅支持两者同时非零或同时使用默认值，单边窗口尚未保真");
    if (streamWindow == 0) {
        streamWindow = 15728640;
        connectionWindow = 67108864;
        diagnostics.Note(Fidelity::Approx, "recv-window/recv-window-conn",
                 std::format("Hysteria 节点「{}」保留 Clash 默认窗口上限，原生 QUIC 初始窗口与 Clash 的渐增分配不同", name),
                 "固定内核窗口字段不能分别设置初始值与上限；需要完全一致时请显式设置两侧非零窗口");
    }
    options.streamReceiveWindow = static_cast<std::uint32_t>(streamWindow);
    options.connectionReceiveWindow = static_cast<std::uint32_t>(connectionWindow);
    if (item["alpn"] && item["alpn"].IsSequence() && item["alpn"].size() == 0)
        return diagnostics.ValueError("alpn", "原生 Hysteria 将空列表恢复为协议默认，不能保留显式空 ALPN");
    std::vector<PortRange> ports;
    if (!ReadHysteriaPorts(item, ports, diagnostics)) return false;
    unsigned long long interval = 10; // HY1 Clash default, native default is 30s.
    if (item["hop-interval"]) {
        const auto parsed = item["hop-interval"].IsScalar() ? ReadUnsigned(item["hop-interval"].Scalar(), 9223372036ULL) : std::nullopt;
        if (!parsed) return diagnostics.ValueError("hop-interval", "必须为非负整数秒，不能超出内核时长上限");
        if (*parsed != 0) interval = *parsed;
        if (ports.empty()) diagnostics.Note(Fidelity::Approx, "hop-interval",
            std::format("节点「{}」未启用 ports，hop-interval 不生效", name));
    }
    if (!ports.empty()) {
        if (interval < 5) return diagnostics.ValueError("hop-interval", "固定内核最小为 5 秒，不能改变显式跳端口间隔");
        options.hopping = PortHopping{std::move(ports), interval, interval};
    }
    return true;
}

bool ReadHysteria2(const YAML::Node& item, Hysteria2Options& options, Diagnostics& diagnostics) {
    const auto& name = diagnostics.origin.name;
    std::string obfs;
    if (item["obfs"] && !ReadString(item["obfs"], "obfs", obfs, diagnostics)) return false;
    const std::string password = yliteral(item, "obfs-password");
    if (!obfs.empty()) {
        if (obfs != "salamander" && obfs != "gecko")
            return diagnostics.ValueError("obfs", "仅支持 salamander 或 gecko");
        if (password.empty()) return diagnostics.ValueError("obfs-password", "启用混淆时不能为空");
        if (obfs == "gecko") options.obfs = GeckoObfs{password, {}, {}};
        else options.obfs = SalamanderObfs{password};
    } else if (!password.empty()) {
        diagnostics.Note(Fidelity::Approx, "obfs-password",
                 std::format("节点「{}」未启用 obfs，obfs-password 不生效", name),
                 "与 Clash 行为一致；启用混淆需要显式指定 obfs 类型");
    }
    unsigned long long minSize = 0, maxSize = 0;
    for (const auto& [source, target, size] : {
             std::tuple{"obfs-min-packet-size", "min_packet_size", &minSize},
             std::tuple{"obfs-max-packet-size", "max_packet_size", &maxSize}}) {
        if (!item[source]) continue;
        const auto value = item[source].IsScalar() ? ReadUnsigned(item[source].Scalar(), 2048) : std::nullopt;
        if (!value) return diagnostics.ValueError(source, "必须为 0–2048 的整数");
        *size = *value;
        if (obfs == "gecko") {
            auto& gecko = std::get<GeckoObfs>(options.obfs);
            auto& sizeOption = std::string_view(target) == "min_packet_size" ? gecko.minimumPacketSize : gecko.maximumPacketSize;
            sizeOption = static_cast<std::uint16_t>(*value);
        }
        else diagnostics.Note(Fidelity::Approx, source,
                      std::format("节点「{}」未启用 gecko，{} 不生效", name, source));
    }
    // Fixed sing-quic gecko defaults: 512/1200, on-wire maximum 2048.
    if (obfs == "gecko" && (minSize == 0 ? 512 : minSize) > (maxSize == 0 ? 1200 : maxSize))
        return diagnostics.ValueError("obfs-min/max-packet-size", "有效最小值不能大于最大值");

    std::vector<PortRange> ports;
    if (!ReadHysteriaPorts(item, ports, diagnostics)) return false;
    unsigned long long hopMin = 30, hopMax = 30;
    if (item["hop-interval"]) {
        std::string interval;
        if (!ReadString(item["hop-interval"], "hop-interval", interval, diagnostics)) return false;
        if (trimCopy(interval).empty()) interval = "0";
        const auto dash = interval.find('-');
        // Go time.Duration is signed nanoseconds: larger seconds overflow.
        const auto first = ReadUnsigned(std::string_view(interval).substr(0, dash), 9223372036ULL);
        const auto last = dash == std::string::npos ? first :
            ReadUnsigned(std::string_view(interval).substr(dash + 1), 9223372036ULL);
        if (!first || !last)
            return diagnostics.ValueError("hop-interval", "必须为空、非负整数秒或秒数范围，且不能超出内核时长上限");
        const auto lower = std::min(*first, *last), upper = std::max(*first, *last);
        hopMin = lower == 0 ? 30 : std::max(5ULL, lower);
        hopMax = std::max(hopMin, upper);
        if (ports.empty()) {
            diagnostics.Note(Fidelity::Approx, "hop-interval",
                     std::format("节点「{}」未启用 ports，hop-interval 不生效", name));
        } else if ((lower > 0 && lower < 5) || (dash != std::string::npos && upper < hopMin)) {
            diagnostics.Note(Fidelity::Approx, "hop-interval",
                     std::format("节点「{}」的 hop-interval 按 Clash 默认和最小 5 秒限制调整", name),
                     "0 为默认 30 秒；非零间隔小于 5 秒时使用 5 秒，范围上限不低于有效下限");
        }
    }
    if (!ports.empty()) {
        options.hopping = PortHopping{std::move(ports), hopMin, hopMax};
    }
    return true;
}

bool ReadAnyTls(const YAML::Node& item, AnyTlsOptions& options, Diagnostics& diagnostics) {
    const auto& name = diagnostics.origin.name;
    if (!ReadFieldsSupported(item,
            {"name", "type", "server", "port", "password", "udp", "tls", "sni", "servername", "skip-cert-verify",
             "client-fingerprint", "alpn", "idle-session-check-interval", "idle-session-timeout", "min-idle-session",
             "client-metadata", "dialer-proxy", "interface-name", "routing-mark", "tfo", "mptcp"}, diagnostics) ||
        !ReadBooleanFields(item, {"udp", "tls", "skip-cert-verify"}, diagnostics)) return false;
    const auto credential = item["password"];
    options.password = credential && credential.IsScalar() ? credential.Scalar() : "";
    if (options.password.empty()) {
        diagnostics.Note(Fidelity::Unsupported, "password", std::format("AnyTLS 节点「{}」缺少 password，已跳过", name),
            "补上 AnyTLS 密码后重新导入");
        return false;
    }
    for (const auto& [source, destination] : {
             std::pair{"idle-session-check-interval", &options.idleSessionCheckSeconds},
             std::pair{"idle-session-timeout", &options.idleSessionTimeoutSeconds},
             std::pair{"min-idle-session", &options.minimumIdleSessions}}) {
        if (!item[source]) continue;
        const auto value = yint(item, source);
        if (!value || *value < 0) {
            diagnostics.Note(Fidelity::Unsupported, source,
                std::format("AnyTLS 节点「{}」的 {} 必须为非负整数，已跳过", name, source),
                "会话时间使用秒数，最少空闲会话使用数量");
            return false;
        }
        *destination = *value;
    }
    if (const auto metadata = item["client-metadata"]; metadata) {
        if (!metadata.IsScalar()) {
            diagnostics.Note(Fidelity::Unsupported, "client-metadata",
                std::format("AnyTLS 节点「{}」的 client-metadata 必须为字符串，已跳过", name),
                "client-metadata 可为空字符串，但不能为对象或列表");
            return false;
        }
        options.clientMetadata = metadata.Scalar();
    }
    return true;
}

bool ReadSnell4(const YAML::Node& item, Snell4Options& options, Diagnostics& diagnostics) {
    const auto& name = diagnostics.origin.name;
    if (!ReadFieldsSupported(item, {"name", "type", "server", "port", "version", "psk", "udp", "reuse", "obfs-opts",
            "dialer-proxy", "interface-name", "routing-mark", "tfo", "mptcp"}, diagnostics) ||
        !ReadBooleanFields(item, {"udp", "reuse"}, diagnostics)) return false;
    const auto version = yint(item, "version");
    const auto credential = item["psk"];
    options.psk = credential && credential.IsScalar() ? credential.Scalar() : "";
    if (!version || *version != 4 || options.psk.empty()) {
        diagnostics.Note(Fidelity::Unsupported, "version/psk",
            std::format("Snell 节点「{}」需要显式 version: 4 和非空 psk，已跳过", name),
            "sing-box 1.14 不支持 Clash 的 Snell v1/v2/v3/v5；版本不能自动替换");
        return false;
    }
    options.reuse = ybool(item, "reuse");
    if (const auto obfs = item["obfs-opts"]; obfs) {
        // Keep the existing Snell scalar fallback in this format adapter.
        const auto mode = obfs.IsMap() ? lowerCopy(ytext(obfs, "mode")) : "(无效)";
        if (!mode.empty() && mode != "http" && mode != "tls") {
            diagnostics.Note(Fidelity::Unsupported, "obfs-opts.mode",
                std::format("Snell 节点「{}」的 obfs 模式 {} 尚未映射，已跳过", name, mode),
                "仅支持 http/tls 伪装；shadow-tls/restls/jls 不会降成无伪装连接");
            return false;
        }
        if (!ReadFieldsSupported(obfs, {"mode", "host"}, diagnostics)) return false;
        if (!mode.empty()) {
            auto host = ytext(obfs, "host");
            if (host.empty()) host = "bing.com";
            options.obfs = SnellObfsOptions{mode == "http" ? SnellObfsOptions::Mode::Http : SnellObfsOptions::Mode::Tls,
                std::move(host)};
        }
    }
    return true;
}

bool ReadSsh(const YAML::Node& item, SshOptions& options, Diagnostics& diagnostics) {
    const auto& name = diagnostics.origin.name;
    // Native SSH defaults an empty user to root, whereas Clash keeps it empty.
    std::string user;
    if (!item["username"]) return diagnostics.ValueError("username", "必须显式指定非空用户名");
    if (!ReadString(item["username"], "username", user, diagnostics, true)) return false;
    options.user = std::move(user);
    std::string password, key, passphrase;
    for (const auto& [field, value] : {
             std::pair{"password", &password}, std::pair{"private-key", &key},
             std::pair{"private-key-passphrase", &passphrase}}) {
        if (item[field] && !ReadString(item[field], field, *value, diagnostics)) return false;
    }
    if (item["password"]) options.password = password;
    if (!key.empty()) {
        // Clash also accepts local paths, with its own safe-path resolver.
        // Do not pass subscription-controlled paths to the native file reader.
        if (key.find("-----BEGIN ") == std::string::npos || key.find("PRIVATE KEY-----") == std::string::npos)
            return diagnostics.ValueError("private-key", "仅支持内联 PEM 私钥，文件路径尚未接入");
        options.privateKey = SshPrivateKey{std::move(key), {}};
        if (item["private-key-passphrase"]) options.privateKey->passphrase = std::move(passphrase);
        if (!password.empty()) diagnostics.Note(Fidelity::Approx, "password/private-key",
            std::format("SSH 节点「{}」同时配置密码和私钥，原生认证顺序为密码优先，Clash 为私钥优先", name),
            "需要固定认证顺序时只配置一种认证方式；两种凭据均保留，不重写内核认证流程");
    } else if (!passphrase.empty()) {
        diagnostics.Note(Fidelity::Approx, "private-key-passphrase",
            std::format("SSH 节点「{}」未启用私钥，private-key-passphrase 不参与认证", name));
    }
    for (const auto& [source, destination] : {
             std::pair{"host-key", "host_key"}, std::pair{"host-key-algorithms", "host_key_algorithms"}}) {
        if (!item[source]) continue;
        std::vector<std::string> values;
        if (!ReadStringList(item, source, values, diagnostics, true)) return false;
        // An empty preference list must not accidentally restore library defaults.
        if (std::string_view(source) == "host-key-algorithms" && values.empty())
            return diagnostics.ValueError(source, "显式空算法列表尚未支持");
        if (std::string_view(source) == "host-key-algorithms") {
            for (std::size_t i = 0; i < values.size(); ++i) {
                if (!SshHostAlgorithmSupported(values[i]))
                    return diagnostics.ValueError(std::format("{}[{}]", source, i), "不是固定内核支持的主机公钥算法");
            }
        }
        if (std::string_view(destination) == "host_key") options.hostKeys = std::move(values);
        else options.hostKeyAlgorithms = std::move(values);
    }
    return true;
}

bool ReadUdpEncoding(const YAML::Node& item, bool vless, std::optional<std::string>& result, Diagnostics& diagnostics) {
    if (!ReadBooleanFields(item, {"packet-addr", "xudp"}, diagnostics)) return false;
    if (!item["packet-encoding"] && !item["packet-addr"] && !item["xudp"]) return true;
    std::string encoding;
    if (item["packet-encoding"] && !ReadString(item["packet-encoding"], "packet-encoding", encoding, diagnostics)) return false;
    if (encoding != "" && encoding != "packetaddr" && encoding != "packet" && encoding != "xudp")
        return diagnostics.ValueError("packet-encoding", "仅支持空字符串、packetaddr/packet 或 xudp");
    bool packet = ybool(item, "packet-addr"), xudp = ybool(item, "xudp");
    if (encoding == "packetaddr" || encoding == "packet") { packet = true; if (vless) xudp = false; }
    else if (vless) { if (!packet) xudp = true; }
    else if (encoding == "xudp") xudp = true;
    if (xudp) packet = false;
    result = xudp ? "xudp" : packet ? "packetaddr" : "";
    return true;
}

std::optional<NodeSpec> ParseClashProxy(const YAML::Node& item, NodeOrigin origin,
        const SourceDefaults& defaults, const TargetCapabilities& target, Diagnostics& diagnostics) {
    NodeSpec node;
    node.origin = std::move(origin);
    const auto& name = node.origin.name;
    const auto type = lowerCopy(ytext(item, "type"));
    node.server = ytext(item, "server");
    const auto port = yint(item, "port");
    const bool hopping = (type == "hysteria" || type == "hysteria2" || type == "hy2") &&
        item["ports"] && item["ports"].IsScalar() && !item["ports"].Scalar().empty();
    const bool unusedPort = hopping && (!item["port"] || (port && *port == 0));
    if (node.server.empty() || (!unusedPort && (!port || *port <= 0 || *port > 65535))) {
        diagnostics.Note(Fidelity::Unsupported, "server/port",
            std::format("节点「{}」缺少有效的 server/port，已跳过", name),
            "补上 server/port 之后该节点才会生效");
        return std::nullopt;
    }
    if (port && *port > 0) node.port = static_cast<std::uint16_t>(*port);
    const auto canonicalType = type == "ss" ? "shadowsocks" : type == "hy2" ? "hysteria2" : type;
    if (!ReadClashFields(item, canonicalType, diagnostics)) return std::nullopt;
    if (canonicalType != "anytls" && canonicalType != "snell" &&
        !ReadBooleanFields(item, {"udp", "tls", "skip-cert-verify", "udp-over-tcp", "reduce-rtt", "disable-sni"}, diagnostics))
        return std::nullopt;
    for (const char* field : {"password", "username", "psk", "obfs-password"}) {
        if (item[field] && !item[field].IsScalar()) {
            diagnostics.ValueError(field, "必须为凭据字符串");
            return std::nullopt;
        }
    }
    const bool shadowsocks = type == "ss" || type == "shadowsocks";
    if (type == "http" || type == "socks5") {
        PasswordAuth auth;
        if (item["username"]) auth.username = yliteral(item, "username");
        if (item["password"]) auth.password = yliteral(item, "password");
        if (type == "http") node.options = HttpOptions{std::move(auth)};
        else node.options = Socks5Options{std::move(auth)};
    } else if (type == "vmess") {
        VmessOptions options;
        if (!ReadUdpEncoding(item, false, options.packetEncoding, diagnostics) ||
            !ReadBooleanFields(item, {"global-padding", "authenticated-length"}, diagnostics)) return std::nullopt;
        options.uuid = ytext(item, "uuid");
        options.security = lowerCopy(ytext(item, "cipher"));
        if (options.security.empty()) options.security = "auto";
        options.alterId = yint(item, "alterId");
        if (item["global-padding"]) options.globalPadding = ybool(item, "global-padding");
        if (item["authenticated-length"]) options.authenticatedLength = ybool(item, "authenticated-length");
        node.options = std::move(options);
    } else if (type == "vless") {
        VlessOptions options;
        if (!ReadUdpEncoding(item, true, options.packetEncoding, diagnostics)) return std::nullopt;
        if (item["encryption"]) {
            std::string value;
            if (!ReadString(item["encryption"], "encryption", value, diagnostics)) return std::nullopt;
            if (!value.empty() && value != "none") {
                diagnostics.ValueError("encryption", "在固定内核中没有对应的加密实现");
                return std::nullopt;
            }
        }
        options.uuid = ytext(item, "uuid");
        const auto flow = ytext(item, "flow");
        if (!flow.empty()) options.flow = flow;
        node.options = std::move(options);
    } else if (type == "trojan") {
        TrojanOptions options;
        if (item["password"]) options.password = yliteral(item, "password");
        node.options = std::move(options);
    } else if (shadowsocks) {
        ShadowsocksOptions options;
        options.method = ytext(item, "cipher");
        if (item["password"]) options.password = yliteral(item, "password");
        if (!ReadShadowsocksPlugin(item, options, diagnostics) ||
            !ReadShadowsocksUot(item, options, diagnostics)) return std::nullopt;
        node.options = std::move(options);
    } else if (type == "hysteria") {
        Hysteria1Options options;
        if (!ReadHysteria1(item, options, diagnostics)) return std::nullopt;
        node.options = std::move(options);
    } else if (type == "hysteria2" || type == "hy2") {
        Hysteria2Options options;
        if (item["password"]) options.password = yliteral(item, "password");
        if (!ReadBandwidth(item, options.bandwidth, false, diagnostics) ||
            !ReadHysteria2(item, options, diagnostics)) return std::nullopt;
        node.options = std::move(options);
    } else if (type == "tuic") {
        TuicOptions options;
        options.uuid = ytext(item, "uuid");
        if (item["password"]) options.password = yliteral(item, "password");
        const auto congestion = ytext(item, "congestion-controller");
        if (!congestion.empty()) options.congestionControl = congestion;
        options.zeroRttHandshake = ybool(item, "reduce-rtt");
        node.options = std::move(options);
    } else if (type == "anytls") {
        AnyTlsOptions options;
        if (!ReadAnyTls(item, options, diagnostics)) return std::nullopt;
        node.options = std::move(options);
    } else if (type == "snell") {
        Snell4Options options;
        if (!ReadSnell4(item, options, diagnostics)) return std::nullopt;
        node.options = std::move(options);
    } else if (type == "ssh") {
        SshOptions options;
        if (!ReadSsh(item, options, diagnostics)) return std::nullopt;
        node.options = std::move(options);
    } else {
        diagnostics.Note(Fidelity::Unsupported, "type",
            std::format("节点「{}」的协议 {} 暂不支持，已跳过", name, type),
            "该节点已从所有组中移除；可在订阅里删除它，或等编译器补上该协议");
        return std::nullopt;
    }
    if ((type == "vmess" || type == "vless" || type == "trojan") &&
        !ReadClashTransport(item, node.transport, diagnostics)) return std::nullopt;
    if (item["udp"] || type == "anytls" || type == "snell") node.udp = ybool(item, "udp");
    const bool ownsTls = !shadowsocks && type != "ssh" && type != "snell";
    if (ownsTls && !ReadClashTls(item, defaults, node.tls, diagnostics,
            type == "trojan" || type == "anytls" || UsesQuicTls(node), !UsesQuicTls(node))) return std::nullopt;
    if (type == "anytls") NoteAnyTlsUdp(node, diagnostics);
    if (!ReadClashDial(item, target, node.dial, diagnostics)) return std::nullopt;
    return node;
}

} // namespace protocol
