// Bounded, owning protocol fields shared by the Windows service and its clients.
#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace clashflux::service_protocol {
inline constexpr std::size_t kMaxCommand = 65536;
inline std::string hex(std::string_view value) {
    constexpr char digits[] = "0123456789abcdef";
    if (value.empty()) return "-";
    std::string result;
    result.reserve(value.size() * 2);
    for (unsigned char c : value) { result += digits[c >> 4]; result += digits[c & 15]; }
    return result;
}
inline std::optional<std::string> unhex(std::string_view value) {
    if (value == "-") return std::string{};
    if (value.empty() || value.size() % 2 || value.size() > kMaxCommand) return {};
    auto nibble = [](char c) { return c >= '0' && c <= '9' ? c - '0' :
        c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    std::string result;
    for (std::size_t i = 0; i < value.size(); i += 2) {
        const int a = nibble(value[i]), b = nibble(value[i + 1]);
        if (a < 0 || b < 0 || (a == 0 && b == 0)) return {};
        result += static_cast<char>((a << 4) | b);
    }
    return result;
}
inline std::vector<std::string> words(std::string_view value) {
    std::vector<std::string> result;
    while (!value.empty()) {
        const auto end = value.find(' ');
        if (end == 0) return {}; // No ambiguous/empty fields.
        result.emplace_back(value.substr(0, end));
        if (end == std::string_view::npos) break;
        value.remove_prefix(end + 1);
        if (value.empty()) return {};
    }
    return result;
}
} // namespace clashflux::service_protocol
