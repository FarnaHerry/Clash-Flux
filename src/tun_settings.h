#pragma once

#include <charconv>
#include <optional>
#include <string_view>

namespace clashflux::tun_settings {
inline bool ValidMtu(int mtu) { return mtu >= 1280 && mtu <= 9000; }
inline std::optional<int> ParseMtu(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return std::nullopt;
    text = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || !ValidMtu(value)) return std::nullopt;
    return value;
}
} // namespace clashflux::tun_settings
