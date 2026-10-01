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

struct ConnectionMetadata {
    std::string host;
    std::string destinationIP;
    std::string destination;
    std::string destinationPort;
    std::string network;
};
struct ConnectionInput {
    std::string id;
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

Result<Traffic> DecodeTraffic(std::string_view text) {
    return codec_detail::decode<Traffic>(text);
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
    return codec_detail::decode<ConnectionTotals>(text);
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
        if (!source.history.empty()) {
            const auto history = codec_detail::decode<codec_detail::DelayInput>(source.history.back().str);
            if (history) value.historyDelay = history.value.delay;
        }
        value.delay = source.urlTestDelay.value_or(value.historyDelay);
        value.testTime = source.urlTestTime;
        result.value.entries.emplace(name, std::move(value));
    }
    return result;
}
Result<int> DecodeDelay(std::string_view text) {
    auto input = codec_detail::decode<codec_detail::DelayInput>(text);
    return {.value = input.value.delay, .error = std::move(input.error)};
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
Result<std::string> EncodeSelection(std::string_view name) {
    return codec_detail::encode(codec_detail::SelectionInput{std::string(name)});
}

} // namespace clashflux::wire
