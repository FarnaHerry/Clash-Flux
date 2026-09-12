// openvpn.cppm — clashflux.openvpn：OpenVPN CLI 引擎契约。
//
// nativeConfig 保存完整的 .ovpn 文本。引擎会在 root 服务的私有目录中写出
// 0600 配置文件，以 `openvpn --config` 建立 tun 接口；连接自己的内网路由
// 仍由 clashflux.vpn 的统一策略通过 root 服务安装。
export module clashflux.openvpn;

import std;
import clashflux.vpn;

namespace openvpn {

export struct OpenVpnConfig {
    std::string configText;
    std::string interfaceName;
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
    return std::string(line.substr(0, split));
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
    // service socket 使用十六进制单行协议；限制原始配置大小，避免一份配置
    // 把单行 IPC 请求撑过 daemon 的上限。常见 .ovpn（含 inline 证书）远小于此值。
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
    while (std::getline(input, line)) {
        ++lineNumber;
        const std::string value = detail::trim(line);
        if (value.empty() || value.starts_with('#') || value.starts_with(';')) {
            continue;
        }
        const std::string key = detail::directive(value);
        if (key == "up" || key == "down" || key == "route-up" ||
            key == "route-pre-down" || key == "route" ||
            key == "route-ipv6" || key == "redirect-gateway" ||
            key == "route-noexec" || key == "route-nopull" ||
            key == "daemon" || key == "log" || key == "writepid") {
            error = std::format(
                "OpenVPN 配置第 {} 行包含托管模式不允许的 '{}' 指令",
                lineNumber, key);
            return std::nullopt;
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
    return config;
}

// 当前机器是否具备 OpenVPN CLI 和统一 root 服务后端。
export bool OpenVpnToolsAvailable();

#if defined(__linux__) && !defined(__ANDROID__)
export bool PrivilegedOpenVpnAvailable();
export bool PrivilegedOpenVpnConnect(std::string_view connectionId,
                                     std::string_view nativeConfig,
                                     std::span<const std::string> routes,
                                     std::string& interfaceName,
                                     std::string& gateway,
                                     std::string& error);
export bool PrivilegedOpenVpnApplyRoutes(std::string_view connectionId,
                                         std::span<const std::string> routes,
                                         std::string& error);
export void PrivilegedOpenVpnDisconnect(std::string_view connectionId);
export void PrivilegedOpenVpnShutdown();
#endif

export vpn::EngineAdapter MakeOpenVpnAdapter();

} // namespace openvpn
