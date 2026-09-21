// openvpn.cppm — clashflux.openvpn：OpenVPN 配置校验契约。
//
// nativeConfig 保存完整的 .ovpn 文本，由 sing-box 编译边界翻译成
// openvpn-client endpoint。
export module clashflux.openvpn;

import std;
import clashflux.vpn;

namespace openvpn {

// Byte ranges refer to configText and include optional quotes around the host.
// They are retained for validation/editor diagnostics.
export struct OpenVpnRemote {
    std::string host;
    std::size_t offset = 0;
    std::size_t length = 0;

    bool operator==(const OpenVpnRemote&) const = default;
};

export struct OpenVpnConfig {
    std::string configText;
    std::string interfaceName;
    std::vector<OpenVpnRemote> remotes;
    int connectTimeoutSecs = 60;

    bool operator==(const OpenVpnConfig&) const = default;
};

namespace detail {

inline std::string trim(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

inline std::string directive(std::string_view line) {
    const std::size_t split = line.find_first_of(" \t");
    auto result = line.substr(0, split);
    if (result.starts_with("--")) result.remove_prefix(2);
    return std::string(result);
}

inline bool hasNewline(std::string_view value) {
    return value.find('\0') != std::string_view::npos;
}

inline bool validInterface(std::string_view value) {
    if (value.empty() || value.size() > 64 || value.front() == '-') return false;
    return std::ranges::all_of(value, [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-' || c == '.';
    });
}

} // namespace detail

// 验证托管模式需要的 OpenVPN 配置。路由和 up/down 脚本由引擎接管，避免
// 配置文件绕过全局 VPN 策略修改系统路由或执行用户传入的 root 脚本。
export inline std::optional<OpenVpnConfig> ParseOpenVpnConfig(
    std::string_view text, std::string& error) {
    if (text.empty()) {
        error = "OpenVPN 配置不能为空";
        return std::nullopt;
    }
    // 限制原始配置大小，避免异常配置占用过多编译和传输资源；常见 .ovpn
    // （含 inline 证书）远小于此值。
    if (text.size() > 20000) {
        error = "OpenVPN 配置过大（最多 20 KiB）";
        return std::nullopt;
    }
    if (detail::hasNewline(text)) {
        error = "OpenVPN 配置包含 NUL 字节";
        return std::nullopt;
    }

    OpenVpnConfig config{.configText = std::string(text)};
    std::istringstream input{std::string(text)};
    std::string line;
    std::size_t lineNumber = 0;
    std::size_t offset = 0;
    std::string inlineBlock;
    while (std::getline(input, line)) {
        ++lineNumber;
        const auto lineOffset = offset;
        offset += line.size() + 1;
        const std::string value = detail::trim(line);
        if (!inlineBlock.empty()) {
            if (value == "</" + inlineBlock + ">") inlineBlock.clear();
            continue;
        }
        if (value.empty() || value.starts_with('#') || value.starts_with(';')) {
            continue;
        }
        if (value.starts_with('<') && value.ends_with('>')) {
            if (value != "<connection>" && value != "</connection>") {
                if (value.starts_with("</")) {
                    error = "OpenVPN inline 配置块不匹配";
                    return std::nullopt;
                }
                inlineBlock = value.substr(1, value.size() - 2);
            }
            continue;
        }
        const std::string key = detail::directive(value);
        if (key == "up" || key == "down" || key == "route-up" ||
            key == "route-pre-down" || key == "route" ||
            key == "route-ipv6" || key == "redirect-gateway" ||
            key == "route-noexec" || key == "route-nopull" ||
            key == "daemon" || key == "log" || key == "writepid" ||
            key == "config" || key == "http-proxy" || key == "socks-proxy" ||
            key == "remote-random-hostname" || key == "management-query-remote" ||
            key == "management" || key == "management-client" ||
            key == "management-hold" || key == "askpass" || key == "plugin" ||
            key == "script-security" || key == "auth-user-pass-verify" ||
            key == "tls-verify" || key == "client-connect" ||
            key == "client-disconnect" || key == "learn-address" ||
            key == "dhcp-option") {
            error = std::format(
                "OpenVPN 配置第 {} 行包含托管模式不允许的 '{}' 指令",
                lineNumber, key);
            return std::nullopt;
        }
        if (key == "remote") {
            const auto directiveStart = line.find_first_not_of(" \t\r");
            const auto split = line.find_first_of(" \t", directiveStart);
            const auto start = split == std::string::npos
                                   ? std::string::npos
                                   : line.find_first_not_of(" \t", split);
            if (start == std::string::npos) {
                error = "OpenVPN remote 缺少服务器地址";
                return std::nullopt;
            }
            const bool quoted = line[start] == '\'' || line[start] == '"';
            const auto end = quoted ? line.find(line[start], start + 1)
                                    : line.find_first_of(" \t\r", start);
            if (quoted && (end == std::string::npos ||
                           (end + 1 < line.size() &&
                            !std::isspace(static_cast<unsigned char>(line[end + 1]))))) {
                error = "OpenVPN remote 服务器引号无效";
                return std::nullopt;
            }
            const auto limit = end == std::string::npos ? line.size() : end;
            const auto host = line.substr(start + (quoted ? 1 : 0),
                                           limit - start - (quoted ? 1 : 0));
            if (host.empty() || host.front() == '-' ||
                !std::ranges::all_of(host, [](unsigned char c) {
                    return std::isalnum(c) || c == '.' || c == '-' ||
                           c == '_' || c == ':' || c == '%';
                })) {
                error = "OpenVPN remote 服务器地址无效";
                return std::nullopt;
            }
            config.remotes.push_back({host, lineOffset + start,
                                      limit - start + (quoted ? 1 : 0)});
        }
        if (key == "dev") {
            const std::size_t split = value.find_first_of(" \t");
            const std::string name = detail::trim(
                split == std::string::npos ? std::string_view{}
                                           : std::string_view(value).substr(split));
            // dev tun / dev tun0 都合法；dev-type 等其他参数不影响实际接口名。
            if (name != "tun" && !detail::validInterface(name)) {
                error = std::format("OpenVPN 配置第 {} 行的 dev 无效", lineNumber);
                return std::nullopt;
            }
            if (name != "tun") config.interfaceName = name;
        }
        if (key == "connect-timeout") {
            const std::size_t split = value.find_first_of(" \t");
            const std::string number = detail::trim(
                split == std::string::npos ? std::string_view{}
                                           : std::string_view(value).substr(split));
            unsigned int seconds = 0;
            const auto [ptr, ec] = std::from_chars(
                number.data(), number.data() + number.size(), seconds);
            if (ec != std::errc() || ptr != number.data() + number.size() ||
                seconds == 0 || seconds > 300) {
                error = "OpenVPN connect-timeout 必须是 1 到 300 秒";
                return std::nullopt;
            }
            config.connectTimeoutSecs = static_cast<int>(seconds);
        }
    }
    if (!inlineBlock.empty()) {
        error = "OpenVPN inline 配置块未结束";
        return std::nullopt;
    }
    return config;
}

export vpn::EngineAdapter MakeOpenVpnAdapter();

} // namespace openvpn
