// pptp.cppm — clashflux.pptp：系统 PPTP 适配器的跨平台契约。
//
// nativeConfig 使用故意简单、可迁移的 key=value 格式。它只保存拨号参数，
// 不保存路由；路由由 VpnConnection::internalRoutes / VpnPolicy 统一管理。
// 例如：
//   server=vpn.example.com
//   username=alice
//   password=secret
//   interface=company-a
//   timeout=30
//   require_mppe=true
export module clashflux.pptp;

import std;
import clashflux.vpn;

namespace pptp {

export struct PptpConfig {
    std::string server;
    std::string username;
    std::string password;
    std::string interfaceName;
    int connectTimeoutSecs = 30;
    bool requireMppe = true;

    bool operator==(const PptpConfig&) const = default;
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

inline bool containsLineBreak(std::string_view value) {
    return value.find('\n') != std::string_view::npos ||
           value.find('\r') != std::string_view::npos;
}

inline bool validServer(std::string_view server) {
    if (server.empty() || server.front() == '-') return false;
    // Linux 的 pppd `pty` 参数经过 shell 解释；Windows RAS 也不需要这些
    // 字符。收紧到主机名/IP/IPv6 zone 常用字符，避免命令注入和歧义。
    if (!std::ranges::all_of(server, [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '-' || c == ':' ||
               c == '_' || c == '%';
    })) {
        return false;
    }
    // inet_aton 一类历史解析器会把 111.26 当作 111.0.0.26。
    // 对看起来是 IPv4 的纯数字/点输入强制四段十进制，避免
    // 用户连到非预期主机后只得到模糊的 PPP 超时。
    if (std::ranges::all_of(server, [](unsigned char c) {
            return std::isdigit(c) || c == '.';
        })) {
        std::size_t begin = 0;
        for (int part = 0; part < 4; ++part) {
            const std::size_t end = server.find('.', begin);
            const std::size_t limit = end == std::string_view::npos
                                          ? server.size()
                                          : end;
            if (limit <= begin) return false;
            unsigned int octet = 0;
            const auto [ptr, ec] = std::from_chars(
                server.data() + begin, server.data() + limit, octet);
            if (ec != std::errc() || ptr != server.data() + limit ||
                octet > 255 || (limit - begin > 1 && server[begin] == '0')) {
                return false;
            }
            if (part < 3 && end == std::string_view::npos) return false;
            if (part == 3 && end != std::string_view::npos) return false;
            begin = limit + 1;
        }
    }
    return true;
}

inline bool parseBool(std::string_view value, bool& out) {
    std::string lower;
    lower.reserve(value.size());
    for (const unsigned char c : value) {
        lower.push_back(static_cast<char>(std::tolower(c)));
    }
    if (lower == "1" || lower == "true" || lower == "yes" ||
        lower == "on") {
        out = true;
        return true;
    }
    if (lower == "0" || lower == "false" || lower == "no" ||
        lower == "off") {
        out = false;
        return true;
    }
    return false;
}

} // namespace detail

// 解析原生 PPTP 参数。失败时只返回通用错误，不把 password 放进错误文本。
export inline std::optional<PptpConfig> ParsePptpConfig(
    std::string_view text, std::string& error) {
    PptpConfig config;
    std::istringstream input{std::string(text)};
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        const std::string value = detail::trim(line);
        if (value.empty() || value.starts_with('#')) continue;
        const std::size_t equal = value.find('=');
        if (equal == std::string::npos) {
            error = std::format("PPTP 配置第 {} 行缺少 '='", lineNumber);
            return std::nullopt;
        }
        const std::string key = detail::trim(value.substr(0, equal));
        const std::string item = value.substr(equal + 1);
        if (key.empty()) {
            error = std::format("PPTP 配置第 {} 行键名为空", lineNumber);
            return std::nullopt;
        }
        if (detail::containsLineBreak(item)) {
            error = "PPTP 配置包含非法换行";
            return std::nullopt;
        }

        if (key == "server" || key == "host") {
            config.server = detail::trim(item);
        } else if (key == "username" || key == "user") {
            config.username = item;
        } else if (key == "password" || key == "pass") {
            config.password = item;
        } else if (key == "interface" || key == "interface_name") {
            config.interfaceName = detail::trim(item);
        } else if (key == "timeout" || key == "timeout_secs") {
            unsigned int seconds = 0;
            const std::string trimmed = detail::trim(item);
            const auto [ptr, ec] = std::from_chars(
                trimmed.data(), trimmed.data() + trimmed.size(), seconds);
            if (ec != std::errc() || ptr != trimmed.data() + trimmed.size() ||
                seconds == 0 || seconds > 300) {
                error = "PPTP timeout 必须是 1 到 300 秒";
                return std::nullopt;
            }
            config.connectTimeoutSecs = static_cast<int>(seconds);
        } else if (key == "require_mppe" || key == "mppe") {
            if (!detail::parseBool(detail::trim(item), config.requireMppe)) {
                error = "PPTP require_mppe 必须是 true/false";
                return std::nullopt;
            }
        } else {
            error = std::format("PPTP 配置不支持键 '{}'", key);
            return std::nullopt;
        }
    }

    if (config.server.empty() || !detail::validServer(config.server)) {
        error = "PPTP server 无效（IPv4 地址必须使用四段十进制）";
        return std::nullopt;
    }
    if (config.username.empty() || detail::containsLineBreak(config.username)) {
        error = "PPTP username 为空或包含非法换行";
        return std::nullopt;
    }
    if (config.password.empty() || detail::containsLineBreak(config.password)) {
        error = "PPTP password 为空或包含非法换行";
        return std::nullopt;
    }
    if (detail::containsLineBreak(config.interfaceName)) {
        error = "PPTP interface 包含非法换行";
        return std::nullopt;
    }
    return config;
}

// 只报告当前机器是否具备系统拨号后端；不代表当前用户一定拥有建隧道权限。
export bool PptpToolsAvailable();

#if defined(__linux__) && !defined(__ANDROID__)
// 这些接口只供 clash-flux.service 的 root daemon 使用。普通 GUI 进程不应
// 直接调用 pppd/ip；它通过 service 模块的 IPC 客户端调用同一 daemon。
export bool PrivilegedPptpAvailable();
export bool PrivilegedPptpConnect(std::string_view connectionId,
                                  std::string_view nativeConfig,
                                  std::span<const std::string> routes,
                                  std::string& interfaceName,
                                  std::string& gateway,
                                  std::string& error);
export bool PrivilegedPptpApplyRoutes(std::string_view connectionId,
                                      std::span<const std::string> routes,
                                      std::string& error);
export void PrivilegedPptpDisconnect(std::string_view connectionId);
export void PrivilegedPptpShutdown();
#endif

// 创建系统 PPTP 适配器。连接对象的 nativeConfig 在 connect 时解析；
// 适配器不记录密码，也不会把密码拼到命令行或日志中。
export vpn::EngineAdapter MakePptpAdapter();

} // namespace pptp
