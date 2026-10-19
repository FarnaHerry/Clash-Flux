#pragma once

#include <charconv>
#include <optional>
#include <string>
#include <string_view>

namespace clashflux::ui {

enum class PortSettingsError { None, Invalid, Duplicate, NotReady };
struct InboundPorts {
    std::string mixed;
    std::string http;
    std::string socks;
};
struct InboundPortsResult {
    InboundPorts ports;
    PortSettingsError error = PortSettingsError::None;
};

inline InboundPortsResult ParseInboundPorts(std::string_view mixed,
                                           std::string_view http,
                                           std::string_view socks) {
    const auto parse = [](std::string_view text, bool optional) -> std::optional<int> {
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos) return optional ? std::optional{0} : std::nullopt;
        text = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
        int port = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), port);
        if (error != std::errc{} || end != text.data() + text.size() || port < 1 || port > 65535)
            return std::nullopt;
        return port;
    };
    const auto m = parse(mixed, false), h = parse(http, true), s = parse(socks, true);
    if (!m || !h || !s) return {.error = PortSettingsError::Invalid};
    if ((*h > 0 && (*h == *m || *h == *s)) || (*s > 0 && *s == *m))
        return {.error = PortSettingsError::Duplicate};
    return {.ports = {std::to_string(*m), *h == 0 ? "" : std::to_string(*h),
                     *s == 0 ? "" : std::to_string(*s)}};
}

} // namespace clashflux::ui
