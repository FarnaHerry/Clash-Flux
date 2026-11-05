#include "../src/connection_trace.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

import clashflux.api;

namespace ct = clashflux::connection_trace;
namespace wire = clashflux::wire;
using namespace std::chrono_literals;

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void unit() {
    auto plan = std::make_shared<ct::Plan>();
    plan->ruleCount = 2; plan->origins = {{0,"first","main","Main","user-override"}};
    const std::string rules = R"json({"rules":[{"type":"default","payload":"domain=a.test","proxy":"route(A)"},{"type":"default","payload":"domain=b.test","proxy":"route(B)"}]})json";
    std::string body = R"json({"connections":[{"id":"old","rule":"domain=a.test => route(A)","metadata":{}}]})json";
    int ruleCalls = 0;
    ct::Operations ops{
        .instance = [] { return std::optional<std::uint64_t>{11}; },
        .rules = [&] { ++ruleCalls; return std::optional{rules}; },
        .connections = [&] { return std::optional{body}; },
        .current = [](const ct::Binding&) { return true; },
    };
    auto first = ct::Read(plan, 1, 7, 11, {}, ops);
    require(bool(first.binding), "baseline rejected");
    const auto resolve = [&](const ct::Binding* binding, const std::string& rule, const std::string& id) {
        return ct::Resolve(binding, rule, "", id);
    };
    require(resolve(first.binding.get(), "domain=a.test => route(A)", "old").status == ct::Status::PriorPlan, "old connection rebound");
    const auto match = resolve(first.binding.get(), "domain=a.test => route(A)", "new");
    require(match.status == ct::Status::Matched && match.ruleId == "first" && match.sourceId == "main" && match.revision == 7, "unique rule failed");
    require(resolve(first.binding.get(), "domain=b.test => route(B)", "new").status == ct::Status::Unmapped, "source rule guessed");
    require(resolve(first.binding.get(), "domain=a.test => route(B)", "new").status == ct::Status::Unmapped, "wrong target guessed");
    require(ct::Resolve(first.binding.get(), "domain=a.test => route(A)", "a.test", "new").status == ct::Status::Unavailable, "foreign payload guessed");
    require(bool(ct::Read(plan, 1, 7, 11, first.binding, ops).binding) && ruleCalls == 1, "rules not cached per plan");
    const auto next = ct::Read(plan, 2, 8, 11, first.binding, ops);
    require(next.binding && ruleCalls == 2 && resolve(next.binding.get(), "domain=a.test => route(A)", "old").status == ct::Status::PriorPlan, "plan reuse rebound history");
    const auto parsed = wire::DecodeRuntimeRules(rules); require(bool(parsed), "runtime rules codec failed");
    auto duplicates = parsed.value; duplicates[1] = duplicates[0];
    auto ambiguous = ct::Bind(plan, duplicates, 1, 7, 11);
    require(resolve(ambiguous.get(), "domain=a.test => route(A)", "new").status == ct::Status::Ambiguous, "unannotated duplicate guessed");
    require(!ct::Bind(plan, parsed.value, 1, 0, 11), "unaccepted revision accepted");
    require(!ct::Bind(plan, {parsed.value.front()}, 1, 7, 11), "rule count mismatch accepted");
    require(!wire::DecodeRuntimeRules(R"json({"rules":[{"type":"default","proxy":"route(A)"}]})json"), "missing payload accepted");
    require(!wire::DecodeRuntimeRules("{}") && !wire::DecodeRuntimeRules(R"json({"rules":[{"payload":"x"}]})json"), "bad runtime rules accepted");
    for (const auto* invalid : {"{}", R"json({"connections":null})json", R"json({"connections":[{"id":5}]})json", R"json({"connections":[{}]})json"}) {
        body = invalid; require(!ct::Read(plan, 1, 7, 11, {}, ops).binding, "bad baseline accepted");
    }
    body = R"json({"connections":[]})json";
    ops.instance = [] { return std::optional<std::uint64_t>{12}; };
    require(!ct::Read(plan, 1, 7, 11, first.binding, ops).binding, "foreign native instance accepted");
    int checks = 0;
    ops.instance = [&] { return std::optional<std::uint64_t>{++checks == 1 ? 11U : 12U}; };
    require(!ct::Read(plan, 1, 7, 11, first.binding, ops).binding, "instance changed during IO");
    ops.instance = [] { return std::optional<std::uint64_t>{11}; };
    checks = 0; ops.current = [&](const ct::Binding&) { return ++checks == 1; };
    require(!ct::Read(plan, 1, 7, 11, first.binding, ops).binding, "superseded plan published");
    ops.current = [](const ct::Binding&) { return false; };
    require(!ct::Read(plan, 1, 7, 11, first.binding, ops).binding, "stopped runtime accepted");
    std::cout << "trace: unique, duplicate, history, count/schema, cache, instance and stale-plan guards passed\n";
}

int main(int argc, char** argv) try {
    if (argc == 1) { unit(); return 0; }
    require(argc == 6, "expected controller native metadata marker mode");
    api::ClashApi controller(argv[1], "test-native-secret");
    api::NativeApi native(argv[2], "test-native-secret");
    const auto instance = native.runtime(); require(instance.ok, "native unavailable");
    api::NativeApi denied(argv[2], "incorrect"); require(!denied.startedAt(), "native auth bypass");
    api::ClashApi deniedRest(argv[1], "incorrect"); require(!deniedRest.rules().ok, "REST auth bypass");
    auto plan = std::make_shared<ct::Plan>(); std::ifstream metadata(argv[3]);
    metadata >> plan->ruleCount;
    ct::Origin origin;
    while (metadata >> origin.index >> origin.ruleId >> origin.sourceId >> origin.sourceName >> origin.tier) plan->origins.push_back(origin);
    require(!plan->origins.empty(), "missing compiler origins");
    std::size_t ruleBytes = 0; int reads = 0;
    ct::Operations ops{
        .instance = [&] { return native.startedAt(); },
        .rules = [&]() -> std::optional<std::string> {
            ++reads; auto result = controller.rules(); ruleBytes = result.body.size();
            return result.ok ? std::optional{std::move(result.body)} : std::nullopt;
        },
        .connections = [&]() -> std::optional<std::string> {
            auto result = controller.traceConnections();
            return result.ok ? std::optional{std::move(result.body)} : std::nullopt;
        },
        .current = [](const ct::Binding&) { return true; },
    };
    const auto first = ct::Read(plan, 1, 7, instance.startedAt, {}, ops);
    require(first.binding && !first.binding->priorConnections.empty(), "missing real baseline connection");
    require(ruleBytes > 65536, "large native rule list missing");
    std::ofstream(std::string(argv[4]) + ".ready").put('1');
    const auto deadline = std::chrono::steady_clock::now() + 12s;
    while (!std::filesystem::exists(std::string(argv[4]) + ".go")) {
        require(std::chrono::steady_clock::now() < deadline, "new connection never opened"); std::this_thread::sleep_for(10ms);
    }
    bool oldSeen = false, newSeen = false;
    for (int attempt = 0; attempt < 80 && !newSeen; ++attempt) {
        const auto frame = ct::Read(plan, 1, 7, instance.startedAt, first.binding, ops);
        require(bool(frame.binding), "live trace lost instance proof");
        const auto connections = wire::DecodeConnections(frame.body); require(bool(connections), "live connections invalid");
        for (const auto& connection : connections.value.entries) {
            const auto result = ct::Resolve(frame.binding.get(), connection.rule, connection.rulePayload, connection.id);
            if (first.binding->priorConnections.contains(connection.id)) {
                require(result.status == ct::Status::PriorPlan && result.ruleId.empty(), "live old connection rebound"); oldSeen = true;
            } else {
                const auto expected = std::string_view(argv[5]) == "unique" ? ct::Status::Matched : ct::Status::Ambiguous;
                require(result.status == expected, "native connection did not match rule catalog");
                if (expected == ct::Status::Matched) require(result.ruleId == "rule-a" && result.sourceId == "main" && result.sourceName == "MainFixture", "wrong native rule origin");
                else require(result.ruleId.empty(), "ambiguous rule claimed identity");
                newSeen = true;
            }
        }
        if (!newSeen) std::this_thread::sleep_for(20ms);
    }
    require(oldSeen && newSeen && reads == 1, "live history/new/cache assertions incomplete");
    std::cout << argv[5] << ": native rules=" << plan->ruleCount << " bytes=" << ruleBytes << ", old retained, new traced, rule catalog fetched once\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
