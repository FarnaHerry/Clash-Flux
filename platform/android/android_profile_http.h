#pragma once

#include <map>
#include <cstddef>
#include <cstdint>
#include <string>

namespace clashflux::android {

struct ProfileHttpResponse {
    long status = 0;
    bool retryable = false;
    unsigned attempts = 1;
    std::string failure = "none";
    std::string error;
    std::multimap<std::string, std::string> headers;
    std::uint64_t bytes = 0;
};

// Synchronous app-specific transport. Call from a HuxerUI worker thread; the
// Java implementation always uses Android certificate and hostname verification.
ProfileHttpResponse DownloadProfileToFile(const std::string& url, int timeoutSecs,
    const std::string& destination, std::size_t maxBytes, bool forceDirect,
    const std::map<std::string, std::string>& headers = {}, bool subscriptionRequest = false);

} // namespace clashflux::android
