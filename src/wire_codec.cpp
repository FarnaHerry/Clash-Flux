#include "wire_codec.h"

#include <glaze/json.hpp>
#include <utility>

namespace clashflux::wire::codec_detail {

struct ReadOptions : glz::opts {
    bool validate_trailing_whitespace = true;
    bool validate_skipped = true;
    constexpr ReadOptions() {
        error_on_unknown_keys = false; // Runtime API projection, not config import.
        null_terminated = false;
    }
};

template <class T> Result<T> decode(std::string_view text) {
    Result<T> result;
    const auto error = glz::read<ReadOptions{}>(result.value, text);
    if (error) result.error = glz::format_error(error, text);
    return result;
}

template <class T> Result<std::string> encode(const T& value) {
    Result<std::string> result;
    if (const auto error = glz::write_json(value, result.value)) {
        result.error = glz::format_error(error, result.value);
    }
    return result;
}

struct VersionInput { std::optional<std::string> version; };
struct ReleaseInput {
    std::optional<std::string> tag_name;
    std::optional<bool> draft, prerelease;
    std::vector<glz::raw_json> assets;
};
struct ReleaseAssetNameInput { std::string name; };
struct TrafficInput { std::optional<std::int64_t> up, down; };
struct RuntimeRuleInput { std::optional<std::string> type, payload, proxy; };
struct RuntimeRulesInput { std::optional<std::vector<RuntimeRuleInput>> rules; };
struct ConnectionIdsInput { std::optional<std::vector<glz::raw_json>> connections; };
struct TotalsInput { std::optional<std::int64_t> uploadTotal, downloadTotal; };

struct ConnectionMetadata {
    std::string host;
    std::string type;
    std::string inboundType;
    std::string inbound;
    std::string destinationIP;
    std::string destination;
    std::string destinationPort;
    std::string network;
};
struct ConnectionInput {
    std::string id;
    std::string outboundType;
    std::int64_t upload = 0;
    std::int64_t download = 0;
    ConnectionMetadata metadata;
    std::string destination;
    std::string network;
    std::vector<glz::raw_json> chains;
    std::string chain;
    std::string rule;
    std::string rulePayload;
    std::string start;
};
struct ConnectionsInput {
    std::int64_t uploadTotal = 0;
    std::int64_t downloadTotal = 0;
    std::vector<glz::raw_json> connections;
};
struct ProxyInput {
    std::string type;
    std::string now;
    std::optional<bool> selectable;
    bool udp = false;
    std::optional<std::vector<glz::raw_json>> all;
    std::vector<glz::raw_json> history;
    std::optional<int> urlTestDelay;
    std::int64_t urlTestTime = 0;
};
struct ProxiesInput {
    std::map<std::string, glz::raw_json> proxies;
};
struct DelayInput { int delay = 0; };
struct HistoryInput {
    std::string time;
    int delay = 0;
};
struct MessageInput { std::string message; };
struct SelectionInput { std::string name; };
struct StoredLogInput {
    std::int64_t at = 0;
    std::string level = "info";
    std::optional<std::string> payload;
};

} // namespace clashflux::wire::codec_detail

// Persisted and outgoing contracts use explicit key lists. UI-only projections
// can evolve without accidentally renaming serialized policy/log keys.
namespace glz {
template <> struct meta<clashflux::network::Advanced> {
    using T = clashflux::network::Advanced;
    static constexpr auto value = object(
        "auto_redirect", &T::auto_redirect,
        "override_filters", &T::override_filters,
        "interface_mode", &T::interface_mode,
        "interfaces", &T::interfaces,
        "uid_mode", &T::uid_mode,
        "uids", &T::uids,
        "mac_mode", &T::mac_mode,
        "macs", &T::macs,
        "override_route_sets", &T::override_route_sets,
        "route_sets", &T::route_sets,
        "exclude_sets", &T::exclude_sets,
        "proxy_mode", &T::proxy_mode,
        "pac_port", &T::pac_port,
        "pac_url", &T::pac_url,
        "pac_direct_domains", &T::pac_direct_domains);
};
template <> struct meta<clashflux::network::Settings> {
    using T = clashflux::network::Settings;
    static constexpr auto value = object(
        "format_version", &T::format_version,
        "revision", &T::revision,
        "mixed_port", &T::mixed_port,
        "http_port", &T::http_port,
        "socks_port", &T::socks_port,
        "allow_lan", &T::allow_lan,
        "ipv6", &T::ipv6,
        "mtu", &T::mtu,
        "override_mtu", &T::override_mtu,
        "strict_route", &T::strict_route,
        "stack", &T::stack,
        "dns_mode", &T::dns_mode,
        "interface_name", &T::interface_name,
        "addresses", &T::addresses,
        "override_routes", &T::override_routes,
        "override_exclusions", &T::override_exclusions,
        "routes", &T::routes,
        "exclusions", &T::exclusions,
        "udp_timeout", &T::udp_timeout,
        "udp_mapping", &T::udp_mapping,
        "udp_filtering", &T::udp_filtering,
        "udp_nat_max", &T::udp_nat_max,
        "override_udp_nat_max", &T::override_udp_nat_max,
        "package_mode", &T::package_mode,
        "packages", &T::packages,
        "listen_address", &T::listen_address,
        "bind_interface", &T::bind_interface,
        "override_ports", &T::override_ports,
        "override_listen", &T::override_listen,
        "allowed_sources", &T::allowed_sources,
        "override_auth", &T::override_auth,
        "username", &T::username,
        "password", &T::password,
        "proxy_target", &T::proxy_target,
        "override_bypass", &T::override_bypass,
        "restore_proxy", &T::restore_proxy,
        "bypass", &T::bypass,
        "advanced", &T::advanced);
};
template <> struct meta<clashflux::network::ProxyValue> {
    using T = clashflux::network::ProxyValue;
    static constexpr auto value = object(
        "scope", &T::scope,
        "key", &T::key,
        "value", &T::value,
        "present", &T::present);
};
template <> struct meta<clashflux::network::ProxySnapshot> {
    using T = clashflux::network::ProxySnapshot;
    static constexpr auto value = object(
        "desktop", &T::desktop,
        "values", &T::values);
};
template <> struct meta<clashflux::network::ProxyLease> {
    using T = clashflux::network::ProxyLease;
    static constexpr auto value = object(
        "format_version", &T::format_version,
        "original", &T::original,
        "applied", &T::applied,
        "restore", &T::restore,
        "local_pac_url", &T::local_pac_url);
};

template <> struct meta<clashflux::wire::PersonalRuleSet> {
    using T = clashflux::wire::PersonalRuleSet;
    static constexpr auto value = object("id", &T::id, "name", &T::name,
        "type", &T::type, "format", &T::format, "url", &T::url,
        "content", &T::content, "update_hours", &T::update_hours,
        "refresh_generation", &T::refresh_generation);
};
template <> struct meta<clashflux::wire::RuleSetLibrary> {
    using T = clashflux::wire::RuleSetLibrary;
    static constexpr auto value = object("format_version", &T::format_version,
        "revision", &T::revision, "resources", &T::resources);
};
template <> struct meta<clashflux::wire::ProfileRuleEdit> {
    using T = clashflux::wire::ProfileRuleEdit;
    static constexpr auto value = object("id", &T::id, "operation", &T::operation,
        "anchor", &T::anchor, "enabled", &T::enabled, "match", &T::match,
        "pattern", &T::pattern, "target", &T::target, "unavailable", &T::unavailable);
};
template <> struct meta<clashflux::wire::ProfileRuleEdits> {
    using T = clashflux::wire::ProfileRuleEdits;
    static constexpr auto value = object("format_version", &T::format_version,
        "revision", &T::revision, "rules", &T::rules);
};
template <> struct meta<clashflux::wire::RouteRule> {
    using T = clashflux::wire::RouteRule;
    static constexpr auto value = object("match", &T::match, "pattern", &T::pattern,
                                        "connection_id", &T::connection_id, "priority", &T::priority,
        "id", &T::id, "tier", &T::tier, "enabled", &T::enabled,
        "unavailable", &T::unavailable, "target_kind", &T::target_kind,
        "target_object", &T::target_object, "order", &T::order);
};
template <> struct meta<clashflux::wire::Policy> {
    using T = clashflux::wire::Policy;
    static constexpr auto value = object("default_main_id", &T::default_main_id, "rules", &T::rules,
        "format_version", &T::format_version, "revision", &T::revision);
};
template <> struct meta<clashflux::wire::StoredLog> {
    using T = clashflux::wire::StoredLog;
    static constexpr auto value = object("at", &T::at, "level", &T::level, "payload", &T::payload);
};
template <> struct meta<clashflux::wire::codec_detail::SelectionInput> {
    using T = clashflux::wire::codec_detail::SelectionInput;
    static constexpr auto value = object("name", &T::name);
};
} // namespace glz

namespace clashflux::wire {

namespace {
template<class T> Result<T> DecodeNetworkValue(std::string_view text) {
    struct Options : glz::opts {
        bool validate_trailing_whitespace = true;
        constexpr Options() { null_terminated = false; error_on_unknown_keys = true; error_on_missing_keys = true; }
    };
    Result<T> result;
    if (const auto error = glz::read<Options{}>(result.value, text))
        result.error = "网络设置数据无效，保留原数据";
    else if (result.value.format_version != 1)
        result.error = "不支持的网络设置版本，保留原数据";
    return result;
}

// Clash-compatible /proxies only exposes the latest URL test as
// history[].time (RFC3339Nano). Android's bridge adds a numeric urlTestTime,
// but desktop must derive the same monotonic version from history or every
// poll compares 0 against 0 and the UI waits for the whole batch.
std::int64_t ParseRfc3339Nanos(std::string_view text) {
    const auto number = [](std::string_view value) {
        std::int64_t result = 0;
        for (const char c : value) {
            if (c < '0' || c > '9') return std::int64_t{-1};
            result = result * 10 + (c - '0');
        }
        return result;
    };
    if (text.size() < 20 || text[4] != '-' || text[7] != '-' ||
        (text[10] != 'T' && text[10] != 't') || text[13] != ':' || text[16] != ':')
        return 0;
    const auto year = number(text.substr(0, 4));
    const auto month = number(text.substr(5, 2));
    const auto day = number(text.substr(8, 2));
    const auto hour = number(text.substr(11, 2));
    const auto minute = number(text.substr(14, 2));
    const auto second = number(text.substr(17, 2));
    if (year < 1970 || year > 2261 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 60)
        return 0;
    std::size_t position = 19;
    std::int64_t nanos = 0;
    if (position < text.size() && text[position] == '.') {
        ++position;
        int digits = 0;
        while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
            if (digits < 9) {
                nanos = nanos * 10 + (text[position] - '0');
                ++digits;
            }
            ++position;
        }
        if (digits == 0) return 0;
        for (; digits < 9; ++digits) nanos *= 10;
    }
    std::int64_t offsetSeconds = 0;
    if (position < text.size() && (text[position] == '+' || text[position] == '-')) {
        if (position + 6 != text.size() || text[position + 3] != ':') return 0;
        const auto offsetHour = number(text.substr(position + 1, 2));
        const auto offsetMinute = number(text.substr(position + 4, 2));
        if (offsetHour < 0 || offsetHour > 23 || offsetMinute < 0 || offsetMinute > 59)
            return 0;
        offsetSeconds = (offsetHour * 60 + offsetMinute) * 60;
        if (text[position] == '-') offsetSeconds = -offsetSeconds;
        position += 6;
    } else if (position < text.size() && (text[position] == 'Z' || text[position] == 'z')) {
        ++position;
    } else {
        return 0;
    }
    if (position != text.size()) return 0;
    // Howard Hinnant's days_from_civil, kept local to avoid a time-zone
    // dependent std::mktime call in the codec.
    const std::int64_t adjustedYear = year - (month <= 2 ? 1 : 0);
    const std::int64_t era = (adjustedYear >= 0 ? adjustedYear : adjustedYear - 399) / 400;
    const std::int64_t yearOfEra = adjustedYear - era * 400;
    const std::int64_t dayOfYear =
        (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const std::int64_t dayOfEra =
        yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    const std::int64_t days = era * 146097 + dayOfEra - 719468;
    const std::int64_t seconds =
        days * 86400 + hour * 3600 + minute * 60 + second - offsetSeconds;
    return seconds * 1000000000 + nanos;
}
}
Result<network::Settings> DecodeNetworkSettings(std::string_view text) {
    return DecodeNetworkValue<network::Settings>(text);
}
Result<std::string> EncodeNetworkSettings(const network::Settings& value) { return codec_detail::encode(value); }
Result<network::ProxyLease> DecodeProxyLease(std::string_view text) { return DecodeNetworkValue<network::ProxyLease>(text); }
Result<std::string> EncodeProxyLease(const network::ProxyLease& value) { return codec_detail::encode(value); }

Result<std::string> DecodeVersion(std::string_view text) {
    auto input = codec_detail::decode<codec_detail::VersionInput>(text);
    if (!input) return {{}, std::move(input.error)};
    if (!input.value.version || input.value.version->empty()) return {{}, "missing version"};
    return {std::move(*input.value.version), {}};
}
Result<Release> DecodeStableRelease(std::string_view text) {
    auto input = codec_detail::decode<codec_detail::ReleaseInput>(text);
    if (!input) return {{}, std::move(input.error)};
    const auto& release = input.value;
    if (!release.tag_name || release.tag_name->empty() ||
        !release.draft.has_value() || !release.prerelease.has_value() ||
        *release.draft || *release.prerelease)
        return {{}, "missing or non-stable release"};
    Release result{std::move(*input.value.tag_name), {}};
    for (const auto& raw : release.assets) {
        auto asset = codec_detail::decode<ReleaseAsset>(raw.str);
        // Older assets may have a null digest. Keep version checking available,
        // but never offer such an asset for automatic installation.
        if (asset) result.assets.push_back(std::move(asset.value));
        else if (auto name = codec_detail::decode<codec_detail::ReleaseAssetNameInput>(raw.str); name)
            result.assets.push_back({std::move(name.value.name)});
    }
    return {std::move(result), {}};
}
Result<Traffic> DecodeTraffic(std::string_view text) {
    auto input = codec_detail::decode<codec_detail::TrafficInput>(text);
    if (!input) return {{}, std::move(input.error)};
    if (!input.value.up || !input.value.down || *input.value.up < 0 || *input.value.down < 0)
        return {{}, "missing or invalid traffic counters"};
    return {{*input.value.up, *input.value.down}, {}};
}
Result<Log> DecodeLog(std::string_view text) {
    return codec_detail::decode<Log>(text);
}
Result<StoredLog> DecodeStoredLog(std::string_view text) {
    auto input = codec_detail::decode<codec_detail::StoredLogInput>(text);
    Result<StoredLog> result;
    if (!input) result.error = std::move(input.error);
    else if (!input.value.payload) result.error = "日志缺少 payload";
    else result.value = {input.value.at, std::move(input.value.level), std::move(*input.value.payload)};
    return result;
}
Result<std::string> EncodeStoredLog(const StoredLog& log) {
    return codec_detail::encode(log);
}
Result<ConnectionTotals> DecodeConnectionTotals(std::string_view text) {
    auto input = codec_detail::decode<codec_detail::TotalsInput>(text);
    if (!input) return {{}, std::move(input.error)};
    if (!input.value.uploadTotal || !input.value.downloadTotal || *input.value.uploadTotal < 0 || *input.value.downloadTotal < 0)
        return {{}, "missing or invalid connection totals"};
    return {{*input.value.uploadTotal, *input.value.downloadTotal}, {}};
}
Result<std::vector<RuntimeRule>> DecodeRuntimeRules(std::string_view text) {
    auto input = codec_detail::decode<codec_detail::RuntimeRulesInput>(text);
    if (!input) return {{}, std::move(input.error)};
    if (!input.value.rules) return {{}, "missing runtime rules"};
    std::vector<RuntimeRule> rules;
    rules.reserve(input.value.rules->size());
    for (auto& rule : *input.value.rules) {
        if (!rule.type || rule.type->empty() || !rule.payload || !rule.proxy || rule.proxy->empty())
            return {{}, "invalid runtime rule"};
        rules.push_back({std::move(*rule.type), std::move(*rule.payload), std::move(*rule.proxy)});
    }
    return {std::move(rules), {}};
}
Result<std::vector<std::string>> DecodeConnectionIds(std::string_view text) {
    const auto input = codec_detail::decode<codec_detail::ConnectionIdsInput>(text);
    if (!input) return {{}, input.error};
    if (!input.value.connections) return {{}, "missing connections"};
    const auto connections = DecodeConnections(text);
    if (!connections || connections.ignoredEntries) return {{}, "invalid baseline connections"};
    std::vector<std::string> result;
    for (const auto& entry : connections.value.entries) {
        if (entry.id.empty()) return {{}, "missing baseline connection ID"};
        result.push_back(entry.id);
    }
    return {std::move(result), {}};
}
Result<Connections> DecodeConnections(std::string_view text) {
    Result<Connections> result;
    auto input = codec_detail::decode<codec_detail::ConnectionsInput>(text);
    if (!input) { result.error = std::move(input.error); return result; }
    result.value.uploadTotal = input.value.uploadTotal;
    result.value.downloadTotal = input.value.downloadTotal;
    result.value.entries.reserve(input.value.connections.size());
    for (const auto& raw : input.value.connections) {
        auto entry = codec_detail::decode<codec_detail::ConnectionInput>(raw.str);
        if (!entry) { ++result.ignoredEntries; continue; }
        auto& source = entry.value;
        Connection value;
        value.id = std::move(source.id);
        value.upload = source.upload;
        value.download = source.download;
        value.host = std::move(source.metadata.host);
        value.inboundType = std::move(source.metadata.inboundType);
        value.inbound = std::move(source.metadata.inbound);
        if (value.inboundType.empty()) {
            const auto separator = source.metadata.type.find('/');
            value.inboundType = source.metadata.type.substr(0, separator);
            if (separator != std::string::npos)
                value.inbound = source.metadata.type.substr(separator + 1);
        }
        value.outboundType = std::move(source.outboundType);
        value.destinationIP = std::move(source.metadata.destinationIP);
        value.destination = source.metadata.destination.empty()
            ? std::move(source.destination) : std::move(source.metadata.destination);
        value.destinationPort = std::move(source.metadata.destinationPort);
        value.network = source.metadata.network.empty()
            ? std::move(source.network) : std::move(source.metadata.network);
        for (const auto& hop : source.chains) {
            auto name = codec_detail::decode<std::string>(hop.str);
            if (name) value.chains.push_back(std::move(name.value));
        }
        value.chain = std::move(source.chain);
        value.rule = std::move(source.rule);
        value.rulePayload = std::move(source.rulePayload);
        value.start = std::move(source.start);
        result.value.entries.push_back(std::move(value));
    }
    return result;
}
Result<Proxies> DecodeProxies(std::string_view text) {
    Result<Proxies> result;
    auto input = codec_detail::decode<codec_detail::ProxiesInput>(text);
    if (!input) { result.error = std::move(input.error); return result; }
    for (const auto& [name, raw] : input.value.proxies) {
        auto entry = codec_detail::decode<codec_detail::ProxyInput>(raw.str);
        if (!entry) { ++result.ignoredEntries; continue; }
        auto& source = entry.value;
        Proxy value;
        value.type = std::move(source.type);
        value.now = std::move(source.now);
        value.selectable = source.selectable;
        value.udp = source.udp;
        if (source.all) {
            value.members.emplace();
            for (const auto& rawMember : *source.all) {
                auto member = codec_detail::decode<std::string>(rawMember.str);
                if (member) value.members->push_back(std::move(member.value));
            }
        }
        int historyDelay = 0;
        std::int64_t historyTime = 0;
        if (!source.history.empty()) {
            const auto history =
                codec_detail::decode<codec_detail::HistoryInput>(source.history.back().str);
            if (history) {
                historyDelay = history.value.delay;
                historyTime = ParseRfc3339Nanos(history.value.time);
            }
        }
        value.historyDelay = historyDelay;
        value.delay = source.urlTestDelay.value_or(historyDelay);
        value.testTime = source.urlTestTime != 0 ? source.urlTestTime : historyTime;
        result.value.entries.emplace(name, std::move(value));
    }
    return result;
}
Result<int> DecodeDelay(std::string_view text) {
    auto input = codec_detail::decode<codec_detail::DelayInput>(text);
    return {.value = input.value.delay, .error = std::move(input.error)};
}
Result<std::map<std::string, int>> DecodeGroupDelays(std::string_view text) {
    return codec_detail::decode<std::map<std::string, int>>(text);
}
Result<std::string> DecodeMessage(std::string_view text) {
    auto input = codec_detail::decode<codec_detail::MessageInput>(text);
    return {.value = std::move(input.value.message), .error = std::move(input.error)};
}
Result<std::map<std::string, std::string>> DecodeHeaders(std::string_view text) {
    return codec_detail::decode<std::map<std::string, std::string>>(text);
}
Result<Policy> DecodePolicy(std::string_view text) {
    // 持久化策略拒绝未知字段，不能套用运行时投影的跳过策略。
    struct PolicyOptions : glz::opts {
        bool validate_trailing_whitespace = true;
        constexpr PolicyOptions() { null_terminated = false; error_on_unknown_keys = true; }
    };
    Result<Policy> result;
    if (const auto error = glz::read<PolicyOptions{}>(result.value, text))
        result.error = glz::format_error(error, text);
    return result;
}
Result<std::string> EncodePolicy(const Policy& policy) {
    return codec_detail::encode(policy);
}
Result<ProfileRuleEdits> DecodeProfileRuleEdits(std::string_view text) {
    if (text.empty()) return {};
    struct Options : glz::opts {
        bool validate_trailing_whitespace = true;
        constexpr Options() { null_terminated = false; error_on_unknown_keys = true; }
    };
    Result<ProfileRuleEdits> result;
    if (const auto error = glz::read<Options{}>(result.value, text))
        result.error = glz::format_error(error, text);
    return result;
}
Result<std::string> EncodeProfileRuleEdits(const ProfileRuleEdits& edits) {
    return codec_detail::encode(edits);
}
Result<RuleSetLibrary> DecodeRuleSetLibrary(std::string_view text) {
    if (text.empty()) return {};
    struct Options : glz::opts {
        bool validate_trailing_whitespace = true;
        constexpr Options() { null_terminated = false; error_on_unknown_keys = true; }
    };
    Result<RuleSetLibrary> result;
    if (const auto error = glz::read<Options{}>(result.value, text))
        result.error = glz::format_error(error, text);
    return result;
}
Result<std::string> EncodeRuleSetLibrary(const RuleSetLibrary& library) {
    return codec_detail::encode(library);
}
Result<std::string> EncodeSelection(std::string_view name) {
    return codec_detail::encode(codec_detail::SelectionInput{std::string(name)});
}

} // namespace clashflux::wire
