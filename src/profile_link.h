#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace clashflux::profile_link {

struct RemoteProfile {
    std::string url;
    std::string name;
    bool operator==(const RemoteProfile&) const = default;
};

bool IsSupportedScheme(std::string_view scheme);
std::optional<RemoteProfile> ParseParts(std::string_view scheme,
    std::string_view authority, std::string_view path, std::string_view query,
    std::string_view fragment, std::string& error);
std::optional<RemoteProfile> Parse(std::string_view value, std::string& error);
bool IsHttpUrl(std::string_view value);

// Entry points and IPC enqueue owned data; only the application's UI thread
// publishes it into the shared ProfilesModel. No persistence outside Runtime.
bool Submit(RemoteProfile profile, std::string& error);
std::vector<RemoteProfile> TakePending();
void SetWakeHandler(std::function<void()> handler);

} // namespace clashflux::profile_link
