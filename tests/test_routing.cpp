#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

import std;
import nlohmann.json;
import clashflux.db;
import clashflux.vpn;
import clashflux.singbox;
import clashflux.routing;

int main() {
    db::Profile main{.id = 1, .name = "main", .selected = true};
    db::Profile other{.id = 2, .name = "other"};
    db::Profile native{.id = 3, .name = "native", .type = "pptp",
                       .nativeRoutes = " 10.2.0.0/16,\n10.3.0.0/16\r\n10.2.0.0/16 "};
    vpn::VpnPolicy policy{.defaultMainId = "profile-2",
        .rules = {{vpn::MatchKind::DomainSuffix, "example.test", "profile-3", 100}}};
    const auto encoded = routing::EncodePolicy(policy).dump();
    assert(routing::DecodePolicy(encoded) == policy);
    assert(routing::DecodePolicy("").rules.empty());
    for (const std::string invalid : {"null", "[]", "{", "{\"rules\":3}",
            "{\"rules\":[{\"match\":\"unsupported\"}]}"}) {
        bool threw = false;
        try { routing::DecodePolicy(invalid); } catch (...) { threw = true; }
        assert(threw);
    }

    std::vector<db::Profile> profiles{main, other, native};
    singbox::CompileOptions options;
    routing::PopulateOptions(options, profiles, policy, {});
    assert(options.mainConnectionId == "profile-1"); // selected beats stale saved main
    assert(options.globalRules == policy.rules);
    assert(options.nativeConnections.size() == 1);
    assert(!options.nativeConnections.front().connected);
    assert(options.nativeConnections.front().internalRoutes.size() == 2);
    assert(options.tunExcludeAddresses.empty()); // data never bypasses global policy

    singbox::NativeConnection active;
    active.id = "profile-3";
    active.connected = true;
    active.interfaceName = "ppp7";
    active.transportAddress = "203.0.113.7";
    routing::PopulateOptions(options, profiles, policy, std::vector{active});
    assert(options.nativeConnections.front().connected);
    assert(options.nativeConnections.front().interfaceName == "ppp7");
    assert(options.nativeConnections.front().transportAddress == "203.0.113.7");
    assert(options.nativeConnections.front().kind == vpn::ConnectionKind::Pptp);
    active.interfaceName.clear();
    routing::PopulateOptions(options, profiles, policy, std::vector{active});
    assert(!options.nativeConnections.front().connected);
    assert(options.nativeConnections.front().transportAddress.empty());
    active.interfaceName = "ppp7";
    active.id = "deleted-profile";
    routing::PopulateOptions(options, profiles, policy, std::vector{active});
    assert(!options.nativeConnections.front().connected);

    profiles.front().selected = false;
    routing::PopulateOptions(options, profiles, policy, {});
    assert(options.mainConnectionId.empty());
    profiles.front().selected = true;
    profiles[1].selected = true;
    bool threw = false;
    try { routing::PopulateOptions(options, profiles, policy, {}); }
    catch (...) { threw = true; }
    assert(threw);
}
