// VMess JSON share links, raw or wrapped in subscription Base64. Keep original
// profile bytes on disk; this bounded adapter only builds the compiler's input.
constexpr std::size_t kNodeSubscriptionBytes = 8 * 1024 * 1024;
constexpr std::size_t kNodeSubscriptionCount = 4096;

std::optional<std::string> decodeNodeBase64(std::string_view input, std::size_t maximum) {
    std::string encoded;
    for (const unsigned char c : input) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (encoded.size() >= ((maximum + 2) / 3) * 4) return std::nullopt;
        encoded += static_cast<char>(c);
    }
    if (encoded.empty()) return std::nullopt;
    const auto dataEnd = encoded.find('=');
    const auto count = dataEnd == std::string::npos ? encoded.size() : dataEnd;
    const auto padding = encoded.size() - count;
    if (count % 4 == 1 || padding > 2 || (padding && encoded.size() % 4 != 0) ||
        (padding && padding != (4 - count % 4) % 4)) return std::nullopt;
    for (std::size_t i = count; i < encoded.size(); ++i) if (encoded[i] != '=') return std::nullopt;
    std::string output;
    unsigned value = 0, bits = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const unsigned char c = encoded[i];
        unsigned digit;
        if (c >= 'A' && c <= 'Z') digit = c - 'A';
        else if (c >= 'a' && c <= 'z') digit = c - 'a' + 26;
        else if (c >= '0' && c <= '9') digit = c - '0' + 52;
        else if (c == '+' || c == '-') digit = 62;
        else if (c == '/' || c == '_') digit = 63;
        else return std::nullopt;
        value = (value << 6) | digit;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (output.size() == maximum) return std::nullopt;
            output += static_cast<char>((value >> bits) & 255);
            value &= (1U << bits) - 1;
        }
    }
    if (value != 0) return std::nullopt; // Reject noncanonical trailing bits.
    return output;
}

std::string_view firstNodeLine(std::string_view text) {
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = text.substr(0, end);
        const auto begin = line.find_first_not_of(" \t\r");
        if (begin != std::string_view::npos) {
            line.remove_prefix(begin);
            if (!line.starts_with('#')) return line;
        }
        if (end == std::string_view::npos) break;
        text.remove_prefix(end + 1);
    }
    return {};
}

bool isNodeLink(std::string_view line) {
    const auto end = line.find("://");
    if (end == std::string_view::npos || end == 0) return false;
    for (std::size_t i = 0; i < end; ++i) {
        const auto c = line[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (i && ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.')))) return false;
    }
    return true;
}

std::optional<std::string> nodeSubscriptionText(std::string_view text) {
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    if (isNodeLink(firstNodeLine(text))) return std::string(text);
    if (text.size() > kNodeSubscriptionBytes) return std::nullopt;
    auto decoded = decodeNodeBase64(text, kNodeSubscriptionBytes);
    if (decoded && isNodeLink(firstNodeLine(*decoded))) return decoded;
    return std::nullopt;
}

struct NodeSubscriptionDocument {
    YAML::Node root;
    std::vector<std::string> tlsTransportAliases;
};

NodeSubscriptionDocument parseNodeSubscription(const std::string& text) {
    if (text.size() > kNodeSubscriptionBytes) throw std::runtime_error("节点订阅超过 8 MiB 上限");
    NodeSubscriptionDocument result;
    result.root = YAML::Node(YAML::NodeType::Map);
    YAML::Node proxies(YAML::NodeType::Sequence), members(YAML::NodeType::Sequence);
    std::set<std::string> names{"DIRECT", "REJECT", "REJECT-DROP"};
    std::istringstream lines(text.starts_with("\xEF\xBB\xBF") ? text.substr(3) : text);
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(lines, line)) {
        ++lineNumber;
        line = trimCopy(line);
        if (line.empty() || line.starts_with('#')) continue;
        const auto fail = [&](std::string_view reason) -> void {
            // Never put a URI, UUID, endpoint or decoded JSON into diagnostics.
            throw std::runtime_error(std::format("节点订阅第 {} 行：{}", lineNumber, reason));
        };
        if (proxies.size() >= kNodeSubscriptionCount) fail("节点数量超过 4096 上限");
        if (!line.starts_with("vmess://")) fail("目前仅支持 VMess JSON 分享链接，不能丢弃其它条目");
        auto payload = decodeNodeBase64(std::string_view(line).substr(8), 64 * 1024);
        if (!payload) fail("VMess 链接的 Base64 无效或超过 64 KiB");
        std::set<std::string> fields;
        bool duplicate = false;
        const auto item = nlohmann::json::parse(*payload,
            [&](int depth, nlohmann::json::parse_event_t event, nlohmann::json& parsed) {
                if (depth > 4) fail("VMess JSON 嵌套过深");
                if (depth == 1 && event == nlohmann::json::parse_event_t::key &&
                    !fields.insert(parsed.get<std::string>()).second) duplicate = true;
                return true;
            }, false);
        if (!item.is_object() || duplicate) fail("VMess JSON 必须是没有重复字段的对象");
        for (auto field = item.cbegin(); field != item.cend(); ++field) {
            const auto& key = field.key();
            if (key != "v" && key != "ps" && key != "add" && key != "port" && key != "id" &&
                key != "aid" && key != "scy" && key != "net" && key != "type" && key != "host" &&
                key != "path" && key != "tls" && key != "sni" && key != "alpn" && key != "fp" &&
                key != "insecure" && key != "allowInsecure" && key != "vcn" && key != "pcs")
                fail("VMess JSON 含未支持字段，保留原订阅");
        }
        const auto string = [&](const char* key) -> std::string {
            if (!item.contains(key) || item[key].is_null()) return {};
            if (!item[key].is_string()) fail(std::string(key) + " 必须是字符串");
            const auto value = item[key].get<std::string>();
            if (std::ranges::any_of(value, [](unsigned char c) { return c < 32 || c == 127; }))
                fail(std::string(key) + " 不能含控制字符");
            return value;
        };
        const auto integer = [&](const char* key, int fallback, int minimum, int maximum) {
            if (!item.contains(key)) return fallback;
            std::string value;
            if (item[key].is_string()) value = item[key].get<std::string>();
            else if (item[key].is_number_integer()) value = item[key].dump();
            else fail(std::string(key) + " 必须是整数或十进制整数字符串");
            int parsed = 0;
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (error != std::errc{} || end != value.data() + value.size() || parsed < minimum || parsed > maximum)
                fail(std::string(key) + " 超出有效范围");
            return parsed;
        };
        const auto flag = [&](const char* key) {
            if (!item.contains(key)) return false;
            if (item[key].is_boolean()) return item[key].get<bool>();
            if (item[key] == 1 || item[key] == "1" || item[key] == "true") return true;
            if (item[key] == 0 || item[key] == "0" || item[key] == "false") return false;
            fail(std::string(key) + " 必须为布尔值或 0/1");
            return false;
        };
        (void)integer("v", 2, 2, 2);
        if (!string("vcn").empty() || !string("pcs").empty()) fail("尚不支持证书名称覆盖或证书指纹钉扎");
        auto name = trimCopy(string("ps"));
        if (name.empty()) name = "VMess " + std::to_string(proxies.size() + 1);
        if (!names.insert(name).second) fail("节点名称重复或与内置出口重名");
        const auto address = string("add"), uuid = string("id");
        if (address.empty() || std::ranges::any_of(address, [](unsigned char c) { return c <= 32; }))
            fail("add 必须为非空服务器地址");
        if (uuid.size() != 36) fail("id 必须为标准 UUID");
        for (std::size_t i = 0; i < uuid.size(); ++i) {
            const char c = uuid[i];
            const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
            if (dash ? c != '-' : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                fail("id 必须为标准 UUID");
        }
        const int port = integer("port", 0, 1, 65535), alterId = integer("aid", 0, 0, 65535);
        if (!port) fail("缺少服务器端口");
        auto network = lowerCopy(string("net"));
        if (network.empty()) network = "tcp";
        const auto tls = lowerCopy(string("tls"));
        if (!tls.empty() && tls != "none" && tls != "tls") fail("尚不支持该 TLS 类型");
        const bool tlsEnabled = tls == "tls";
        if (network == "tls") {
            if (!tlsEnabled) fail("net=tls 兼容写法必须同时显式启用 TLS");
            network = "tcp";
            result.tlsTransportAliases.push_back(name);
        }
        if (network != "tcp" && network != "ws" && network != "h2" && network != "http" && network != "grpc")
            fail("传输类型尚无等价映射，不能改为 TCP");
        const auto disguise = lowerCopy(string("type"));
        if (!disguise.empty() && disguise != "none") fail("尚不支持该伪装或传输模式");
        const auto host = string("host"), path = string("path");
        auto sni = string("sni");
        const auto alpn = string("alpn"), fingerprint = string("fp");
        const bool insecure = flag("insecure"), allowInsecure = flag("allowInsecure");
        if (item.contains("insecure") && item.contains("allowInsecure") && insecure != allowInsecure)
            fail("insecure 与 allowInsecure 存在冲突");
        if (!tlsEnabled && (!sni.empty() || !alpn.empty() || !fingerprint.empty() || insecure || allowInsecure))
            fail("未启用 TLS，不能应用 TLS 字段");
        if (network == "tcp" && !path.empty() && path != "/") fail("TCP 不支持传输路径");
        if (network == "grpc" && !host.empty()) fail("gRPC authority 尚无等价映射");
        YAML::Node proxy(YAML::NodeType::Map);
        proxy["name"] = name; proxy["type"] = "vmess";
        proxy["server"] = address; proxy["port"] = port; proxy["uuid"] = uuid;
        proxy["alterId"] = alterId;
        const auto cipher = string("scy");
        proxy["cipher"] = cipher.empty() ? "auto" : cipher;
        proxy["network"] = network;
        proxy["tls"] = tlsEnabled;
        if (tlsEnabled) {
            // Match established VMess client behavior: explicit SNI wins,
            // otherwise the first transport host supplies the TLS server name.
            if (sni.empty()) sni = trimCopy(std::string_view(host).substr(0, host.find(',')));
            if (!sni.empty()) proxy["servername"] = sni;
            if (insecure || allowInsecure) proxy["skip-cert-verify"] = true;
            if (!fingerprint.empty()) proxy["client-fingerprint"] = fingerprint;
            if (!alpn.empty()) {
                YAML::Node protocols(YAML::NodeType::Sequence);
                std::size_t start = 0;
                for (;;) {
                    const auto end = alpn.find(',', start);
                    auto protocol = trimCopy(std::string_view(alpn).substr(start,
                        end == std::string::npos ? end : end - start));
                    if (protocol.empty() || protocol.size() > 255) fail("alpn 列表包含无效协议名");
                    protocols.push_back(protocol);
                    if (end == std::string::npos) break;
                    start = end + 1;
                }
                proxy["alpn"] = protocols;
            }
        }
        if (network == "ws") {
            if (!path.empty()) proxy["ws-opts"]["path"] = path;
            if (!host.empty()) proxy["ws-opts"]["headers"]["Host"] = host;
        } else if (network == "h2" || network == "http") {
            const char* options = network == "h2" ? "h2-opts" : "http-opts";
            if (!path.empty()) proxy[options]["path"].push_back(path);
            if (!host.empty()) {
                YAML::Node hosts(YAML::NodeType::Sequence);
                std::size_t start = 0;
                for (;;) {
                    const auto end = host.find(',', start);
                    const auto value = trimCopy(std::string_view(host).substr(start,
                        end == std::string::npos ? end : end - start));
                    if (value.empty()) fail("host 列表含空值");
                    hosts.push_back(value);
                    if (end == std::string::npos) break;
                    start = end + 1;
                }
                proxy[options]["host"] = hosts;
            }
        } else if (network == "grpc" && !path.empty()) proxy["grpc-opts"]["grpc-service-name"] = path;
        proxies.push_back(proxy); members.push_back(name);
    }
    if (proxies.size() == 0) throw std::runtime_error("节点订阅中没有可用节点");
    std::string groupName = "节点选择";
    for (unsigned i = 2; names.contains(groupName); ++i) groupName = "节点选择 (" + std::to_string(i) + ")";
    result.root["proxies"] = proxies;
    YAML::Node group(YAML::NodeType::Map);
    group["name"] = groupName; group["type"] = "select"; group["proxies"] = members;
    result.root["proxy-groups"].push_back(group);
    result.root["rules"].push_back("MATCH," + groupName);
    return result;
}

YAML::Node loadClashProfileDocument(const std::string& content) {
    if (auto text = nodeSubscriptionText(content)) return parseNodeSubscription(*text).root;
    return YAML::Load(content);
}
