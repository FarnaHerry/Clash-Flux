// TLS-only share formats. Format defaults and aliases stay at this boundary;
// QUIC never inherits TCP uTLS, and credentials are decoded exactly once.
template<class Fail>
protocol::PortHopping nodeUriHopping(std::string_view text, const Fail& fail) {
    protocol::PortHopping result{{}, 30, 30};
    const auto number = [&](std::string_view part) -> std::uint16_t {
        unsigned value = 0;
        const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), value);
        if (part.empty() || error != std::errc{} || end != part.data() + part.size() || value == 0 || value > 65535)
            fail("HY2 URI 端口范围成员必须为 1–65535 的十进制整数");
        return static_cast<std::uint16_t>(value);
    };
    for (;;) {
        if (result.ports.size() == 128) fail("HY2 URI 端口集合超过 128 段上限");
        const auto comma = text.find(',');
        const auto part = text.substr(0, comma);
        const auto dash = part.find('-');
        const auto first = number(part.substr(0, dash));
        const auto last = dash == std::string_view::npos ? first : number(part.substr(dash + 1));
        if (first > last) fail("HY2 URI 端口范围必须升序");
        for (const auto& previous : result.ports)
            if (first <= previous.last && previous.first <= last) fail("HY2 URI 端口集合不能重叠或重复");
        result.ports.push_back({first, last});
        if (comma == std::string_view::npos) break;
        text.remove_prefix(comma + 1);
    }
    return result;
}

template<class Fail>
protocol::NodeSpec parseNodeSecureUri(std::string_view remainder, std::string_view scheme, const Fail& fail) {
    const bool hy2 = scheme == "hy2" || scheme == "hysteria2", anytls = scheme == "anytls";
    if (remainder.size() > 64 * 1024 - scheme.size() - 3) fail("URI 超过 64 KiB 上限");
    const auto uri = parseNodeShareUri(remainder, fail, NodeUriSyntax{
        .optionalUserinfo = hy2 || anytls, .defaultPort = hy2 || anytls ? std::optional<std::uint16_t>{443} : std::nullopt,
        .allowPortSet = hy2});
    if (!uri.path.empty() && uri.path != "/") fail("HY2/TUIC/AnyTLS URI 不支持非根路径");
    std::map<std::string, std::string> query;
    for (const auto& [key, value] : uri.query) {
        if (!query.emplace(key, value).second) fail("URI query 含重复参数");
        bool supported = key == "sni" || key == "insecure";
        if (hy2) supported |= key == "obfs" || key == "obfs-password";
        else if (anytls) supported |= key == "fp" || key == "alpn" || key == "security";
        else {
            static constexpr std::string_view fields[]{"alpn", "security", "allow_insecure", "allow-insecure",
                "congestion_control", "congestion-controller", "udp_relay_mode", "udp-relay-mode",
                "zero_rtt_handshake", "zero-rtt-handshake", "reduce_rtt", "reduce-rtt", "disable_sni", "disable-sni"};
            supported |= std::ranges::find(fields, key) != std::end(fields);
        }
        if (!supported) fail("HY2/TUIC/AnyTLS URI 含未支持参数，不能忽略连接约束");
    }
    const auto get = [&](const char* key, std::string fallback = {}) {
        const auto found = query.find(key);
        return found == query.end() ? std::move(fallback) : found->second;
    };
    const auto boolean = [&](std::initializer_list<const char*> keys, bool binaryOnly) -> std::optional<bool> {
        std::optional<bool> value;
        for (const auto* key : keys) {
            if (!query.contains(key)) continue;
            const auto text = get(key);
            if (text != "0" && text != "1" && (binaryOnly || (text != "false" && text != "true")))
                fail("URI 布尔值必须为当前格式支持的 0/1 或 true/false");
            const bool next = text == "1" || text == "true";
            if (value && *value != next) fail("URI 布尔参数别名冲突");
            value = next;
        }
        return value;
    };
    const auto alias = [&](const char* primary, const char* secondary) -> std::optional<std::string> {
        if (query.contains(primary) && query.contains(secondary) && get(primary) != get(secondary)) fail("URI 参数别名冲突");
        if (query.contains(primary)) return get(primary);
        if (query.contains(secondary)) return get(secondary);
        return std::nullopt;
    };
    protocol::NodeSpec node;
    node.origin.name = uri.fragment ? trimCopy(*uri.fragment) : std::string{};
    node.server = uri.host;
    if (!uri.portSet) node.port = uri.port;
    node.tls.declaredEnabled = true; node.tls.enabled = true;
    node.tls.serverName = get("sni", uri.host);
    if (node.tls.serverName->empty()) fail("URI sni 不能为空");
    if (!hy2 && get("security", "tls") != "tls") fail("TUIC/AnyTLS URI 必须启用 TLS");
    if (const auto insecure = hy2 || anytls ? boolean({"insecure"}, true) :
            boolean({"insecure", "allow_insecure", "allow-insecure"}, false)) node.tls.verifyPeer = !*insecure;
    if (!hy2 && query.contains("alpn")) {
        node.tls.alpn = nodeUriList(get("alpn"), fail);
        for (const auto& value : *node.tls.alpn) if (value.size() > 255) fail("URI alpn 协议名超过 255 字节");
    }
    if (hy2) {
        protocol::Hysteria2Options options;
        options.password = decodeNodeUriComponent(uri.userinfo, fail); // Includes userpass's ':' verbatim; may be empty.
        if (uri.portSet) options.hopping = nodeUriHopping(*uri.portSet, fail);
        if (query.contains("obfs")) {
            const auto type = get("obfs"), password = get("obfs-password");
            if (password.empty()) fail("HY2 URI 启用 obfs 必须有非空 obfs-password");
            if (type == "salamander") options.obfs = protocol::SalamanderObfs{password};
            else if (type == "gecko") options.obfs = protocol::GeckoObfs{password, {}, {}};
            else fail("HY2 URI 仅支持 salamander/gecko 混淆");
        } else if (query.contains("obfs-password")) fail("HY2 URI 未启用 obfs，不能应用 obfs-password");
        node.options = std::move(options);
    } else if (anytls) {
        protocol::AnyTlsOptions options;
        options.password = decodeNodeUriComponent(uri.userinfo, fail);
        if (options.password.empty()) fail("AnyTLS URI password 不能为空");
        node.options = std::move(options);
        node.tls.fingerprint = get("fp", "chrome");
        if (!protocol::FingerprintSupported(node.tls.fingerprint)) fail("AnyTLS URI fp 不是固定内核支持的指纹");
    } else {
        const auto colon = uri.userinfo.find(':'); // Split encoded components before decoding either one.
        if (colon == std::string::npos) fail("TUIC v5 URI 必须为 UUID:password，不能按旧 token 解释");
        protocol::TuicOptions options;
        options.uuid = decodeNodeUriComponent(std::string_view(uri.userinfo).substr(0, colon), fail);
        if (!nodeUriUuidValid(options.uuid)) fail("TUIC v5 URI 必须为标准 UUID");
        options.password = decodeNodeUriComponent(std::string_view(uri.userinfo).substr(colon + 1), fail);
        if (const auto congestion = alias("congestion_control", "congestion-controller")) {
            if (*congestion != "cubic" && *congestion != "new_reno" && *congestion != "bbr")
                fail("TUIC URI congestion_control 仅支持 cubic/new_reno/bbr");
            options.congestionControl = *congestion;
        }
        if (const auto mode = alias("udp_relay_mode", "udp-relay-mode")) {
            if (*mode != "native" && *mode != "quic") fail("TUIC URI udp_relay_mode 仅支持 native/quic");
            options.udpRelayMode = *mode == "quic" ? protocol::TuicUdpRelayMode::Quic : protocol::TuicUdpRelayMode::Native;
        }
        options.zeroRttHandshake = boolean({"zero_rtt_handshake", "zero-rtt-handshake", "reduce_rtt", "reduce-rtt"}, false).value_or(false);
        node.tls.disableServerName = boolean({"disable_sni", "disable-sni"}, false).value_or(false);
        node.options = std::move(options);
    }
    return node;
}
