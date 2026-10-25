// Smoke test for the platform-independent VPN orchestration layer.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#ifdef _WIN32
#include "../src/windows_pptp_health.h"
#endif

import std;
import clashflux.openvpn;
import clashflux.pptp;
import clashflux.vpn;

int main() {
    using namespace vpn;
#ifdef _WIN32
    using clashflux::pptp_health::RasEvidence;
    assert(RasEvidence(ERROR_SUCCESS, RASCS_Connected, true) == true);
    assert(RasEvidence(ERROR_SUCCESS, RASCS_Connected, false) == false);
    assert(!RasEvidence(ERROR_SUCCESS, RASCS_Connected, std::nullopt));
    assert(!RasEvidence(ERROR_ACCESS_DENIED, RASCS_Connected, true));
    assert(!RasEvidence(ERROR_SUCCESS, RASCS_Authenticate, true));
    assert(RasEvidence(ERROR_SUCCESS, RASCS_Disconnected, std::nullopt) == false);
    assert(RasEvidence(ERROR_INVALID_HANDLE, RASCS_Connected, true) == false);
#endif

    std::string pptpError;
    const auto pptpConfig = pptp::ParsePptpConfig(
        "# company A\nserver=vpn.example.com\nusername=alice\n"
        "password=p@ss word\ntimeout=45\nrequire_mppe=false\n",
        pptpError);
    assert(pptpConfig.has_value());
    assert(pptpConfig->server == "vpn.example.com");
    assert(pptpConfig->username == "alice");
    assert(pptpConfig->password == "p@ss word");
    assert(pptpConfig->connectTimeoutSecs == 45);
    assert(!pptpConfig->requireMppe);
    assert(!pptp::ParsePptpConfig(
        "server=vpn.example.com\nusername=alice\npassword=secret\n"
        "server=bad host\n",
        pptpError));
    assert(pptp::ParsePptpConfig(
        "server=111.0.0.26\nusername=alice\npassword=secret\n",
        pptpError));
    assert(!pptp::ParsePptpConfig(
        "server=111.26\nusername=alice\npassword=secret\n", pptpError));

    const auto pptpAdapter = pptp::MakePptpAdapter();
    assert(pptpAdapter.descriptor.kind == EngineKind::SystemPptp);
    assert(pptpAdapter.descriptor.connectionKinds ==
           std::vector<ConnectionKind>{ConnectionKind::Pptp});
    assert(pptpAdapter.descriptor.available == pptp::PptpToolsAvailable());

    std::string openVpnError;
    const auto openVpnConfig = openvpn::ParseOpenVpnConfig(
        "client\ndev tun0\nremote vpn.example.com 1194 udp\n", openVpnError);
    assert(openVpnConfig.has_value());
    assert(openVpnConfig->interfaceName == "tun0");
    assert(openVpnConfig->remotes.size() == 1);
    assert(openVpnConfig->remotes.front().host == "vpn.example.com");
    assert(!openvpn::ParseOpenVpnConfig(
        "client\nremote bad$host 1194 udp\n", openVpnError));
    assert(!openvpn::ParseOpenVpnConfig(
        "client\n<ca>\nunterminated\n", openVpnError));
    const auto longOptions = openvpn::ParseOpenVpnConfig(
        "client\n--dev tun0\n--remote 198.51.100.8 443 tcp\n", openVpnError);
    assert(longOptions.has_value());
    assert(longOptions->remotes.front().host == "198.51.100.8");
    const auto remoteProfiles = openvpn::ParseOpenVpnConfig(
        "client\r\n<ca>\r\nremote certificate-data\r\n</ca>\r\n"
        "<connection>\r\n  remote \"first.example.com\" 1194 udp\r\n</connection>\r\n"
        "<connection>\r\n\tremote 'second.example.com' 443 tcp\r\n</connection>",
        openVpnError);
    assert(remoteProfiles.has_value());
    assert(remoteProfiles->remotes.size() == 2);
    assert(remoteProfiles->remotes[0].host == "first.example.com");
    assert(remoteProfiles->remotes[1].host == "second.example.com");
    std::string numericRemotes = remoteProfiles->configText;
    for (const auto& remote : remoteProfiles->remotes | std::views::reverse) {
        numericRemotes.replace(remote.offset, remote.length, "198.51.100.9");
    }
    assert(numericRemotes.find("remote certificate-data") != std::string::npos);
    assert(numericRemotes.find("remote 198.51.100.9 1194 udp") != std::string::npos);
    assert(numericRemotes.find("remote 198.51.100.9 443 tcp") != std::string::npos);
    assert(!openvpn::ParseOpenVpnConfig("remote \"unterminated\n", openVpnError));
    assert(!openvpn::ParseOpenVpnConfig("remote \"host\"suffix\n", openVpnError));
    assert(!openvpn::ParseOpenVpnConfig("remote\n", openVpnError));
    for (const auto& directive : {"config other.ovpn", "--config other.ovpn",
                                  "http-proxy proxy.example 8080",
                                  "socks-proxy proxy.example 1080",
                                  "remote-random-hostname",
                                  "management-query-remote"}) {
        assert(!openvpn::ParseOpenVpnConfig(
            std::string("client\nremote vpn.example 1194\n") + directive + "\n",
            openVpnError));
    }
    assert(!openvpn::ParseOpenVpnConfig("client\nroute 10.0.0.0 255.0.0.0\n",
                                        openVpnError));
    const auto openVpnAdapter = openvpn::MakeOpenVpnAdapter();
    assert(openVpnAdapter.descriptor.kind == EngineKind::SingBox);
    assert(openVpnAdapter.descriptor.connectionKinds ==
           std::vector<ConnectionKind>{ConnectionKind::OpenVpn});
    assert(openVpnAdapter.descriptor.available);

    const std::vector<EngineDescriptor> engines{
        {EngineKind::SingBox, 100, true, {ConnectionKind::ProxyConfig}},
        {EngineKind::SystemPptp, 90, true, {ConnectionKind::Pptp}},
    };

    VpnConnection main{
        .id = "main",
        .name = "主 VPN",
        .kind = ConnectionKind::ProxyConfig,
    };
    VpnConnection company{
        .id = "company-a",
        .name = "A 公司",
        .kind = ConnectionKind::Pptp,
        .internalRoutes = {"10.20.0.0/16"},
        .interfaceName = "ppp0",
    };

    VpnManager manager;
    manager.setEngines(engines);
    manager.setConnections({main, company});
    manager.setPolicy(VpnPolicy{
        .defaultMainId = "main",
        .rules = {{MatchKind::Ipv4Cidr, "10.20.0.0/16", "company-a", 100},
                  {MatchKind::Ipv4Cidr, "10.21.0.0/16", "company-a", 90}},
    });

    assert(manager.selectEngine("main").engine == EngineKind::SingBox);
    assert(manager.selectEngine("company-a").engine == EngineKind::SystemPptp);
    bool rejectedUnavailable = false;
    try { manager.resolveConnection("10.20.8.9"); }
    catch (const std::runtime_error&) { rejectedUnavailable = true; }
    assert(rejectedUnavailable);
    assert(NormalizeRuleDomain("https://Portal.Example:443/path?q=1") == "portal.example");
    assert(!NormalizeRuleDomain("https://user:pass@example.com/path"));
    assert(!NormalizeRuleDomain("https:///missing-host"));
    assert(manager.resolveConnection("198.51.100.9") == "main");

    const TunConfig linuxTun = MakeFullTunConfig(PlatformKind::Linux);
    assert(linuxTun.ready);
    assert(linuxTun.backend == TunBackendKind::SingBox);
    assert(linuxTun.mode == TunCaptureMode::FullDevice);
    assert(linuxTun.routeAddress.size() == 4);

    const TunRoutePlan linuxPlan =
        BuildTunRoutePlan(manager, PlatformKind::Linux);
    assert(linuxPlan.ready());
    assert(linuxPlan.mainConnectionId == "main");
    assert(linuxPlan.requiresNativeRouteBackend());
    assert(linuxPlan.nativeRoutes.size() == 2);
    assert(linuxPlan.nativeRoutes.front().destination == "10.20.0.0/16");
    assert(linuxPlan.nativeRoutes.front().interfaceName == "ppp0");
    assert((linuxPlan.capture.routeExcludeAddress ==
            std::vector<std::string>{"10.20.0.0/16", "10.21.0.0/16"}));

    const TunRoutePlan windowsPlan =
        BuildTunRoutePlan(manager, PlatformKind::Windows);
    assert(windowsPlan.ready());
    assert(windowsPlan.capture.backend == TunBackendKind::SingBox);

    const TunConfig androidTun = MakeFullTunConfig(PlatformKind::Android);
    assert(!androidTun.ready);
    assert(androidTun.backend == TunBackendKind::AndroidVpnService);

    bool routesApplied = false;
    manager.setAdapters({
        EngineAdapter{
            .descriptor = engines[0],
            .connect = [](VpnConnection&, std::string&) { return true; },
            .applyRoutes = {},
            .disconnect = {},
        },
        EngineAdapter{
            .descriptor = engines[1],
            .connect = [](VpnConnection&, std::string&) { return true; },
            .applyRoutes = [&routesApplied](
                               VpnConnection&, std::span<const std::string> routes,
                               std::string&) {
                routesApplied =
                    std::ranges::find(routes, "10.21.0.0/16") != routes.end();
                return routesApplied;
            },
            .disconnect = {},
        },
    });
    std::string error;
    assert(manager.connect("company-a", error));
    assert(manager.findConnection("company-a")->activeEngine ==
           EngineKind::SystemPptp);
    assert(routesApplied);
    manager.disconnect("company-a");

    // A service absent at application startup can be installed/restarted later.
    // Both selection and actual dialing must recover without replacing adapters
    // (which would disconnect sessions and release their route leases).
    int recoveredDialCalls = 0;
    int recoveredRouteCalls = 0;
    int recoveredDisconnectCalls = 0;
    int otherDisconnectCalls = 0;
    {
        VpnManager recovering;
        recovering.setAdapters({
            EngineAdapter{
                .descriptor = {EngineKind::SystemPptp, 80, false, {ConnectionKind::Pptp}},
                .connect = [&](VpnConnection& connection, std::string&) {
                    ++recoveredDialCalls;
                    connection.interfaceName = "service-ppp";
                    return true;
                },
                .applyRoutes = [&](VpnConnection&, std::span<const std::string> routes, std::string&) {
                    ++recoveredRouteCalls;
                    return std::ranges::find(routes, "10.90.0.0/16") != routes.end();
                },
                .disconnect = [&](VpnConnection&) { ++recoveredDisconnectCalls; },
            },
            EngineAdapter{
                .descriptor = {EngineKind::SingBox, 95, true, {ConnectionKind::OpenVpn}},
                .connect = [](VpnConnection&, std::string&) { return true; },
                .applyRoutes = {},
                .disconnect = [&](VpnConnection&) { ++otherDisconnectCalls; },
            },
        });
        recovering.setConnections({
            VpnConnection{.id = "recovering", .kind = ConnectionKind::Pptp,
                          .enginePreference = {EngineKind::SystemPptp},
                          .internalRoutes = {"10.90.0.0/16"}},
            VpnConnection{.id = "unrelated", .kind = ConnectionKind::OpenVpn,
                          .enginePreference = {EngineKind::SingBox}},
        });
        assert(recovering.connect("unrelated", error));
        assert(!recovering.selectEngine("recovering").ok());
        assert(!recovering.connect("recovering", error));
        assert(recoveredDialCalls == 0);
        recovering.setEngineAvailable(EngineKind::Unknown, true);
        assert(!recovering.selectEngine("recovering").ok());
        recovering.setEngineAvailable(EngineKind::SystemPptp, true);
        assert(recovering.selectEngine("recovering").engine == EngineKind::SystemPptp);
        assert(recovering.connect("recovering", error));
        assert(recoveredDialCalls == 1 && recoveredRouteCalls == 1);
        recovering.setEngineAvailable(EngineKind::SystemPptp, false);
        assert(!recovering.selectEngine("recovering").ok());
        assert(recovering.selectEngine("unrelated").engine == EngineKind::SingBox);
        assert(recovering.findConnection("unrelated")->state == ConnectionState::Connected);
        assert(recovering.findConnection("recovering")->interfaceName == "service-ppp");
        assert(recoveredDisconnectCalls == 0 && otherDisconnectCalls == 0);
        recovering.setEngineAvailable(EngineKind::SystemPptp, true);
        assert(recovering.connect("recovering", error));
        assert(recoveredDialCalls == 1 && recoveredRouteCalls == 1);
    }
    assert(recoveredDisconnectCalls == 1 && otherDisconnectCalls == 1);

    int disconnectCalls = 0;
    int dialCalls = 0;
    {
        VpnManager scopedManager;
        scopedManager.setAdapters({
            EngineAdapter{
                .descriptor =
                    {EngineKind::SystemPptp, 1, true, {ConnectionKind::Pptp}},
                .connect = [&dialCalls](VpnConnection&, std::string&) {
                    ++dialCalls;
                    return true;
                },
                .applyRoutes = {},
                .disconnect = [&disconnectCalls](VpnConnection&) {
                    ++disconnectCalls;
                },
            },
        });
        scopedManager.setConnections({VpnConnection{
            .id = "raii",
            .name = "RAII",
            .kind = ConnectionKind::Pptp,
            .enginePreference = {EngineKind::SystemPptp},
        }});
        assert(scopedManager.connect("raii", error));
        assert(scopedManager.connect("raii", error));
        assert(dialCalls == 1);
        assert(disconnectCalls == 0);
    }
    assert(disconnectCalls == 1);

    int exceptionCleanupCalls = 0;
    VpnManager exceptionManager;
    exceptionManager.setAdapters({
        EngineAdapter{
            .descriptor =
                {EngineKind::SystemPptp, 1, true, {ConnectionKind::Pptp}},
            .connect = [](VpnConnection&, std::string&) -> bool {
                throw std::runtime_error("test adapter failure");
            },
            .applyRoutes = {},
            .disconnect = [&exceptionCleanupCalls](VpnConnection&) {
                ++exceptionCleanupCalls;
            },
        },
    });
    exceptionManager.setConnections({VpnConnection{
        .id = "exception",
        .name = "异常测试",
        .kind = ConnectionKind::Pptp,
        .enginePreference = {EngineKind::SystemPptp},
    }});
    assert(!exceptionManager.connect("exception", error));
    assert(exceptionManager.findConnection("exception")->state ==
           ConnectionState::Failed);
    assert(exceptionCleanupCalls == 1);

    // A failed dial can still own temporary transport routes. Both false and
    // exception paths must invoke the adapter's idempotent cleanup.
    exceptionManager.setAdapters({EngineAdapter{
        .descriptor = engines[1],
        .connect = [](VpnConnection& connection, std::string& failure) {
            connection.interfaceName = "ppp-stale";
            failure = "dial failed";
            return false;
        },
        .disconnect = [&exceptionCleanupCalls](VpnConnection&) {
            ++exceptionCleanupCalls;
        },
    }});
    assert(!exceptionManager.connect("exception", error));
    assert(exceptionCleanupCalls == 2);
    assert(exceptionManager.findConnection("exception")->interfaceName.empty());

    // A policy update replaces the complete route set on every active native
    // connection, and restores all attempted connections after partial failure.
    std::map<std::string, std::vector<std::string>> installed;
    std::vector<std::string> applied;
    std::string failNextId;
    bool throwNext = false;
    VpnManager liveManager;
    liveManager.setAdapters({EngineAdapter{
        .descriptor = engines[1],
        .connect = [](VpnConnection&, std::string&) { return true; },
        .applyRoutes = [&](VpnConnection& connection,
                           std::span<const std::string> routes,
                           std::string& failure) {
            applied.push_back(connection.id);
            installed[connection.id] = {routes.begin(), routes.end()};
            if (connection.id == failNextId) {
                failNextId.clear();
                if (throwNext) throw std::runtime_error("partial route exception");
                failure = "partial route failure";
                return false;
            }
            return true;
        },
        .disconnect = [](VpnConnection&) {},
    }});
    liveManager.setConnections({
        VpnConnection{.id = "a", .name = "A", .kind = ConnectionKind::Pptp},
        VpnConnection{.id = "b", .name = "B", .kind = ConnectionKind::Pptp},
        VpnConnection{.id = "idle", .name = "Idle", .kind = ConnectionKind::Pptp},
    });
    const VpnPolicy initialPolicy{
        .rules = {{MatchKind::Ipv4Cidr, "10.1.0.0/16", "a", 100},
                  {MatchKind::ExactIp, "10.2.0.1", "b", 100}},
    };
    liveManager.setPolicy(initialPolicy);
    assert(liveManager.connect("a", error));
    assert(liveManager.connect("b", error));
    assert(installed["b"] == std::vector<std::string>{"10.2.0.1/32"});
    const auto initialRoutes = installed;
    const VpnPolicy changedPolicy{
        .rules = {{MatchKind::Ipv4Cidr, "10.3.0.0/16", "a", 100},
                  {MatchKind::Ipv4Cidr, "10.4.0.0/16", "b", 100},
                  {MatchKind::Ipv4Cidr, "10.5.0.0/16", "idle", 100}},
    };
    applied.clear();
    failNextId = "b";
    assert(!liveManager.applyPolicy(changedPolicy, error));
    assert(error.find("partial route failure") != std::string::npos);
    assert(liveManager.policy() == initialPolicy);
    assert(installed == initialRoutes);
    assert((applied == std::vector<std::string>{"a", "b", "b", "a"}));
    assert(liveManager.findConnection("a")->state == ConnectionState::Connected);

    applied.clear();
    failNextId = "b";
    throwNext = true;
    assert(!liveManager.applyPolicy(changedPolicy, error));
    assert(error.find("partial route exception") != std::string::npos);
    assert(liveManager.policy() == initialPolicy);
    assert(installed == initialRoutes);
    assert((applied == std::vector<std::string>{"a", "b", "b", "a"}));

    applied.clear();
    assert(liveManager.applyPolicy(changedPolicy, error));
    assert(error.empty());
    assert(liveManager.policy() == changedPolicy);
    assert((applied == std::vector<std::string>{"a", "b"}));
    assert(installed["a"] == std::vector<std::string>{"10.3.0.0/16"});
    assert(installed["b"] == std::vector<std::string>{"10.4.0.0/16"});
    assert(!installed.contains("idle"));
    applied.clear();
    assert(liveManager.applyPolicy(changedPolicy, error));
    assert(applied.empty());

    // Removing a rule must send an empty desired set, not leave stale routes.
    assert(liveManager.applyPolicy(VpnPolicy{}, error));
    assert(installed["a"].empty());
    assert(installed["b"].empty());
    assert((applied == std::vector<std::string>{"a", "b"}));

    // These rules are handled by the core's bound outbound. They must never
    // produce system default routes, malformed IPv6 /32 routes or DNS routes.
    const VpnPolicy coreOnlyPolicy{
        .rules = {{MatchKind::Any, "", "a", 100},
                  {MatchKind::Ipv4Cidr, "0.0.0.0/0", "a", 100},
                  {MatchKind::ExactIp, "2001:db8::1", "a", 100},
                  {MatchKind::DomainSuffix, "example.com", "a", 100}},
    };
    applied.clear();
    assert(liveManager.applyPolicy(coreOnlyPolicy, error));
    assert(liveManager.policy() == coreOnlyPolicy);
    assert(applied.empty());
    assert(installed["a"].empty());

    VpnConnection unsafeNative{
        .id = "unsafe", .name = "Unsafe", .kind = ConnectionKind::Pptp,
        .internalRoutes = {"0.0.0.0/0"},
    };
    liveManager.upsertConnection(unsafeNative);
    assert(!liveManager.connect("unsafe", error));
    assert(!error.empty());
    assert(!installed.contains("unsafe"));
    unsafeNative.internalRoutes = {"2001:db8::1/64"};
    liveManager.upsertConnection(unsafeNative);
    assert(!liveManager.connect("unsafe", error));
    unsafeNative.internalRoutes = {"10.20.0.1", "10.20.0.1/32", "10.21.0.0/16"};
    liveManager.upsertConnection(unsafeNative);
    assert(liveManager.connect("unsafe", error));
    assert((installed["unsafe"] ==
            std::vector<std::string>{"10.20.0.1/32", "10.21.0.0/16"}));

    // A rollback error must be surfaced instead of reporting the old policy as
    // successfully applied to a backend that could not restore its state.
    bool failRoutes = false;
    VpnManager rollbackManager;
    rollbackManager.setAdapters({EngineAdapter{
        .descriptor = engines[1],
        .connect = [](VpnConnection&, std::string&) { return true; },
        .applyRoutes = [&](VpnConnection&, std::span<const std::string>,
                           std::string& failure) {
            if (!failRoutes) return true;
            failure = "route backend unavailable";
            return false;
        },
        .disconnect = [](VpnConnection&) {},
    }});
    rollbackManager.setConnections({
        VpnConnection{.id = "a", .name = "A", .kind = ConnectionKind::Pptp},
    });
    assert(rollbackManager.connect("a", error));
    failRoutes = true;
    assert(!rollbackManager.applyPolicy(initialPolicy, error));
    assert(rollbackManager.policy() == VpnPolicy{});
    assert(error.find("回滚失败") != std::string::npos);
    std::println("test_vpn: ok");
}
