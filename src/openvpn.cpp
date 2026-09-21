// sing-box owns OpenVPN sessions; this unit only supplies the adapter contract.
module clashflux.openvpn;

import std;
import clashflux.vpn;

namespace openvpn {

vpn::EngineAdapter MakeOpenVpnAdapter() {
    vpn::EngineAdapter adapter{
        .descriptor = vpn::EngineDescriptor{
            .kind = vpn::EngineKind::SingBox,
            .priority = 95,
            .available = true,
            .connectionKinds = {vpn::ConnectionKind::OpenVpn},
        },
    };
    adapter.connect = [](vpn::VpnConnection&, std::string& error) {
        error.clear();
        return true;
    };
    return adapter;
}

} // namespace openvpn
