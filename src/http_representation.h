#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace clashflux::http_cache {

inline bool ValidValidator(std::string_view value) {
    if (value.size() > 2048 || (!value.empty() && (value.front() == ' ' || value.back() == ' '))) return false;
    for (const unsigned char c : value) if (c < 32 || c > 126) return false;
    return true;
}

struct Representation { std::string body, etag, modified; };

inline std::string Encode(std::string_view kind, std::string_view identity, const std::string& body,
                          std::string_view etag = {}, std::string_view modified = {}) {
    if (identity.size() > 32 * 1024 || identity.find('\n') != std::string_view::npos ||
        !ValidValidator(etag) || !ValidValidator(modified)) return {};
    const bool conditional = !etag.empty() || !modified.empty();
    auto image = std::string(kind) + (conditional ? "-v2\n" : "-v1\n") + std::string(identity) + "\n";
    if (conditional) image += std::string(etag) + "\n" + std::string(modified) + "\n";
    return image + body;
}

// Identity and version are checked before any body is exposed. Unknown versions
// never get repaired or overwritten by callers using this cache as an authority.
inline std::optional<Representation> Decode(const std::string& image, std::string_view kind,
        std::string_view identity, std::size_t limit) {
    const auto v1 = std::string(kind) + "-v1\n" + std::string(identity) + "\n";
    const auto v2 = std::string(kind) + "-v2\n" + std::string(identity) + "\n";
    Representation result;
    std::size_t offset = v1.size();
    if (image.starts_with(v2)) {
        offset = v2.size();
        for (auto* value : {&result.etag, &result.modified}) {
            const auto end = image.find('\n', offset);
            if (end == std::string::npos || end - offset > 2048) return {};
            *value = image.substr(offset, end - offset); offset = end + 1;
            if (!ValidValidator(*value)) return {};
        }
    } else if (!image.starts_with(v1)) return {};
    if (offset > image.size() || image.size() - offset > limit) return {};
    result.body = image.substr(offset);
    return result;
}

} // namespace clashflux::http_cache
