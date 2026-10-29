#pragma once

#include <charconv>
#include <cstdint>
#include <string_view>

namespace clashflux::subscription {

inline constexpr std::int64_t kNeverExpires = -1;

// Only call for a present, trimmed expire field. A missing field stays unknown (0).
inline std::int64_t ParseExpiryValue(std::string_view value) {
    if (value.empty()) return kNeverExpires;
    std::int64_t parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size() || parsed < -1) return 0;
    return parsed <= 0 ? kNeverExpires : parsed;
}

} // namespace clashflux::subscription
