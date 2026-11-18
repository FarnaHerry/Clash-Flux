#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <string_view>

#include "../src/wire_codec.h"

int main() {
    using namespace clashflux::wire;
    // Desktop /proxies exposes the latest URL test only as history[].time.
    // Android additionally sends a numeric urlTestTime; both must produce a
    // usable monotonic testTime for the proxy page's incremental display.
    constexpr std::string_view desktop = R"({"proxies":{
        "a":{"type":"Shadowsocks","history":[{"time":"2026-11-06T07:12:34.123456789+08:00","delay":123}]},
        "b":{"type":"Shadowsocks","history":[{"time":"2026-11-06T07:12:35.123456789+08:00","delay":124}]},
        "c":{"type":"Shadowsocks","history":[{"time":"2026-11-05T23:12:34.123456789Z","delay":0}]},
        "d":{"type":"Shadowsocks","urlTestTime":456,"urlTestDelay":321,"history":[]}
    }})";
    const auto decoded = DecodeProxies(desktop);
    assert(decoded);
    const auto& a = decoded.value.entries.at("a");
    const auto& b = decoded.value.entries.at("b");
    const auto& c = decoded.value.entries.at("c");
    const auto& d = decoded.value.entries.at("d");
    assert(a.delay == 123 && a.testTime > 0);
    assert(b.testTime > a.testTime);
    assert(c.testTime == a.testTime); // Same instant, different RFC3339 offset.
    assert(d.testTime == 456 && d.delay == 321);

    const auto invalid = DecodeProxies(R"({"proxies":{"e":{
        "type":"Shadowsocks","history":[{"time":"not-a-time","delay":7}]}}})");
    assert(invalid);
    const auto& e = invalid.value.entries.at("e");
    assert(e.delay == 7 && e.testTime == 0);

    const auto foreign = DecodePublicIpLookup(
        R"({"ip":"43.216.255.247","country":"MY"})");
    assert(foreign && foreign.value.ip == "43.216.255.247" &&
           foreign.value.countryCode == "MY");
    const auto domestic = DecodePublicIpLookup(
        R"({"code":200,"data":{"ip":"1.30.213.166","countryCode":"CN"}})");
    assert(domestic && domestic.value.ip == "1.30.213.166" &&
           domestic.value.countryCode == "CN");
    assert(!DecodePublicIpLookup(R"({"ip":"1.2.3.999","country":"CN"})"));
    assert(!DecodePublicIpLookup(R"({"ip":"1.2.3.4"})"));
    assert(!DecodePublicIpLookup(R"({"ip":"1.2.3.4","country":"CHN"})"));
}
