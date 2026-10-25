#pragma once
#include "../src/network_conflict.h"
#include <algorithm>
#include <stdexcept>
#include <string>
inline void NetworkConflictNamespaceCase(const std::string& mode) {
    using namespace clashflux::network_conflict;
    const auto require=[](bool value,const char* message){if(!value)throw std::runtime_error(message);};
    const auto snapshot=Inspect();
    const auto found=std::find_if(snapshot.interfaces.begin(),snapshot.interfaces.end(),
        [](const auto& link){return link.name=="other-tun";});
    require(found!=snapshot.interfaces.end(),"real namespace TUN missing");
    Request request;request.tuns={{"ClashFlux",{"172.19.0.1/30"}}};
    if(mode=="global"||mode=="halves"||mode=="global6"||mode=="halves6") {
        const bool ipv6=mode=="global6"||mode=="halves6";
        require(found->up&&found->tunnel&&(ipv6?found->capturesIpv6:found->capturesIpv4),
                "real full-tunnel route not recognized");
        require(!Check(snapshot,{}).empty(),"proxy-only route conflict missed");
        request.owned=OwnedInterfaces(snapshot,{{"other-tun",{"172.19.0.1/30"}}});
        require(!request.owned.empty()&&Check(snapshot,request).empty(),"owned live TUN exemption failed");
    } else if(mode=="address") {
        require(found->up&&!found->capturesIpv4&&!found->capturesIpv6,"route cleanup evidence incorrect");
        require(Check(snapshot,{}).empty(),"split route caused false full-tunnel alarm");
        require(!Check(snapshot,request).empty(),"real address collision missed");
    } else if(mode=="down") {
        require(!found->up&&Check(snapshot,request).empty(),"offline adapter caused false alarm");
    } else throw std::runtime_error("unknown namespace test mode");
}
