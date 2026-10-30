#include "../src/runtime_health.h"
#include "../src/wire_codec.h"
#include "runtime_health_cases.h"
#include <chrono>
#include <atomic>
#include <array>
#include <memory>
#include <set>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <filesystem>
#include <fstream>
import clashflux.stream;
import clashflux.api;
import clashflux.core;
import clashflux.singbox;
import clashflux.service;
using namespace std::chrono_literals;
void require(bool value, const char* message) {
    if (!value) {
        std::cerr << "runtime health assertion failed: " << message << '\n';
        throw std::runtime_error(message);
    }
}
template<class Predicate> void until(Predicate predicate, std::chrono::seconds budget = 20s) {
    const auto end = std::chrono::steady_clock::now() + budget;
    while (!predicate()) {
        require(std::chrono::steady_clock::now() < end, "timed out awaiting production stream evidence");
        std::this_thread::sleep_for(50ms);
    }
}
int main(int argc, char** argv) {
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
    {
        require(service::required(), "desktop core must require the shared service facade");
        core::CoreProcess process;
        require(!process.start({}, {}, {}) && !process.running() &&
                process.lastError() == "内核必须由系统网络服务托管",
                "application-owned core start must reject before creating a process");
        std::string error;
        require(!core::spawnDetached({}, {}, {}, error) &&
                error == "内核必须由系统网络服务托管",
                "CLI detached spawn must not bypass the desktop service owner");
    }
#endif
    if (argc == 4 && std::string_view(argv[1]) == "--rule-set-import") {
        try {
            const auto root = std::filesystem::absolute(argv[2]);
            const auto binary = std::filesystem::absolute(argv[3]);
            std::string error;
            const auto original = singbox::ReadRuleProviderText(root/"rules.srs", 8*1024*1024, error);
            require(original.has_value(), "missing SRS fixture");
            auto imported = core::importRuleSet(binary, root, root/"rules.srs", error);
            if (!imported || !error.empty()) std::cerr << "production SRS import error: " << error << '\n';
            require(imported.has_value() && error.empty(), "production SRS import failed");
            const auto unicodeSource = root/std::filesystem::path(u8"导入 规则.srs");
            const auto unicodeWork = root/std::filesystem::path(u8"暂存 work");
            std::filesystem::copy_file(root/"rules.srs", unicodeSource, std::filesystem::copy_options::overwrite_existing);
            auto unicodeImported = core::importRuleSet(binary, unicodeWork, unicodeSource, error);
            if (!unicodeImported || !error.empty()) std::cerr << "Unicode SRS import error: " << error << '\n';
            require(unicodeImported == imported && error.empty(),
                "native import failed with Unicode paths or spaces");
            require(std::filesystem::is_empty(unicodeWork), "Unicode staging directory was not cleaned");
            require(singbox::ReadRuleProviderText(root/"rules.srs", 8*1024*1024, error) == original, "import changed selected source file");
            singbox::CompileOptions options;
            options.profileYaml = "rules: ['MATCH,DIRECT']\n";
            options.ruleSetLibrary.resources.push_back({.id="imported", .name="Imported", .type="inline", .format="source", .content=*imported});
            options.profileRuleEdits.rules.push_back({.id="ref", .match="rule_set", .pattern="imported", .target="REJECT"});
            const auto compiled = singbox::compileConfig(options);
            require(compiled.error.empty() && !compiled.json.empty(), "imported resource cannot compile");
            std::ofstream(root/"personal-library-imported.json") << compiled.json;
            std::ofstream(root/"malformed.srs", std::ios::binary) << "SRS\x05not-zlib";
            require(!core::importRuleSet(binary, root, root/"malformed.srs", error) && !error.empty(), "malformed SRS accepted");
            require(!core::importRuleSet(binary, root, root/"missing.srs", error) && !error.empty(), "missing SRS accepted");
            std::ofstream(root/"oversized.srs", std::ios::binary) << std::string(8*1024*1024+1, 'x');
            require(!core::importRuleSet(binary, root, root/"oversized.srs", error) && !error.empty(), "oversized SRS accepted");
            const auto stagingPrefix = std::filesystem::path("rule-set-import-").native();
            for (const auto& entry : std::filesystem::directory_iterator(root))
                require(!entry.path().filename().native().starts_with(stagingPrefix), "import left a staging directory");
            std::cout << "production SRS import: valid roundtrip, malformed/missing/oversize rejection, source and staging preservation passed\n";
            return 0;
        } catch (const std::exception& exception) {
            std::cerr << "rule-set import regression exception: " << exception.what() << '\n';
            return 1;
        }
    }
    testRuntimeHealth();
    using namespace clashflux::health;
    require(!clashflux::wire::DecodeTraffic("{}"), "missing traffic counters accepted");
    require(!clashflux::wire::DecodeTraffic("{\"up\":-1,\"down\":0}"), "negative counter accepted");
    require(!clashflux::wire::DecodeConnectionTotals("{\"uploadTotal\":0}"), "missing totals accepted");
    require(!clashflux::wire::DecodeVersion("{}"), "invalid version accepted");
    require(argc == 6, "controller, mixed port, target, TLS target and native endpoint required");
    const std::string base = argv[1];
    const int mixed = std::stoi(argv[2]);
    const std::string target = argv[3];
    const std::string nativeBase = argv[5];
    api::NativeApi native(nativeBase, "health-fixture-token");
    const auto nativeInitial = native.runtime();
    require(nativeInitial.ok, "production native readiness failed");
    std::stop_source nativeCancel; nativeCancel.request_stop();
    require(native.runtime(nativeCancel.get_token()).failurePhase == "cancelled", "native cancellation ignored");
    {
        struct CopyFailure {
            std::shared_ptr<std::atomic<bool>> fail;
            std::shared_ptr<std::atomic<unsigned>> calls;
            CopyFailure(std::shared_ptr<std::atomic<bool>> failure, std::shared_ptr<std::atomic<unsigned>> count)
                : fail(std::move(failure)), calls(std::move(count)) {}
            CopyFailure(const CopyFailure& other) : fail(other.fail), calls(other.calls) {
                if (fail->exchange(false)) throw std::runtime_error("injected observer copy failure");
            }
            void operator()(stream::StreamKind kind) const {
                ++*calls;
                stream::acknowledgeStreamUpdate(kind);
            }
        };
        auto fail = std::make_shared<std::atomic<bool>>(false);
        auto calls = std::make_shared<std::atomic<unsigned>>(0);
        const auto id = stream::addStreamUpdateObserver(CopyFailure{fail, calls});
        *fail = true;
        stream::logApplication("debug", "runtime-health injected observer copy failure");
        stream::logApplication("debug", "runtime-health observer delivery retry");
        require(*calls == 1, "callback copy failure latched the notification");
        stream::removeStreamUpdateObserver(id);
    }
    {
        std::array<std::array<std::uint64_t, 32>, 4> ids{};
        std::vector<std::jthread> writers;
        for (unsigned worker = 0; worker < ids.size(); ++worker) writers.emplace_back([&, worker] {
            for (auto& id : ids[worker]) id = stream::addStreamUpdateObserver([](auto) {});
        });
        writers.clear();
        std::set<std::uint64_t> unique;
        for (const auto& group : ids) for (auto id : group) {
            require(id != 0 && unique.insert(id).second, "concurrent observer registration returned duplicate identity");
            stream::removeStreamUpdateObserver(id);
        }
    }
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
    began = std::chrono::steady_clock::now();
    const auto budget = api::probeThroughProxy(target + "/slow", mixed, {}, 200);
    require(!budget.ok && std::chrono::steady_clock::now() - began < 1s, "remaining diagnostic budget ignored");
    stream::CoreStreams streams;
    std::atomic<unsigned> notifications{0};
    const auto observer = stream::addStreamUpdateObserver([&](stream::StreamKind kind) {
        const bool consumed = stream::consumeStreamUpdate(kind, [&](stream::StreamKind event) {
            if (event == stream::StreamKind::Traffic && ++notifications == 1) throw std::runtime_error("injected consumer failure");
        });
        if (!consumed) require(notifications == 1, "unexpected consumer failure");
    });
    streams.start(base + "/stall", "health-fixture-token", "info", nativeBase);
    std::cerr << "await initial samples\n";
    until([&] { return streams.health().traffic.state == State::Fresh && streams.health().connections.state == State::Fresh; });
    stream::TrafficPoint point;
    require(streams.takeTraffic(point) && point.up == 0 && point.down == 0, "valid zero frame lost");
    require(notifications >= 2, "consumer exception latched notification delivery");
    const auto initialEpoch = streams.health().runtimeEpoch;
    std::cerr << "await repaired samples\n";
    until([&] { return streams.health().traffic.repairs == 1 && streams.health().traffic.state == State::Fresh; });
    const auto recovered = streams.health();
    require(recovered.runtimeEpoch == initialEpoch && recovered.connections.repairs == 0, "repair affected other stream or runtime");
    require(recovered.nativeControl == Control::Responsive && recovered.nativeStartedAt == nativeInitial.startedAt &&
        recovered.nativeAgeMs >= 0, "native evidence missing or wrong instance");
    require(recovered.traffic.invalid > 0 && recovered.traffic.valid > recovered.traffic.consumed, "sample/consumer evidence missing");
    std::int64_t up, down;
    require(streams.takeConnectionTotals(up, down) && up == 42 && down == 84, "repair reset authoritative totals");
    {
        auto paused = streams.pauseHealth();
        require(streams.health().traffic.state == State::Paused, "maintenance pause not exposed");
    }
    streams.stop();
    require(streams.health().traffic.state == State::Paused, "stopped stream not paused");
    streams.start(base + "/control-down", "health-fixture-token", "info", nativeBase);
    until([&] { return streams.health().control == Control::Unreachable; });
    require(streams.health().nativeControl == Control::Responsive &&
        streams.health().traffic.state == State::Stale, "Clash outage confused with native core exit");
    streams.stop();
    for (const auto& scenario : {"auth", "forbidden", "unsupported"}) {
        streams.start(base + "/" + scenario, "health-fixture-token", "info");
        std::cerr << "await terminal " << scenario << "\n";
        until([&] { auto state = streams.health().traffic.state; return state == State::Blocked || state == State::Unsupported; }, 5s);
        const auto generation = streams.health().traffic.generation;
        std::this_thread::sleep_for(1200ms);
        require(streams.health().traffic.generation == generation && streams.health().traffic.repairs == 0, "terminal error retried");
        streams.stop();
    }
    streams.start(base + "/pause-probe", "health-fixture-token", "info");
    until([&] { return streams.health().traffic.repairs != 0; });
    {
        auto paused = streams.pauseHealth();
        std::this_thread::sleep_for(1500ms); // old local probe completes while maintenance owns observation
        require(streams.health().control == Control::Unknown, "paused probe changed current control evidence");
        for (const auto& event : streams.recentHealthEvents()) {
            // Events from earlier epochs are tagged, so inspect only this one.
            const auto prefix = "runtime-health epoch=" + std::to_string(streams.health().runtimeEpoch) + " ";
            require(!event.starts_with(prefix + "control="), "paused probe published obsolete health evidence");
        }
    }
    streams.stop();
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
    stream::removeStreamUpdateObserver(observer);
    std::this_thread::sleep_for(1100ms);
    require(streams.health().traffic.state == State::Paused && !streams.trafficOpen(), "old probe revived stopped stream");
    std::cout << "runtime_health: production WebSockets, malformed frames, isolated recovery, terminal errors, stop/probe race passed\n";
}
