// Audited semantic baseline. The required gate compares it with the packaged
// executable, Android libbox identity and CI pin; a core upgrade requires review.
constexpr std::string_view kNodeInputCoreVersion = "1.14.4";
constexpr std::string_view kNodeInputCoreRevision = "c005f35300ce76427dc285481ec5695f5b179cd2";

enum class NodeInputKind { Shadowsocks, Vmess, Vless, Trojan, Hysteria2, Tuic, AnyTls };
struct NodeInputRegistration {
    NodeInputKind kind;
    std::string_view protocol, nativeType, defaultName;
};
constexpr NodeInputRegistration kNodeInputProtocols[]{
    {NodeInputKind::Shadowsocks, "ss", "shadowsocks", "Shadowsocks "},
    {NodeInputKind::Vmess, "vmess", "vmess", "VMess "},
    {NodeInputKind::Vless, "vless", "vless", "VLESS "},
    {NodeInputKind::Trojan, "trojan", "trojan", "Trojan "},
    {NodeInputKind::Hysteria2, "hysteria2", "hysteria2", "Hysteria2 "},
    {NodeInputKind::Tuic, "tuic", "tuic", "TUIC "},
    {NodeInputKind::AnyTls, "anytls", "anytls", "AnyTLS "},
};
struct NodeSchemeRegistration { std::string_view scheme; NodeInputKind kind; bool caseSensitive = false; };
constexpr NodeSchemeRegistration kNodeInputSchemes[]{
    {"ss", NodeInputKind::Shadowsocks}, {"vmess", NodeInputKind::Vmess, true},
    {"vless", NodeInputKind::Vless}, {"trojan", NodeInputKind::Trojan},
    {"hysteria2", NodeInputKind::Hysteria2}, {"hy2", NodeInputKind::Hysteria2},
    {"tuic", NodeInputKind::Tuic}, {"anytls", NodeInputKind::AnyTls},
};
constexpr std::size_t kNodeSchemeMaxBytes = [] {
    std::size_t length = 0;
    for (const auto& input : kNodeInputSchemes) length = std::max(length, input.scheme.size());
    return length;
}();
struct NodeFormatRegistration { std::string_view id; NodeInputKind kind; bool legacy = false; };
constexpr NodeFormatRegistration kNodeInputFormats[]{
    {"ss-sip002", NodeInputKind::Shadowsocks}, {"ss-legacy-base64", NodeInputKind::Shadowsocks, true},
    {"vmess-json", NodeInputKind::Vmess}, {"vless-uri", NodeInputKind::Vless}, {"trojan-uri", NodeInputKind::Trojan},
    {"hysteria2-uri", NodeInputKind::Hysteria2}, {"tuic-v5-uri", NodeInputKind::Tuic}, {"anytls-uri", NodeInputKind::AnyTls},
};

const NodeInputRegistration& nodeInputProtocol(NodeInputKind kind) {
    for (const auto& input : kNodeInputProtocols) if (input.kind == kind) return input;
    throw std::logic_error("节点输入协议未登记");
}

std::optional<NodeInputKind> nodeInputKind(std::string_view scheme) {
    if (scheme.size() > kNodeSchemeMaxBytes) return std::nullopt;
    const auto lower = lowerCopy(std::string(scheme));
    for (const auto& input : kNodeInputSchemes)
        if ((input.caseSensitive ? scheme : std::string_view(lower)) == input.scheme) return input.kind;
    return std::nullopt;
}

NodeInputKind nodeInputKind(const protocol::NodeSpec& node) {
    if (std::holds_alternative<protocol::ShadowsocksOptions>(node.options)) return NodeInputKind::Shadowsocks;
    if (std::holds_alternative<protocol::VmessOptions>(node.options)) return NodeInputKind::Vmess;
    if (std::holds_alternative<protocol::VlessOptions>(node.options)) return NodeInputKind::Vless;
    if (std::holds_alternative<protocol::TrojanOptions>(node.options)) return NodeInputKind::Trojan;
    if (std::holds_alternative<protocol::Hysteria2Options>(node.options)) return NodeInputKind::Hysteria2;
    if (std::holds_alternative<protocol::TuicOptions>(node.options)) return NodeInputKind::Tuic;
    if (std::holds_alternative<protocol::AnyTlsOptions>(node.options)) return NodeInputKind::AnyTls;
    throw std::logic_error("节点订阅的协议模型尚未注册筛选类型");
}
