#pragma once

#include <string>
#include <string_view>

namespace clashflux::ui {

// The catalog supplies original names. Never split user text on punctuation or
// alter runtime tags: labels are presentation only.
inline std::string ProxyObjectDisplayName(std::string_view objectName,
    std::string_view sourceName, bool qualifySource) {
    std::string label{objectName};
    if (qualifySource && !sourceName.empty()) label += " · " + std::string{sourceName};
    return label;
}

} // namespace clashflux::ui
