#pragma once
#include <chrono>
#include <string>
#include <string_view>

namespace clashflux::website {
inline constexpr auto kTimeout = std::chrono::seconds{10};
inline constexpr const char* kTimeoutError = "网站测试超时（10 秒）";
inline std::string TraceCountry(std::string_view text) {
    std::string country;
    bool address = false;
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = text.substr(0, end);
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (line.starts_with("ip=") && line.size() > 3) address = true;
        if (line.starts_with("loc=")) {
            if (!country.empty() || line.size() != 6 || line[4] < 'A' || line[4] > 'Z' || line[5] < 'A' || line[5] > 'Z') return {};
            country = line.substr(4);
        }
        if (end == std::string_view::npos) break;
        text.remove_prefix(end + 1);
    }
    return address && country != "XX" ? country : std::string{};
}
} // namespace clashflux::website
