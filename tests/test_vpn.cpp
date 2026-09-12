// Smoke test for the platform-independent VPN orchestration layer.
#include <cassert>

import std;
import clashflux.openvpn;
import clashflux.pptp;
import clashflux.vpn;

int main() {
    using namespace vpn;

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
    assert(!openvpn::ParseOpenVpnConfig("client\nroute 10.0.0.0 255.0.0.0\n",
                                        openVpnError));
    const auto openVpnAdapter = openvpn::MakeOpenVpnAdapter();
    assert(openVpnAdapter.descriptor.kind == EngineKind::SystemOpenVpn);
    assert(openVpnAdapter.descriptor.connectionKinds ==
           std::vector<ConnectionKind>{ConnectionKind::OpenVpn});
    assert(openVpnAdapter.descriptor.available ==
           openvpn::OpenVpnToolsAvailable());

    const std::vector<EngineDescriptor> engines{
        {EngineKind::Mihomo, 100, true, {ConnectionKind::ProxyConfig}},
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

    assert(manager.selectEngine("main").engine == EngineKind::Mihomo);
    assert(manager.selectEngine("company-a").engine == EngineKind::SystemPptp);
    assert(manager.resolveConnection("10.20.8.9") == "company-a");
    assert(manager.resolveConnection("198.51.100.9") == "main");

    const TunConfig linuxTun = MakeFullTunConfig(PlatformKind::Linux);
    assert(linuxTun.ready);
    assert(linuxTun.backend == TunBackendKind::Mihomo);
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
    assert(windowsPlan.capture.backend == TunBackendKind::Mihomo);

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

    int disconnectCalls = 0;
    {
        VpnManager scopedManager;
        scopedManager.setAdapters({
            EngineAdapter{
                .descriptor =
                    {EngineKind::SystemPptp, 1, true, {ConnectionKind::Pptp}},
                .connect = [](VpnConnection&, std::string&) { return true; },
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
    std::println("test_vpn: ok");
}
