// Private URI syntax, followed by format-specific adaptation into protocol values.
// Keep encoded userinfo until its protocol has split the raw component boundary.
struct NodeShareUri {
    std::string userinfo, host, path;
    std::uint16_t port = 0;
    std::vector<std::pair<std::string, std::string>> query;
    std::optional<std::string> fragment;
    std::optional<std::string> portSet;
};

struct NodeUriSyntax {
    bool optionalUserinfo = false;
    std::optional<std::uint16_t> defaultPort;
    bool allowPortSet = false;
};

bool nodeUriTextValid(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char first = text[i++];
        if (first < 32 || first == 127) return false;
        if (first < 128) continue;
        unsigned count, value, minimum;
        if (first >= 0xc2 && first <= 0xdf) { count = 1; value = first & 31; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { count = 2; value = first & 15; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { count = 3; value = first & 7; minimum = 0x10000; }
        else return false;
        if (count > text.size() - i) return false;
        while (count--) {
            const unsigned char next = text[i++];
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 63);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}

template<class Fail>
std::string decodeNodeUriComponent(std::string_view raw, const Fail& fail) {
    const auto hex = [](unsigned char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string value;
    value.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '%') { value += raw[i]; continue; } // '+' is never form-decoded.
        if (raw.size() - i < 3 || hex(raw[i + 1]) < 0 || hex(raw[i + 2]) < 0) fail("URI 含无效百分号转义");
        value += static_cast<char>((hex(raw[i + 1]) << 4) | hex(raw[i + 2]));
        i += 2;
    }
    if (!nodeUriTextValid(value)) fail("URI 组件含控制字符或无效 UTF-8");
    return value;
}

template<class Fail>
NodeShareUri parseNodeShareUri(std::string_view remainder, const Fail& fail, const NodeUriSyntax& syntax = {}) {
    if (remainder.size() > 64 * 1024) fail("URI 超过 64 KiB 上限");
    if (!nodeUriTextValid(remainder) || std::ranges::any_of(remainder, [](unsigned char c) { return c <= 32; }))
        fail("URI 含未编码空白、控制字符或无效 UTF-8");
    NodeShareUri uri;
    if (const auto hash = remainder.find('#'); hash != std::string_view::npos) {
        if (remainder.find('#', hash + 1) != std::string_view::npos) fail("URI 含重复 fragment 分隔符");
        uri.fragment = decodeNodeUriComponent(remainder.substr(hash + 1), fail);
        remainder = remainder.substr(0, hash);
    }
    if (const auto question = remainder.find('?'); question != std::string_view::npos) {
        auto query = remainder.substr(question + 1);
        remainder = remainder.substr(0, question);
        while (!query.empty()) {
            if (uri.query.size() == 32) fail("URI query 超过 32 项上限");
            const auto ampersand = query.find('&');
            const auto entry = query.substr(0, ampersand);
            const auto equals = entry.find('=');
            if (entry.empty() || equals == 0 || equals == std::string_view::npos) fail("URI query 必须为非空 key=value 条目");
            uri.query.emplace_back(decodeNodeUriComponent(entry.substr(0, equals), fail),
                decodeNodeUriComponent(entry.substr(equals + 1), fail));
            if (ampersand == std::string_view::npos) break;
            query.remove_prefix(ampersand + 1);
            if (query.empty()) fail("URI query 含空条目");
        }
    }
    if (const auto slash = remainder.find('/'); slash != std::string_view::npos) {
        uri.path = decodeNodeUriComponent(remainder.substr(slash), fail);
        remainder = remainder.substr(0, slash);
    }
    const auto at = remainder.find('@');
    if ((at == std::string_view::npos && !syntax.optionalUserinfo) || (at == 0 && !syntax.optionalUserinfo) ||
            (at != std::string_view::npos && remainder.find('@', at + 1) != std::string_view::npos))
        fail("URI 必须有唯一的 userinfo@host:port；旧式整串 Base64 尚未支持");
    if (at != std::string_view::npos) uri.userinfo = remainder.substr(0, at);
    const auto endpoint = at == std::string_view::npos ? remainder : remainder.substr(at + 1);
    std::string_view host, port;
    bool explicitPort = false;
    if (endpoint.starts_with('[')) {
        const auto close = endpoint.find(']');
        if (close == std::string_view::npos || (close + 1 == endpoint.size() ? !syntax.defaultPort : endpoint[close + 1] != ':'))
            fail("URI 的 IPv6 方括号或端口无效");
        host = endpoint.substr(1, close - 1);
        explicitPort = close + 1 < endpoint.size();
        if (explicitPort) port = endpoint.substr(close + 2);
        if (!clashflux::network::IPv6(host)) fail("URI 的 IPv6 地址无效或包含未支持的 zone");
        uri.host = host;
    } else {
        const auto colon = endpoint.find(':');
        if ((colon == std::string_view::npos && !syntax.defaultPort) ||
                (colon != std::string_view::npos && endpoint.find(':', colon + 1) != std::string_view::npos))
            fail("URI 必须显式给出端口；IPv6 必须使用方括号");
        host = endpoint.substr(0, colon);
        explicitPort = colon != std::string_view::npos;
        if (explicitPort) port = endpoint.substr(colon + 1);
        uri.host = decodeNodeUriComponent(host, fail);
        if (uri.host.empty() || std::ranges::any_of(uri.host, [](unsigned char c) {
                return !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_');
            })) fail("URI 的服务器必须是 IP 或 ASCII 主机名");
    }
    if (!explicitPort) { uri.port = *syntax.defaultPort; return uri; }
    if (syntax.allowPortSet && port.find_first_of(",-") != std::string_view::npos) {
        uri.portSet = port; // The protocol adapter validates range semantics before using it.
        return uri;
    }
    unsigned number = 0;
    const auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), number);
    if (port.empty() || error != std::errc{} || end != port.data() + port.size() || number == 0 || number > 65535)
        fail("URI 端口必须为 1–65535 的十进制整数");
    uri.port = static_cast<std::uint16_t>(number);
    return uri;
}

struct NodePluginArgument { std::string key; std::optional<std::string> value; };

template<class Fail>
std::vector<NodePluginArgument> parseNodePluginArguments(std::string_view text, const Fail& fail) {
    std::vector<NodePluginArgument> arguments;
    for (std::size_t i = 0; i < text.size();) {
        if (arguments.size() == 32) fail("SS plugin 超过 32 项上限");
        NodePluginArgument argument;
        while (i < text.size()) {
            char c = text[i++];
            if (c == ';') break;
            if (c == '\\') {
                if (i == text.size() || std::string_view("\\;=:").find(text[i]) == std::string_view::npos)
                    fail("SS plugin 含无效 SIP003 转义");
                c = text[i++];
            } else if (c == '=' && !argument.value) { argument.value.emplace(); continue; }
            if (argument.value) *argument.value += c;
            else argument.key += c;
        }
        if (argument.key.empty()) fail("SS plugin 含空参数名");
        arguments.push_back(std::move(argument));
    }
    return arguments;
}

template<class Fail>
void readNodeShadowsocksPlugin(std::string_view text, protocol::ShadowsocksOptions& options, const Fail& fail) {
    const auto arguments = parseNodePluginArguments(text, fail);
    if (arguments.empty() || arguments[0].value) fail("SS plugin 缺少有效插件名");
    const bool obfs = arguments[0].key == "obfs-local";
    if (!obfs && arguments[0].key != "v2ray-plugin") fail("SS plugin 仅支持 obfs-local/v2ray-plugin 子集");
    std::set<std::string> keys;
    protocol::SimpleObfsPlugin simple;
    protocol::V2RayWebSocketPlugin websocket;
    websocket.multiplex = true; // The fixed SIP003 implementation defaults mux to 1.
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        const auto& [key, value] = arguments[i];
        if (!keys.insert(key).second) fail("SS plugin 含重复选项");
        if (obfs && key == "obfs") {
            if (!value || (*value != "http" && *value != "tls")) fail("SS plugin 的 obfs 仅支持 http/tls");
            simple.mode = *value == "http" ? protocol::SimpleObfsMode::Http : protocol::SimpleObfsMode::Tls;
        } else if ((obfs && key == "obfs-host") || (!obfs && key == "host")) {
            if (!value || value->empty()) fail("SS plugin 的 host 必须为非空字符串");
            if (obfs) simple.host = *value; else websocket.host = *value;
        } else if (!obfs && key == "mode") {
            if (value != "websocket") fail("SS plugin 的 mode 仅支持 websocket");
        } else if (!obfs && key == "path") {
            if (!value) fail("SS plugin 的 path 必须有值");
            websocket.path = *value;
        } else if (!obfs && key == "tls") {
            if (value) fail("SS plugin 的 tls 只支持无值标志；不能把 tls=false 当作关闭");
            websocket.tls = true;
        } else if (!obfs && key == "mux") {
            if (value && *value != "0" && *value != "1") fail("SS plugin 的 mux 仅支持 0/1");
            websocket.multiplex = value != "0";
        } else fail("SS plugin 含未支持的选项");
    }
    if (obfs) options.plugin = std::move(simple); else options.plugin = std::move(websocket);
}

template<class Fail>
void validateNodeShadowsocksCredentials(const protocol::ShadowsocksOptions& options, bool encoded, const Fail& fail) {
    static constexpr std::string_view methods[]{"none", "aes-128-gcm", "aes-192-gcm", "aes-256-gcm",
        "chacha20-ietf-poly1305", "xchacha20-ietf-poly1305", "2022-blake3-aes-128-gcm",
        "2022-blake3-aes-256-gcm", "2022-blake3-chacha20-poly1305", "aes-128-ctr", "aes-192-ctr", "aes-256-ctr",
        "aes-128-cfb", "aes-192-cfb", "aes-256-cfb", "rc4-md5", "chacha20-ietf", "xchacha20"};
    if (std::ranges::find(methods, options.method) == std::end(methods)) fail("SS method 不在固定内核支持范围");
    if (encoded && options.method.starts_with("2022-")) fail("SS 2022 userinfo 不得使用 Base64URL 包装");
    if (options.password->empty()) fail("SS password 不能为空");
}

template<class Fail>
protocol::NodeSpec parseNodeShadowsocks(std::string_view remainder, const Fail& fail) {
    if (remainder.size() > 64 * 1024 - 5) fail("SS URI 超过 64 KiB 上限");
    const auto uri = parseNodeShareUri(remainder, fail);
    if (!uri.path.empty() && uri.path != "/") fail("SS URI 不支持非根路径");
    protocol::ShadowsocksOptions options;
    const auto colon = uri.userinfo.find(':');
    const bool encoded = colon == std::string::npos;
    if (encoded) {
        auto userinfo = decodeNodeUriComponent(uri.userinfo, fail);
        if (userinfo.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-=") != std::string::npos)
            fail("SS userinfo 必须为 Base64URL");
        const auto decoded = decodeNodeBase64(userinfo, 64 * 1024);
        if (!decoded || !nodeUriTextValid(*decoded)) fail("SS userinfo 的 Base64URL 或 UTF-8 无效");
        const auto separator = decoded->find(':');
        if (separator == std::string::npos) fail("SS userinfo 缺少 method:password");
        options.method = decoded->substr(0, separator); options.password = decoded->substr(separator + 1);
    } else {
        options.method = decodeNodeUriComponent(std::string_view(uri.userinfo).substr(0, colon), fail);
        options.password = decodeNodeUriComponent(std::string_view(uri.userinfo).substr(colon + 1), fail);
    }
    validateNodeShadowsocksCredentials(options, encoded, fail);
    std::set<std::string> keys;
    for (const auto& [key, value] : uri.query) {
        if (!keys.insert(key).second) fail("SS query 含重复参数");
        if (key != "plugin") fail("SS query 含未支持参数");
        readNodeShadowsocksPlugin(value, options, fail);
    }
    protocol::NodeSpec node;
    node.origin.name = uri.fragment ? trimCopy(*uri.fragment) : std::string{};
    node.server = uri.host; node.port = uri.port; node.options = std::move(options);
    return node;
}

bool nodeUriUuidValid(std::string_view uuid) {
    if (uuid.size() != 36) return false;
    for (std::size_t i = 0; i < uuid.size(); ++i) {
        const char c = uuid[i];
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? c != '-' : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

std::string_view nodeSubscriptionType(const protocol::NodeSpec& node) {
    return nodeInputProtocol(nodeInputKind(node)).protocol;
}
