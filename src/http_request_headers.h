#pragma once

#include <map>
#include <set>
#include <string>
#include <string_view>

namespace clashflux::http_request {

inline std::string LowerHeaderName(std::string_view name) {
    std::string result(name);
    for (char& c : result) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return result;
}

// Shared by compiler and transports. This is deliberately the common single-
// value ASCII subset; do not silently flatten lists or let a platform suppress
// restricted transport headers. Diagnostics never contain names/credentials.
inline bool ValidHeaders(const std::map<std::string, std::string>& headers) {
    std::set<std::string> seen;
    std::size_t bytes = 0;
    for (const auto& [name, value] : headers) {
        if (name.empty() || value.empty()) return false;
        for (const unsigned char c : name)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos))
                return false;
        const auto lower = LowerHeaderName(name);
        if (!seen.insert(lower).second) return false;
        for (const auto restricted : {"host", "connection", "content-length", "transfer-encoding",
             "proxy-connection", "proxy-authorization", "keep-alive", "upgrade", "te", "trailer",
             "accept-encoding", "expect", "origin", "via", "access-control-request-headers",
             "access-control-request-method"})
            if (lower == restricted) return false;
        if (value.front() == ' ' || value.back() == ' ') return false;
        for (const unsigned char c : value) if (c < 32 || c > 126) return false;
        if (name.size() > 16384 || value.size() > 16384 ||
            name.size() + value.size() + 4 > 16384 - bytes) return false;
        bytes += name.size() + value.size() + 4;
    }
    return true;
}

} // namespace clashflux::http_request
