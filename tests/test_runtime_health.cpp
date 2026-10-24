#include "../src/runtime_health.h"
#include "../src/wire_codec.h"
#include "runtime_health_cases.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
import clashflux.stream;
import clashflux.api;
using namespace std::chrono_literals;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Predicate> void until(Predicate predicate, std::chrono::seconds budget = 20s) {
    const auto end = std::chrono::steady_clock::now() + budget;
    while (!predicate()) {
        require(std::chrono::steady_clock::now() < end, "timed out awaiting production stream evidence");
        std::this_thread::sleep_for(50ms);
    }
}
int main(int argc, char** argv) {
    testRuntimeHealth();
    using namespace clashflux::health;
    require(!clashflux::wire::DecodeTraffic("{}"), "missing traffic counters accepted");
    require(!clashflux::wire::DecodeTraffic("{\"up\":-1,\"down\":0}"), "negative counter accepted");
    require(!clashflux::wire::DecodeConnectionTotals("{\"uploadTotal\":0}"), "missing totals accepted");
    require(!clashflux::wire::DecodeVersion("{}"), "invalid version accepted");
    require(argc == 5, "controller, mixed port, target and TLS target required");
    const std::string base = argv[1];
    const int mixed = std::stoi(argv[2]);
    const std::string target = argv[3];
    require(api::probeThroughProxy(target, mixed, {}).ok, "explicit target failed through real mixed inbound");
    const auto denied = api::probeThroughProxy(target + "/denied", mixed, {});
    require(!denied.ok && denied.status == 403, "HTTP denial reported healthy");
    require(api::probeThroughProxy("file:///etc/passwd", mixed, {}).failurePhase == "invalid-target-or-port", "non HTTP target accepted");
    const auto tls = api::probeThroughProxy(argv[4], mixed, {});
    require(!tls.ok && tls.failurePhase == "tls", "untrusted TLS certificate not rejected");
    std::stop_source cancellation;
    std::jthread cancel([&] { std::this_thread::sleep_for(150ms); cancellation.request_stop(); });
    auto began = std::chrono::steady_clock::now();
    auto cancelled = api::probeThroughProxy(target + "/slow", mixed, cancellation.get_token());
    require(cancelled.failurePhase == "cancelled" && std::chrono::steady_clock::now() - began < 3s, "in-flight diagnostic cancellation failed");
    stream::CoreStreams streams;
    streams.start(base + "/stall", "health-fixture-token", "info");
    std::cerr << "await initial samples\n";
    until([&] { return streams.health().traffic.state == State::Fresh && streams.health().connections.state == State::Fresh; });
    stream::TrafficPoint point;
    require(streams.takeTraffic(point) && point.up == 0 && point.down == 0, "valid zero frame lost");
    const auto initialEpoch = streams.health().runtimeEpoch;
    std::cerr << "await repaired samples\n";
    until([&] { return streams.health().traffic.repairs == 1 && streams.health().traffic.state == State::Fresh; });
    const auto recovered = streams.health();
    require(recovered.runtimeEpoch == initialEpoch && recovered.connections.repairs == 0, "repair affected other stream or runtime");
    require(recovered.traffic.invalid > 0 && recovered.traffic.valid > recovered.traffic.consumed, "sample/consumer evidence missing");
    std::int64_t up, down;
    require(streams.takeConnectionTotals(up, down) && up == 42 && down == 84, "repair reset authoritative totals");
    {
        auto paused = streams.pauseHealth();
        require(streams.health().traffic.state == State::Paused, "maintenance pause not exposed");
    }
    streams.stop();
    require(streams.health().traffic.state == State::Paused, "stopped stream not paused");
    for (const auto& scenario : {"auth", "forbidden", "unsupported"}) {
        streams.start(base + "/" + scenario, "health-fixture-token", "info");
        std::cerr << "await terminal " << scenario << "\n";
        until([&] { auto state = streams.health().traffic.state; return state == State::Blocked || state == State::Unsupported; }, 5s);
        const auto generation = streams.health().traffic.generation;
        std::this_thread::sleep_for(1200ms);
        require(streams.health().traffic.generation == generation && streams.health().traffic.repairs == 0, "terminal error retried");
        streams.stop();
    }
    streams.start(base + "/timeout", "health-fixture-token", "info");
    std::cerr << "await timeout probe\n";
    until([&] { return streams.health().traffic.repairs != 0; });
    const auto stopAt = std::chrono::steady_clock::now();
    streams.stop(); // probe is in flight; cannot resurrect this instance
    require(std::chrono::steady_clock::now() - stopAt < 2s, "stop blocked on normal 10s API timeout");
    const auto stoppedEpoch = streams.health().runtimeEpoch;
    streams.start(base + "/timeout", "health-fixture-token", "info"); // endpoint/secret ABA
    until([&] { return streams.health().traffic.state == State::Fresh; }, 5s);
    std::this_thread::sleep_for(1600ms);
    require(streams.health().runtimeEpoch != stoppedEpoch && streams.health().control == Control::Unknown,
        "old probe contaminated replacement instance");
    streams.stop();
    std::this_thread::sleep_for(1100ms);
    require(streams.health().traffic.state == State::Paused && !streams.trafficOpen(), "old probe revived stopped stream");
    std::cout << "runtime_health: production WebSockets, malformed frames, isolated recovery, terminal errors, stop/probe race passed\n";
}
