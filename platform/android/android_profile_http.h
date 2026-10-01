#pragma once

#include <map>
#include <string>

namespace clashflux::android {

struct ProfileHttpResponse {
    long status = 0;
    std::string error;
    std::multimap<std::string, std::string> headers;
    std::string body;
};

// Synchronous app-specific transport. Call from a HuxerUI worker thread; the
// Java implementation scopes the optional trust override to this request.
ProfileHttpResponse DownloadProfile(const std::string& url, int timeoutSecs,
                                    bool allowInvalidCertificate);

} // namespace clashflux::android
