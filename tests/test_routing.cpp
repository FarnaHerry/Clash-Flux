#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include "../src/wire_codec.h"

import std;
import nlohmann.json;
import clashflux.db;
import clashflux.vpn;
import clashflux.singbox;
import clashflux.routing;

int main() {
    {
        clashflux::network::Settings network;
        network.revision = 17;
        network.stack = "mixed";
        network.routes = "192.0.2.0/24\n2001:db8::/32";
        network.override_routes = true;
        network.override_auth = true;
        network.username = "literal user";
        network.password = "literal secret ";
        network.override_bypass = true;
        network.bypass = "localhost\n*.example.test";
        assert(clashflux::network::Validate(network).empty());
        const auto encodedNetwork = clashflux::wire::EncodeNetworkSettings(network);
        assert(encodedNetwork);
        const auto decodedNetwork = clashflux::wire::DecodeNetworkSettings(encodedNetwork.value);
        assert(decodedNetwork && decodedNetwork.value == network);
        for (const auto bad : {"{}", "{\"format_version\":1,\"revision\":0}", "[]", "null", "{", "{\"format_version\":2}", "{\"unknown\":true}", "{\"mtu\":\"secret value\"}"}) {
            const auto value = clashflux::wire::DecodeNetworkSettings(bad);
            assert(!value && value.error.find("secret value") == std::string::npos);
        }
        for (const auto bad : {"1.2.3.256/24", "01.2.3.4/24", "1.2.3.4/33", "1.2.3.4", "host.test/24", "1::2::3/64", "::/129"})
            assert(!clashflux::network::CIDR(bad));
        for (const auto good : {"0.0.0.0/0", "192.0.2.1/32", "::/0", "::1/128", "2001:db8::1/64", "::ffff:192.0.2.1/128"})
            assert(clashflux::network::CIDR(good));
        assert(!clashflux::network::Validate(network, true).empty()); // Android stack policy
        network.stack = "gvisor";
        assert(clashflux::network::Validate(network, true).empty());
        auto legacy = nlohmann::json::parse(encodedNetwork.value);
        legacy.erase("advanced");
        const auto old = clashflux::wire::DecodeNetworkSettings(legacy.dump());
        assert(old && !old.value.advanced && old.value.password == network.password);
        network.advanced.emplace();
        network.advanced->auto_redirect = "enabled";
        network.advanced->override_filters = true;
        network.advanced->uid_mode = "include";
        network.advanced->uids = "1000\n2000:3000";
        network.advanced->mac_mode = "exclude";
        network.advanced->macs = "aa:bb:cc:dd:ee:ff";
        assert(clashflux::network::Validate(network).empty());
        const auto advancedText = clashflux::wire::EncodeNetworkSettings(network);
        assert(advancedText && clashflux::wire::DecodeNetworkSettings(advancedText.value).value == network);
        auto incomplete = nlohmann::json::parse(advancedText.value);
        incomplete["advanced"].erase("pac_port");
        assert(!clashflux::wire::DecodeNetworkSettings(incomplete.dump()));
        network.advanced->uids = "3000:2000";
        assert(!clashflux::network::Validate(network).empty());
        network.advanced->uids = "1000";
        network.advanced->auto_redirect = "disabled";
        assert(!clashflux::network::Validate(network).empty());
        network.advanced->override_filters = false;
        network.advanced->proxy_mode = "pac_url";
        for (const auto& url : {"https://trusted.test/proxy.pac", "http://127.0.0.1:7900/proxy.pac"}) {
            network.advanced->pac_url = url;
            assert(clashflux::network::Validate(network).empty());
        }
        for (const auto& url : {"javascript:bad", "https://user:secret@host/pac", "https:///empty", "https://bad.test/\nsecret", "https://?q=bad", "http://host:65536/pac", "http://[bad]/pac", "http://-bad.test/pac"}) {
            network.advanced->pac_url = url;
            assert(!clashflux::network::Validate(network).empty());
        }
        network.advanced.reset();
        network.routes = "192.0.2.0/99";
        assert(!clashflux::network::Validate(network).empty());
        clashflux::network::ProxyLease lease;
        lease.original.desktop = "gnome";
        lease.original.values.push_back({"", "mode", "'auto'"});
        lease.applied = lease.original;
        lease.applied.values[0].value = "'manual'";
        const auto encodedLease = clashflux::wire::EncodeProxyLease(lease);
        assert(encodedLease);
        const auto decodedLease = clashflux::wire::DecodeProxyLease(encodedLease.value);
        assert(decodedLease && decodedLease.value.original == lease.original && decodedLease.value.applied == lease.applied);
    }
    singbox::RuleSetLibrary library;
    library.revision = 9;
    library.resources.push_back({.id="ads", .name="Ads", .type="inline", .format="source",
        .content=R"({"version":3,"rules":[{"domain_suffix":["ads.test"]}]})", .refresh_generation=12});
    assert(routing::DecodeRuleSetLibrary(routing::EncodeRuleSetLibrary(library)) == library);
    assert(routing::DecodeRuleSetLibrary("").resources.empty());
    assert(routing::DecodeRuleSetLibrary(R"({"resources":[{"id":"old","name":"Old"}]})").resources[0].refresh_generation == 0);
    vpn::VpnPolicy setPolicy{.rules={{.match=vpn::MatchKind::RuleSet, .pattern="ads", .connectionId="profile-1"}}};
    assert(routing::DecodePolicy(routing::EncodePolicy(setPolicy)) == setPolicy);
    vpn::VpnPolicy directPolicy{.rules={{.match=vpn::MatchKind::DomainSuffix,
        .pattern="direct.test", .connectionId=std::string(vpn::DirectConnectionId())}}};
    assert(routing::DecodePolicy(routing::EncodePolicy(directPolicy)) == directPolicy);
    std::string directError;
    assert(vpn::ValidatePolicyRules(directPolicy.rules, directError));
    directPolicy.rules[0].targetObject = "DIRECT";
    assert(!vpn::ValidatePolicyRules(directPolicy.rules, directError));
    for (const std::string invalid : {"null", "[]", "{", "{\"unknown\":1}",
            "{\"resources\":[{\"extra\":true}]}", "{\"format_version\":99}",
            "{\"resources\":false}", "{} trailing"}) {
        bool refused = false;
        try { routing::DecodeRuleSetLibrary(invalid); } catch (...) { refused = true; }
        assert(refused);
    }
    singbox::ProfileRuleEdits edits;
    edits.revision = 17;
    edits.rules.push_back({.id="r1", .operation="replace", .anchor="clash:DOMAIN,a.test,DIRECT",
        .match="domain", .pattern="b.test", .target="Pick"});
    assert(routing::DecodeProfileRules(routing::EncodeProfileRules(edits)) == edits);
    assert(routing::DecodeProfileRules("").rules.empty());
    for (const std::string invalid : {"null", "[]", "{", "{\"unknown\":1}", "{\"rules\":3}", "{\"format_version\":99}"}) {
        bool refused = false;
        try { routing::DecodeProfileRules(invalid); } catch (...) { refused = true; }
        assert(refused);
    }
    db::Profile main{.id = 1, .name = "main", .selected = true};
    db::Profile other{.id = 2, .name = "other"};
    db::Profile native{.id = 3, .name = "native", .type = "pptp",
                       .nativeRoutes = " 10.2.0.0/16,\n10.3.0.0/16\r\n10.2.0.0/16 "};
    vpn::VpnPolicy policy{.defaultMainId = "profile-2",
        .rules = {{vpn::MatchKind::DomainSuffix, "example.test", "profile-3", 100}}};
    const auto encoded = routing::EncodePolicy(policy);
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

    // Ordinary OpenVPN source files keep the normal source identity and lifecycle.
    // Only PPTP is projected into separately controlled native sessions.
    db::Profile localOvpn{.id = 41, .name = "local OpenVPN", .file = "41.ovpn",
                           .selected = true, .type = "local"};
    db::Profile remoteOvpn{.id = 42, .name = "remote OpenVPN",
                            .url = "https://example.test/profile.ovpn", .file = "42.yaml",
                            .type = "remote"};
    std::vector<db::Profile> ovpnSources{localOvpn, remoteOvpn, native};
    routing::PopulateOptions(options, ovpnSources, policy, {});
    assert(options.mainConnectionId == "profile-41");
    assert(options.mainSourceName == "local OpenVPN");
    assert(options.nativeConnections.size() == 1);
    assert(options.nativeConnections.front().id == "profile-3");
    assert(options.nativeConnections.front().kind == vpn::ConnectionKind::Pptp);
    ovpnSources[0].selected = false;
    ovpnSources[1].selected = true;
    routing::PopulateOptions(options, ovpnSources, policy, {});
    assert(options.mainConnectionId == "profile-42");
    assert(options.nativeConnections.size() == 1);

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
