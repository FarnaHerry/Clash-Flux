#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

import std;
import clashflux.vpn_compensation;

using namespace vpn::compensation;

int main() {
    assert(NormalizeIpv4Cidr("10.1.2.9/24") == "10.1.2.0/24");
    assert(!NormalizeIpv4Cidr("0.0.0.0/0"));
    assert(NormalizeIpv4Cidr("0.0.0.0/0", true) == "0.0.0.0/0");
    assert(IsSyntheticIpv4("198.18.1.7"));
    assert(!IsSyntheticIpv4("198.51.100.7"));
    const std::vector<PhysicalRoute> routes{
        {"default", "eth0", "192.0.2.1", "192.0.2.9", 100, true},
        {"10.0.0.0/8", "tun0", {}, {}, 1, false},
        {"198.51.100.0/24", "eth9", "192.0.2.2", "192.0.2.10", 90, true},
        {"198.51.100.0/24", "eth0", "192.0.2.1", "192.0.2.9", 20, true},
    };
    assert(SelectPhysicalRoute("198.51.100.1", routes)->interfaceName == "eth0");
    assert(!SelectPhysicalRoute("198.18.1.7", routes));
    assert(SelectPhysicalRoute("10.2.3.4", routes)->interfaceName == "eth0");

    std::vector<std::vector<std::string>> commands;
    CommandRunner runner = [&commands](const std::vector<std::string>& command) {
        commands.push_back(command);
        if (std::ranges::find(command, "show") != command.end())
            return CommandResult{0, "[]"};
        return CommandResult{0, {}};
    };
    RouteRegistry registry(std::move(runner));
    std::string error;
    auto first = registry.acquire({RoutePurpose::NativeDestination, "10.5.4.9/16",
                                   "ppp0", {}, {}}, error);
    assert(first && error.empty());
    const auto commandCount = commands.size();
    auto second = registry.acquire({RoutePurpose::NativeDestination, "10.5.0.0/16",
                                    "ppp0", {}, {}}, error);
    assert(second && commands.size() == commandCount);
    second.reset();
    assert(commands.size() == commandCount);
    first.reset();
    assert(commands.size() > commandCount);

    std::vector<RouteLease> leases;
    assert(ReplaceNativeRoutes(registry, "ppp0",
                               std::vector<std::string>{"10.7.0.0/16", "10.8.2.1/24"},
                               leases, error));
    assert(leases.size() == 2);
    assert(!ReplaceNativeRoutes(registry, "ppp0", std::vector<std::string>{"0.0.0.0/0"},
                                leases, error));
    leases.clear();

    // Invalid targets are rejected before the executor is called.
    std::size_t invalidCalls = 0;
    RouteRegistry validating([&invalidCalls](const std::vector<std::string>&) {
        ++invalidCalls;
        return CommandResult{0, "[]"};
    });
    assert(!validating.acquire({RoutePurpose::NativeDestination, "0.0.0.0/0", "ppp0", {}, {}}, error));
    assert(invalidCalls == 0);
    assert(!validating.acquire({RoutePurpose::NativeDestination, "10.0.0.0/8", "ppp0;bad", {}, {}}, error));
    assert(invalidCalls == 0);

    // A data-route failure removes the reservation but never installs a rule.
    std::vector<std::vector<std::string>> routeFailureCommands;
    RouteRegistry routeFailure([&routeFailureCommands](const std::vector<std::string>& command) {
        routeFailureCommands.push_back(command);
        const bool dataAdd = std::ranges::find(command, "route") != command.end() &&
                             std::ranges::find(command, "add") != command.end() &&
                             std::ranges::find(command, "unreachable") == command.end();
        if (dataAdd) return CommandResult{2, "data route denied"};
        if (std::ranges::find(command, "show") != command.end()) return CommandResult{0, "[]"};
        return CommandResult{0, {}};
    });
    assert(!routeFailure.acquire({RoutePurpose::Transport, "203.0.113.9/32", "eth0",
                                  "192.0.2.1", {}}, error));
    assert(std::ranges::none_of(routeFailureCommands, [](const auto& command) {
        return std::ranges::find(command, "rule") != command.end() &&
               std::ranges::find(command, "add") != command.end();
    }));
    assert(std::ranges::any_of(routeFailureCommands, [](const auto& command) {
        return std::ranges::find(command, "del") != command.end() &&
               std::ranges::find(command, "unreachable") != command.end();
    }));
    assert(std::ranges::none_of(routeFailureCommands, [](const auto& command) {
        return std::ranges::find(command, "flush") != command.end() ||
               std::ranges::find(command, "main") != command.end() ||
               std::ranges::find(command, "replace") != command.end();
    }));

    // Rule failure rolls back both the data route and the private-table sentinel.
    std::vector<std::vector<std::string>> ruleFailureCommands;
    RouteRegistry ruleFailure([&ruleFailureCommands](const std::vector<std::string>& command) {
        ruleFailureCommands.push_back(command);
        if (std::ranges::find(command, "show") != command.end()) return CommandResult{0, "[]"};
        if (std::ranges::find(command, "rule") != command.end() &&
            std::ranges::find(command, "add") != command.end())
            return CommandResult{2, "rule denied"};
        return CommandResult{0, {}};
    });
    assert(!ruleFailure.acquire({RoutePurpose::NativeDestination, "10.40.0.0/16", "ppp0", {}, {}}, error));
    assert(std::ranges::any_of(ruleFailureCommands, [](const auto& command) {
        return std::ranges::find(command, "route") != command.end() &&
               std::ranges::find(command, "del") != command.end();
    }));
    assert(std::ranges::any_of(ruleFailureCommands, [](const auto& command) {
        return std::ranges::find(command, "unreachable") != command.end() &&
               std::ranges::find(command, "del") != command.end();
    }));

    // EEXIST on our sentinel is a collision: retry with the next private table.
    std::vector<std::vector<std::string>> collisionCommands;
    RouteRegistry collision([&collisionCommands](const std::vector<std::string>& command) {
        collisionCommands.push_back(command);
        if (std::ranges::find(command, "show") != command.end()) return CommandResult{0, "[]"};
        const auto table = std::ranges::find(command, "table");
        if (table != command.end() && std::next(table) != command.end() &&
            *std::next(table) == "52000" &&
            std::ranges::find(command, "unreachable") != command.end())
            return CommandResult{2, "RTNETLINK answers: File exists"};
        return CommandResult{0, {}};
    });
    auto collisionLease = collision.acquire({RoutePurpose::NativeDestination, "10.50.0.0/16",
                                              "ppp0", {}, {}}, error);
    assert(collisionLease);
    assert(std::ranges::any_of(collisionCommands, [](const auto& command) {
        const auto table = std::ranges::find(command, "table");
        return table != command.end() && std::next(table) != command.end() &&
               *std::next(table) == "52001";
    }));
    collisionLease.reset();

    // A replacement is atomic: a failed second acquire leaves the old set intact
    // and destroys only leases acquired for the attempted replacement.
    std::vector<std::vector<std::string>> replacementCommands;
    RouteRegistry replacementRegistry([&replacementCommands](const std::vector<std::string>& command) {
        replacementCommands.push_back(command);
        if (std::ranges::find(command, "show") != command.end()) return CommandResult{0, "[]"};
        if (std::ranges::find(command, "10.8.0.0/16") != command.end() &&
            std::ranges::find(command, "route") != command.end() &&
            std::ranges::find(command, "add") != command.end())
            return CommandResult{2, "second route denied"};
        return CommandResult{0, {}};
    });
    std::vector<RouteLease> old;
    assert(ReplaceNativeRoutes(replacementRegistry, "ppp0",
                               std::vector<std::string>{"10.6.0.0/16"}, old, error));
    const auto beforeReplacement = replacementCommands.size();
    assert(!ReplaceNativeRoutes(replacementRegistry, "ppp0",
                                std::vector<std::string>{"10.7.0.0/16", "10.8.0.0/16"},
                                old, error));
    assert(old.size() == 1);
    assert(std::ranges::any_of(replacementCommands.begin() + static_cast<std::ptrdiff_t>(beforeReplacement),
                               replacementCommands.end(), [](const auto& command) {
        return std::ranges::find(command, "10.7.0.0/16") != command.end() &&
               std::ranges::find(command, "del") != command.end();
    }));
    old.clear();

    // The rule shape preserves precedence around sing-box's pref 9000 rule.
    std::vector<std::vector<std::string>> shapeCommands;
    RouteRegistry shape([&shapeCommands](const std::vector<std::string>& command) {
        shapeCommands.push_back(command);
        if (std::ranges::find(command, "show") != command.end()) return CommandResult{0, "[]"};
        return CommandResult{0, {}};
    });
    auto bound = shape.acquire({RoutePurpose::BoundInterface, {}, "ppp0", {}, {}}, error);
    assert(bound);
    assert(std::ranges::any_of(shapeCommands, [](const auto& command) {
        return std::ranges::find(command, "rule") != command.end() &&
               std::ranges::find(command, "8100") != command.end() &&
               std::ranges::find(command, "oif") != command.end();
    }));
    bound.reset();
    auto transportShape = shape.acquire({RoutePurpose::Transport, "203.0.113.9/32", "eth0",
                                         "192.0.2.1", {}}, error);
    assert(transportShape);
    assert(std::ranges::any_of(shapeCommands, [](const auto& command) {
        return std::ranges::find(command, "rule") != command.end() &&
               std::ranges::find(command, "8000") != command.end();
    }));
    transportShape.reset();
    auto nativeShape = shape.acquire({RoutePurpose::NativeDestination, "10.70.0.0/16", "ppp0", {}, {}}, error);
    assert(nativeShape);
    assert(std::ranges::any_of(shapeCommands, [](const auto& command) {
        return std::ranges::find(command, "rule") != command.end() &&
               std::ranges::find(command, "12000") != command.end();
    }));
    nativeShape.reset();

    // A lease outlives its registry object; the shared implementation remains
    // available to perform exact, session-owned cleanup.
    std::vector<std::vector<std::string>> lifetimeCommands;
    RouteLease surviving;
    {
        auto owner = std::make_unique<RouteRegistry>([&lifetimeCommands](const std::vector<std::string>& command) {
            lifetimeCommands.push_back(command);
            if (std::ranges::find(command, "show") != command.end()) return CommandResult{0, "[]"};
            return CommandResult{0, {}};
        });
        surviving = owner->acquire({RoutePurpose::NativeDestination, "10.60.0.0/16", "ppp0", {}, {}}, error);
    }
    assert(surviving);
    const auto beforeLifetimeCleanup = lifetimeCommands.size();
    surviving.reset();
    assert(lifetimeCommands.size() > beforeLifetimeCleanup);
    return 0;
}
