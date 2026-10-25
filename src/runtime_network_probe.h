#pragma once
#include <chrono>
#include <cstdint>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace clashflux::network_probe {
enum class Evidence { Unknown, Unsupported, NotChecked, Absent, Down, Present,
    AddressMismatch, ViaTun, OtherInterface, Response, NegativeResponse,
    ServerError, InvalidResponse, Timeout, Cancelled };
inline std::string_view Name(Evidence value) {
    switch (value) {
    case Evidence::Unknown: return "unknown";
    case Evidence::Unsupported: return "unsupported";
    case Evidence::NotChecked: return "not-checked";
    case Evidence::Absent: return "absent";
    case Evidence::Down: return "down";
    case Evidence::Present: return "present-address-matched";
    case Evidence::AddressMismatch: return "address-mismatch";
    case Evidence::ViaTun: return "via-tun";
    case Evidence::OtherInterface: return "other-interface";
    case Evidence::Response: return "response";
    case Evidence::NegativeResponse: return "negative-response";
    case Evidence::ServerError: return "server-error";
    case Evidence::InvalidResponse: return "invalid-response";
    case Evidence::Timeout: return "timeout";
    case Evidence::Cancelled: return "cancelled";
    }
    return "unknown";
}
enum class Proxy { Unknown, Unsupported, Disabled, Ours, Other };
inline std::string_view Name(Proxy value) {
    switch(value) {
    case Proxy::Unknown: return "unknown";
    case Proxy::Unsupported: return "unsupported";
    case Proxy::Disabled: return "disabled";
    case Proxy::Ours: return "matches-running-port";
    case Proxy::Other: return "other-or-per-protocol";
    }
    return "unknown";
}
struct Request {
    std::string interfaceName;
    std::string address; // Numeric IPv4 address of the accepted TUN, no prefix.
    std::string dnsAddress;
    std::string routeDestination = "192.0.2.1"; // Read-only routing-table sample, never contacted.
    bool dnsRoundTrip = false; // Explicit diagnostic only; never enabled by the background recorder.
    std::uint16_t dnsPort = 53;
    int mixedPort = 0;
};
struct Result {
    Evidence interface = Evidence::Unknown;
    Evidence route = Evidence::NotChecked;
    Evidence dnsRoute = Evidence::NotChecked;
    Evidence dns = Evidence::NotChecked;
    Proxy proxy = Proxy::Unknown;
    bool operator==(const Result&) const = default;
};
// No settings/routes/proxy mutations, hostname resolution, shell commands or raw
// configuration in output. DNS uses a reserved .invalid name on the matched TUN.
Result Inspect(const Request&, std::stop_token cancel = {},
    std::chrono::milliseconds budget = std::chrono::milliseconds(1200));
std::vector<std::uint8_t> DnsQuestion(std::uint16_t id);
Evidence ValidateDns(const std::vector<std::uint8_t>& question,
                     const std::vector<std::uint8_t>& response);
} // namespace clashflux::network_probe
