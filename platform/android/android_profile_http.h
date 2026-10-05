#pragma once

#include <map>
#include <cstddef>
#include <string>

namespace clashflux::android {

struct ProfileHttpResponse {
    long status = 0;
    bool retryable = false;
    std::string error;
    std::multimap<std::string, std::string> headers;
    std::string body;
};

// Synchronous app-specific transport. Call from a HuxerUI worker thread; the
// Java implementation scopes the optional trust override to this request.
ProfileHttpResponse DownloadProfile(const std::string& url, int timeoutSecs,
                                    bool allowInvalidCertificate, std::size_t maxBytes = 0,
                                    bool forceDirect = false,
                                    const std::map<std::string, std::string>& headers = {});

} // namespace clashflux::android
