#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Owning wire values shared by the module and Android legacy builds. No JSON
// library types escape this boundary. JSON decoding is independent of the UI.
namespace clashflux::wire {

template <class T> struct Result {
    T value{};
    std::string error;
    std::size_t ignoredEntries = 0;
    explicit operator bool() const noexcept { return error.empty(); }
};

struct Traffic {
    std::int64_t up = 0;
    std::int64_t down = 0;
};
struct Log {
    std::string type = "info";
    std::string payload;
};
struct StoredLog {
    std::int64_t at = 0;
    std::string level = "info";
    std::string payload;
};
struct Connection {
    std::string id;
    std::string host;
    std::string destinationIP;
    std::string destination;
    std::string destinationPort;
    std::string network;
    std::vector<std::string> chains;
    std::string chain;
    std::string rule;
    std::string rulePayload;
    std::string start;
    std::int64_t upload = 0;
    std::int64_t download = 0;
};
struct Connections {
    std::int64_t uploadTotal = 0;
    std::int64_t downloadTotal = 0;
    std::vector<Connection> entries;
};
struct ConnectionTotals {
    std::int64_t uploadTotal = 0;
    std::int64_t downloadTotal = 0;
};
struct Proxy {
    std::string type;
    std::string now;
    std::optional<bool> selectable;
    bool udp = false;
    std::optional<std::vector<std::string>> members;
    int historyDelay = 0;
    int delay = 0;
    std::int64_t testTime = 0;
    bool operator==(const Proxy&) const = default;
};
struct Proxies {
    std::map<std::string, Proxy> entries;
    bool operator==(const Proxies&) const = default;
};
struct RouteRule {
    std::string match;
    std::string pattern;
    std::string connection_id;
    int priority = 0;
    std::string id;
    std::string tier = "user_override";
    bool enabled = true;
    std::string unavailable = "reject";
    std::string target_kind = "default";
    std::string target_object;
    std::int64_t order = 0;
};
struct Policy {
    int format_version = 1;
    std::uint64_t revision = 0;
    std::string default_main_id;
    std::vector<RouteRule> rules;
};

Result<Traffic> DecodeTraffic(std::string_view text);
Result<Log> DecodeLog(std::string_view text);
Result<StoredLog> DecodeStoredLog(std::string_view text);
Result<std::string> EncodeStoredLog(const StoredLog& log);
Result<Connections> DecodeConnections(std::string_view text);
Result<ConnectionTotals> DecodeConnectionTotals(std::string_view text);
Result<Proxies> DecodeProxies(std::string_view text);
Result<int> DecodeDelay(std::string_view text);
Result<std::string> DecodeMessage(std::string_view text);
Result<std::map<std::string, std::string>> DecodeHeaders(std::string_view text);
Result<Policy> DecodePolicy(std::string_view text);
Result<std::string> EncodePolicy(const Policy& policy);
Result<std::string> EncodeSelection(std::string_view name);

} // namespace clashflux::wire
