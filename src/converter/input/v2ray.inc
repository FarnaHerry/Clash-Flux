// Xray-style VLESS / compatible Trojan query subset. Values are already decoded
// by the shared URI reader; never construct Clash nodes or decode them again.
template<class Fail>
std::vector<std::string> nodeUriList(std::string_view text, const Fail& fail) {
    std::vector<std::string> values;
    for (;;) {
        const auto comma = text.find(',');
        const auto part = text.substr(0, comma);
        if (part.empty() || part != trimCopy(part)) fail("URI 列表不能含空成员或两侧空白");
        values.emplace_back(part);
        if (comma == std::string_view::npos) break;
        text.remove_prefix(comma + 1);
    }
    return values;
}

template<class Fail>
protocol::NodeSpec parseNodeV2RayUri(std::string_view remainder, bool vless, const Fail& fail) {
    if (remainder.size() > 64 * 1024 - (vless ? 8 : 9)) fail("URI 超过 64 KiB 上限");
    const auto uri = parseNodeShareUri(remainder, fail);
    if (!uri.path.empty() && uri.path != "/") fail("VLESS/Trojan URI 不支持 authority 后的非根路径");
    std::map<std::string, std::string> query;
    for (const auto& [key, value] : uri.query) {
        if (!query.emplace(key, value).second) fail("URI query 含重复参数");
        static constexpr std::string_view common[]{"type", "security", "sni", "fp", "alpn", "insecure", "allowInsecure",
            "host", "path", "serviceName", "mode", "authority", "headerType"};
        if (std::ranges::find(common, key) == std::end(common) &&
            !(vless && (key == "encryption" || key == "flow" || key == "pbk" || key == "sid")))
            fail("VLESS/Trojan URI 含未支持参数");
    }
    const auto get = [&](const char* key, std::string fallback = {}) {
        const auto found = query.find(key);
        return found == query.end() ? std::move(fallback) : found->second;
    };
    const auto security = get("security", vless ? "none" : "tls");
    if ((security != "none" && security != "tls" && security != "reality") || (!vless && security != "tls"))
        fail("VLESS 仅支持 none/tls/reality，Trojan 必须启用 tls");
    auto transport = get("type", "tcp");
    if (transport == "raw") transport = "tcp";
    if (transport != "tcp" && transport != "ws" && transport != "grpc" && transport != "http" &&
        transport != "h2" && transport != "httpupgrade") fail("URI 传输尚无等价映射，不能改为 TCP");
    if (query.contains("headerType") && (transport != "tcp" || get("headerType") != "none"))
        fail("URI headerType 仅支持 TCP 的 none");
    if (transport != "grpc" && (query.contains("serviceName") || query.contains("mode") || query.contains("authority")))
        fail("URI gRPC 参数不能用于其它传输");
    if ((transport == "tcp" || transport == "grpc") && (query.contains("host") || query.contains("path")))
        fail("URI host/path 不能用于当前传输，gRPC 使用 serviceName");

    protocol::NodeSpec node;
    node.origin.name = uri.fragment ? trimCopy(*uri.fragment) : std::string{};
    node.server = uri.host; node.port = uri.port;
    const auto credential = decodeNodeUriComponent(uri.userinfo, fail);
    if (vless) {
        if (!nodeUriUuidValid(credential)) fail("VLESS userinfo 必须为标准 UUID");
        if (get("encryption", "none") != "none") fail("固定内核的 VLESS URI 仅支持 encryption=none");
        protocol::VlessOptions options;
        options.uuid = credential;
        if (query.contains("flow")) {
            const auto flow = get("flow");
            if (flow != "" && flow != "xtls-rprx-vision") fail("URI flow 仅支持空值或 xtls-rprx-vision");
            if (!flow.empty() && (security == "none" || transport != "tcp"))
                fail("Vision 需要 TLS/Reality 和原生 TCP 传输");
            options.flow = flow;
        }
        node.options = std::move(options); // Absent packet encoding retains native VLESS XUDP.
    } else {
        if (credential.empty()) fail("Trojan password 不能为空");
        node.options = protocol::TrojanOptions{credential}; // Entire userinfo, including ':' and '+'.
    }

    node.tls.declaredEnabled = security != "none";
    node.tls.enabled = security != "none";
    if (!node.tls.enabled) {
        for (const char* key : {"sni", "fp", "alpn", "insecure", "allowInsecure", "pbk", "sid"})
            if (query.contains(key)) fail("未启用 TLS，不能应用 TLS/Reality 参数");
    } else {
        node.tls.serverName = get("sni", uri.host); // Transport Host never supplies SNI in this format.
        if (node.tls.serverName->empty()) fail("URI sni 不能为空");
        node.tls.fingerprint = get("fp", "chrome"); // URI default, not a Clash source default.
        if (!protocol::FingerprintSupported(node.tls.fingerprint)) fail("URI fp 不是固定内核支持的指纹");
        if (query.contains("alpn")) {
            node.tls.alpn = nodeUriList(get("alpn"), fail);
            for (const auto& value : *node.tls.alpn) if (value.size() > 255) fail("URI alpn 协议名超过 255 字节");
        }
        std::optional<bool> insecure;
        for (const char* key : {"insecure", "allowInsecure"}) {
            if (!query.contains(key)) continue;
            const auto value = get(key);
            if (value != "true" && value != "false" && value != "0" && value != "1")
                fail("URI insecure/allowInsecure 必须为 true/false 或 0/1");
            const bool enabled = value == "true" || value == "1";
            if (insecure && *insecure != enabled) fail("URI insecure 与 allowInsecure 冲突");
            insecure = enabled;
        }
        if (insecure) node.tls.verifyPeer = !*insecure;
        if (security == "reality") {
            if (transport != "tcp") fail("当前 Reality URI 子集仅支持原生 TCP");
            if (!query.contains("fp") || !query.contains("pbk")) fail("Reality URI 必须显式给出 fp 和 pbk");
            if (insecure) fail("Reality URI 不接受普通 TLS 的 insecure 例外");
            const auto publicKey = get("pbk");
            if (publicKey.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") != std::string::npos)
                fail("Reality pbk 必须为无 padding 的 Base64URL 公钥");
            const auto bytes = decodeNodeBase64(publicKey, 32);
            if (!bytes || bytes->size() != 32) fail("Reality pbk 必须为 32 字节公钥");
            protocol::RealityOptions reality{publicKey, {}};
            if (query.contains("sid")) {
                const auto id = get("sid");
                if (id.size() > 16 || id.size() % 2 || id.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
                    fail("Reality sid 必须为至多 8 字节的偶数长度十六进制");
                reality.shortId = id;
            }
            node.tls.reality = std::move(reality);
        } else if (query.contains("pbk") || query.contains("sid")) fail("pbk/sid 只能用于 Reality，不能降为普通 TLS");
    }

    if (transport == "ws" || transport == "httpupgrade" || transport == "http" || transport == "h2") {
        const auto path = get("path", "/");
        if (path.empty() || !path.starts_with('/')) fail("URI 传输 path 必须为非空绝对路径");
        if (query.contains("host") && get("host").empty()) fail("URI 传输 host 不能为空");
        if (transport == "ws") {
            protocol::WebSocketTransport options;
            options.path = path;
            if (query.contains("host")) options.headers = std::map<std::string, std::string>{{"Host", get("host")}};
            node.transport = std::move(options);
        } else if (transport == "httpupgrade") {
            protocol::HttpUpgradeTransport options;
            options.path = path;
            if (query.contains("host")) options.host = get("host");
            node.transport = std::move(options);
        } else {
            if (!node.tls.enabled) fail("HTTP/2 URI 需要 TLS；不能静默改为原生明文 HTTP/1.1");
            if (node.tls.alpn && (std::ranges::find(*node.tls.alpn, "h2") == node.tls.alpn->end() ||
                    std::ranges::find(*node.tls.alpn, "h3") != node.tls.alpn->end()))
                fail("当前 HTTP/2 URI 的显式 ALPN 必须包含 h2，且不支持 h3");
            protocol::HttpTransport options;
            options.path = path;
            if (query.contains("host")) options.hosts = nodeUriList(get("host"), fail);
            node.transport = std::move(options);
        }
    } else if (transport == "grpc") {
        const auto service = get("serviceName");
        if (service.empty()) fail("gRPC URI 必须给出非空 serviceName");
        if (get("mode", "gun") != "gun") fail("gRPC URI 仅支持 gun 模式");
        if (!get("authority").empty()) fail("固定 gRPC 映射尚不支持非空 authority");
        node.transport = protocol::GrpcTransport{service};
    }
    return node;
}
