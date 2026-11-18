#pragma once

#include <string>
#include <string_view>

namespace clashflux::ui {

// ISO 3166-1 alpha-2 → regional-indicator flag emoji. Unknown input uses a
// globe so the card never claims a country that the API did not return.
inline std::string CountryFlagEmoji(std::string_view code) {
    if (code.size() != 2) return "🌐";
    std::string flag;
    flag.reserve(8);
    for (char value : code) {
        if (value >= 'a' && value <= 'z') value = static_cast<char>(value - 'a' + 'A');
        if (value < 'A' || value > 'Z') return "🌐";
        const char32_t codepoint = 0x1F1E6 + static_cast<char32_t>(value - 'A');
        flag.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        flag.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        flag.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        flag.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    return flag;
}

} // namespace clashflux::ui
