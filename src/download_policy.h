#pragma once

#include "http_request_headers.h"
#include <cstddef>
#include <array>
#include <optional>
#include <cstdlib>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace clashflux::download {

inline constexpr std::size_t kSubscriptionMaxBytes = 8 * 1024 * 1024;
inline constexpr long ResourceTimeout(std::size_t limit) { return limit > kSubscriptionMaxBytes ? 120 : 20; }
inline constexpr unsigned ResourceAttempts(std::size_t limit, bool android) {
    return android && limit <= kSubscriptionMaxBytes ? 2 : 1;
}

inline std::array<std::optional<std::string>, 11> TransportEnvironment() {
    constexpr std::array names{"http_proxy", "https_proxy", "all_proxy", "no_proxy",
        "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "NO_PROXY", "SSL_CERT_FILE", "SSL_CERT_DIR", "CURL_CA_BUNDLE"};
    std::array<std::optional<std::string>, 11> values;
    for (std::size_t i = 0; i < names.size(); ++i)
        if (const auto* value = std::getenv(names[i])) values[i] = value;
    return values;
}

// Complete transport headers, shared by source/rule/proxy requests. Validators
// belong to one verified representation; caller-supplied conditions are retained.
inline std::map<std::string, std::string> ConditionalHeaders(
        const std::map<std::string, std::string>& original,
        std::string_view etag, std::string_view modified) {
    for (const auto& [name, value] : original) {
        const auto lower = http_request::LowerHeaderName(name);
        if (lower == "if-none-match" || lower == "if-modified-since") return original;
    }
    auto headers = original;
    if (!etag.empty()) headers.emplace("if-none-match", etag);
    if (!modified.empty()) headers.emplace("if-modified-since", modified);
    return http_request::ValidHeaders(headers) ? headers : original;
}

inline bool HasCondition(const std::map<std::string, std::string>& headers) {
    for (const auto& [name, value] : headers) {
        const auto lower = http_request::LowerHeaderName(name);
        if (lower == "if-none-match" || lower == "if-modified-since") return true;
    }
    return false;
}

// Diagnostic token only; never an integrity check or a cache ownership key.
inline std::uint64_t DiagnosticId(std::string_view identity) {
    std::uint64_t value = 14695981039346656037ULL;
    for (const unsigned char c : identity) { value ^= c; value *= 1099511628211ULL; }
    return value;
}

} // namespace clashflux::download
