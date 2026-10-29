#pragma once

#include <string>
#include <string_view>

namespace clashflux::ui {

template<class SourceObjects>
bool ShouldQualifyProxySources(const SourceObjects& objects) {
    std::string_view firstSource;
    for (const auto& object : objects) {
        if (object.sourceId.empty()) continue;
        if (firstSource.empty()) firstSource = object.sourceId;
        else if (object.sourceId != firstSource) return true;
    }
    return false;
}

// The catalog supplies original names. Never split user text on punctuation or
// alter runtime tags: labels are presentation only.
inline std::string ProxyObjectDisplayName(std::string_view objectName,
    std::string_view sourceName, bool qualifySource) {
    std::string label{objectName};
    if (qualifySource && !sourceName.empty()) label += " · " + std::string{sourceName};
    return label;
}

} // namespace clashflux::ui
