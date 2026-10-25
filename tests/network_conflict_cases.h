#pragma once
#include "../src/network_conflict.h"
#include <stdexcept>
inline void NetworkConflictCases() {
    using namespace clashflux::network_conflict;
    const auto require=[](bool condition,const char* text){if(!condition)throw std::runtime_error(text);};
    require(Overlaps("172.19.0.1/30","172.19.0.2/30"),"TUN subnet collision missed");
    require(Overlaps("10.23.0.1/30","10.0.0.1/8"),"LAN overlap missed");
    require(!Overlaps("172.19.0.1/30","172.19.0.5/30"),"adjacent subnets collided");
    require(Overlaps("fd00:1234::1/126","fd00::1/8"),"IPv6 overlap missed");
    require(!Overlaps("fd00::1/126","fd00::5/126"),"IPv6 adjacent subnets collided");
    require(!Overlaps("10.0.0.1/33","10.0.0.1/8")&&!Overlaps("fd00::1/129","fd00::/8"),"invalid prefix accepted");
    require(!Overlaps("172.19.0.1/30","::ffff:172.19.0.1/128"),"different address families conflated");
    Snapshot snapshot{true,true,{{12,"tun0",true,true,{"172.19.0.1/30"},true,false}}};
    Request request;request.tuns={{"ClashFlux",{"172.19.0.1/30"}}};
    require(Check(snapshot,request).find("tun0")!=std::string::npos,"confirmed address conflict lacks interface");
    auto ignored=snapshot;ignored.interfaces.front().up=false;
    require(Check(ignored,request).empty(),"inactive residual adapter blocked startup");
    request.owned={12};require(Check(snapshot,request).empty(),"authenticated own TUN blocked restart");
    request.owned.clear();request.tuns={{"tun0",{"10.255.255.1/30"}}};
    require(Check(snapshot,request).find("接口冲突")!=std::string::npos,"occupied explicit interface name accepted");
    request.tuns.clear();
    require(!Check(snapshot,request).empty(),"proxy-only startup missed other full-tunnel routing");
    request.nativeInterfaces={"tun0"};
    require(Check(snapshot,request).empty(),"explicit native upstream was rejected");
    request.tuns={{"ClashFlux",{"172.19.0.1/30"}}};
    require(!Check(snapshot,request).empty(),"native allowlist concealed address overlap");
    snapshot.interfaces.front().capturesIpv4=false;request.nativeInterfaces.clear();request.tuns.clear();
    require(Check(snapshot,request).empty(),"split-tunnel enterprise VPN blocked without collision");
    snapshot.interfaces.front().capturesIpv6=true;
    require(Check(snapshot,request).find("IPv6")!=std::string::npos,"IPv6-only full tunnel missed");
    snapshot.interfaces.front().tunnel=false;
    require(Check(snapshot,request).empty(),"physical default route mistaken for VPN");
    snapshot.interfaces.front().tunnel=true;
    const std::vector<Tun> old{{"",{"172.19.0.1/30"}}};
    require(OwnedInterfaces(snapshot,old)==std::vector<unsigned>{12},"unique old-plan address not resolved");
    snapshot.interfaces.push_back({13,"tun1",true,true,{"172.19.0.1/30"}});
    require(OwnedInterfaces(snapshot,old).empty(),"ambiguous address granted ownership");
    require(OwnedInterfaces(snapshot,{{"tun0",{"10.0.0.1/30"}}}).empty(),"name alone granted ownership");
    require(Check({},request).empty(),"unknown OS evidence fabricated a conflict");
    const auto live=Inspect();require(live.interfacesKnown,"live OS interface inspection failed");
    bool verified=false;
    for(const auto& link:live.interfaces)if(link.up&&!link.addresses.empty()) {
        Request collision;collision.tuns={{"",{link.addresses.front()}}};
        require(!Check(live,collision).empty(),"live adapter address not visible to production conflict check");
        verified=true;break;
    }
    require(verified,"live OS inspection returned no active address, including loopback");
}
