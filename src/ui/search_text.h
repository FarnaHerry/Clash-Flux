#pragma once

#include <algorithm>
#include <cctype>
#include <string_view>

namespace clashflux::ui {

inline bool SearchTextMatches(std::string_view text, std::string_view query) {
    if (query.empty()) return true;
    const auto equalIgnoringAsciiCase = [](char left, char right) {
        return std::tolower(static_cast<unsigned char>(left)) ==
               std::tolower(static_cast<unsigned char>(right));
    };
    return std::search(text.begin(), text.end(), query.begin(), query.end(),
                       equalIgnoringAsciiCase) != text.end();
}

} // namespace clashflux::ui
