// Documented pre-SIP002 SS format. Its decoded authority is NOT a URI:
// password delimiters and percent sequences are literal bytes, not escapes.
bool nodeShadowsocksLegacy(std::string_view remainder) {
    return remainder.substr(0, remainder.find_first_of("?#")).find('@') == std::string_view::npos;
}

template<class Fail>
protocol::NodeSpec parseNodeLegacyShadowsocks(std::string_view remainder, const Fail& fail) {
    if (remainder.size() > 64 * 1024 - 5) fail("SS URI 超过 64 KiB 上限");
    protocol::NodeSpec node;
    if (const auto hash = remainder.find('#'); hash != std::string_view::npos) {
        if (remainder.find('#', hash + 1) != std::string_view::npos) fail("SS 历史格式含重复 fragment 分隔符");
        const auto fragment = remainder.substr(hash + 1);
        if (std::ranges::any_of(fragment, [](unsigned char c) { return c <= 32; })) fail("SS 名称含未编码空白");
        node.origin.name = trimCopy(decodeNodeUriComponent(fragment, fail));
        remainder = remainder.substr(0, hash);
    }
    if (remainder.find('?') != std::string_view::npos) fail("SS 历史格式不支持 query 或插件；请使用 SIP002");
    if (remainder.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=") != std::string_view::npos)
        fail("SS 历史格式必须为标准 Base64");
    const auto decoded = decodeNodeBase64(remainder, 64 * 1024);
    if (!decoded || !nodeUriTextValid(*decoded)) fail("SS 历史格式的 Base64 或 UTF-8 无效");
    const auto colon = decoded->find(':'), at = decoded->rfind('@');
    if (colon == std::string::npos || at == std::string::npos || colon >= at) fail("SS 历史格式缺少 method:password@host:port");
    protocol::ShadowsocksOptions options;
    options.method = decoded->substr(0, colon);
    options.password = decoded->substr(colon + 1, at - colon - 1);
    validateNodeShadowsocksCredentials(options, true, fail); // Never wrap AEAD-2022 credentials in this old format.
    const auto endpoint = std::string_view(*decoded).substr(at + 1);
    if (endpoint.find_first_of("/?#%@") != std::string_view::npos) fail("SS 历史格式的服务器不是原始 host:port");
    const auto address = parseNodeShareUri("placeholder@" + std::string(endpoint), fail);
    node.server = address.host; node.port = address.port; node.options = std::move(options);
    return node;
}
