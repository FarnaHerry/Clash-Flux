#include "network_conflict.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#elif !defined(__ANDROID__) && !defined(CLASHFLUX_IOS)
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/ioctl.h>
#elif defined(__APPLE__)
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/route.h>
#endif
#endif

namespace clashflux::network_conflict {
namespace {
struct Prefix { std::array<unsigned char,16> bytes{}; unsigned bits=0, size=0; };
bool parse(const std::string& text, Prefix& out) {
#if defined(__ANDROID__) || defined(CLASHFLUX_IOS)
    (void)text; (void)out; return false;
#else
    const auto slash=text.find('/'); const auto host=text.substr(0,slash);
    const int family=host.find(':')==std::string::npos?AF_INET:AF_INET6;
    out.size=family==AF_INET?4:16; out.bits=out.size*8;
    if(slash!=std::string::npos) {
        const auto value=text.substr(slash+1);
        const auto [end,error]=std::from_chars(value.data(),value.data()+value.size(),out.bits);
        if(error!=std::errc{}||end!=value.data()+value.size()||out.bits>out.size*8)return false;
    }
#ifdef _WIN32
    return InetPtonA(family,host.c_str(),out.bytes.data())==1;
#else
    return inet_pton(family,host.c_str(),out.bytes.data())==1;
#endif
#endif
}
bool contains(const std::vector<unsigned>& values,unsigned index) {
    return std::find(values.begin(),values.end(),index)!=values.end();
}
bool sameAddress(const std::string& a,const std::string& b) {
    Prefix x,y;return parse(a,x)&&parse(b,y)&&x.size==y.size&&x.bytes==y.bytes;
}
struct Route { unsigned index=0,prefix=128; bool known=false; };
#if !defined(__ANDROID__) && !defined(CLASHFLUX_IOS)
std::string addressText(int family,const void* bytes,unsigned prefix) {
    std::array<char,64> buffer{};
#ifdef _WIN32
    if(!InetNtopA(family,const_cast<void*>(bytes),buffer.data(),buffer.size()))return {};
#else
    if(!inet_ntop(family,bytes,buffer.data(),buffer.size()))return {};
#endif
    return std::string(buffer.data())+"/"+std::to_string(prefix);
}
#ifdef _WIN32
std::string utf8(const wchar_t* text) {
    const int size=WideCharToMultiByte(CP_UTF8,0,text,-1,nullptr,0,nullptr,nullptr);
    if(size<2)return {};
    std::string result(size,'\0');
    WideCharToMultiByte(CP_UTF8,0,text,-1,result.data(),size,nullptr,nullptr);
    result.pop_back();return result;
}
Route lookup(const char* target) {
    SOCKADDR_INET destination{};destination.si_family=std::strchr(target,':')?AF_INET6:AF_INET;
    void* bytes=destination.si_family==AF_INET?static_cast<void*>(&destination.Ipv4.sin_addr):&destination.Ipv6.sin6_addr;
    if(InetPtonA(destination.si_family,target,bytes)!=1)return {};
    MIB_IPFORWARD_ROW2 row{};SOCKADDR_INET source{};
    const auto result=GetBestRoute2(nullptr,0,nullptr,&destination,0,&row,&source);
    if(result==ERROR_NOT_FOUND||result==ERROR_NETWORK_UNREACHABLE)return {0,128,true};
    if(result!=NO_ERROR)return {};
    return {row.InterfaceIndex,row.DestinationPrefix.PrefixLength,true};
}
#else
struct Fd { int value; ~Fd(){if(value>=0)close(value);} Fd(const Fd&)=delete; explicit Fd(int v):value(v){} };
Route lookup(const char* target) {
    const int family=std::strchr(target,':')?AF_INET6:AF_INET;
#ifdef __linux__
    struct Query { nlmsghdr header;rtmsg route;rtattr attribute;std::array<unsigned char,16> bytes; } query{};
    const unsigned size=family==AF_INET?4:16;
    if(inet_pton(family,target,query.bytes.data())!=1)return {};
    query.header.nlmsg_len=NLMSG_LENGTH(sizeof(rtmsg))+RTA_LENGTH(size);
    query.header.nlmsg_type=RTM_GETROUTE;query.header.nlmsg_flags=NLM_F_REQUEST;query.header.nlmsg_seq=1;
    query.route.rtm_family=family;query.route.rtm_dst_len=size*8;
    query.route.rtm_flags=RTM_F_FIB_MATCH;
    query.attribute.rta_type=RTA_DST;query.attribute.rta_len=RTA_LENGTH(size);
    const Fd fd{socket(AF_NETLINK,SOCK_DGRAM|SOCK_CLOEXEC,NETLINK_ROUTE)};
    if(fd.value<0)return {};
    sockaddr_nl kernel{};kernel.nl_family=AF_NETLINK;
    if(sendto(fd.value,&query,query.header.nlmsg_len,0,reinterpret_cast<sockaddr*>(&kernel),sizeof(kernel))<0)return {};
    pollfd descriptor{fd.value,POLLIN,0};if(poll(&descriptor,1,100)<=0)return {};
    alignas(nlmsghdr) std::array<char,16384> buffer{};
    int left=static_cast<int>(recv(fd.value,buffer.data(),buffer.size(),0));
    for(auto h=reinterpret_cast<nlmsghdr*>(buffer.data());NLMSG_OK(h,left);h=NLMSG_NEXT(h,left)) {
        if(h->nlmsg_seq!=1)continue;
        if(h->nlmsg_type==NLMSG_ERROR&&h->nlmsg_len>=NLMSG_LENGTH(sizeof(nlmsgerr))) {
            const int error=reinterpret_cast<nlmsgerr*>(NLMSG_DATA(h))->error;
            return error==-ENETUNREACH||error==-ESRCH?Route{0,128,true}:Route{};
        }
        if(h->nlmsg_type!=RTM_NEWROUTE||h->nlmsg_len<NLMSG_LENGTH(sizeof(rtmsg)))continue;
        auto route=reinterpret_cast<rtmsg*>(NLMSG_DATA(h));int length=RTM_PAYLOAD(h);
        if(route->rtm_type!=RTN_UNICAST)return {0,128,true};
        for(auto a=RTM_RTA(route);RTA_OK(a,length);a=RTA_NEXT(a,length))
            if(a->rta_type==RTA_OIF&&RTA_PAYLOAD(a)>=sizeof(unsigned)) {
                unsigned index=0;std::memcpy(&index,RTA_DATA(a),sizeof(index));
                return {index,route->rtm_dst_len,true};
            }
    }
#elif defined(__APPLE__)
    struct Query { rt_msghdr header;sockaddr_in6 destination; } query{};
    static_assert(offsetof(Query,destination)==sizeof(rt_msghdr));
    query.header.rtm_version=RTM_VERSION;query.header.rtm_type=RTM_GET;
    static std::atomic<int> sequence{1};
    query.header.rtm_addrs=RTA_DST;query.header.rtm_seq=sequence.fetch_add(1);query.header.rtm_pid=getpid();
    auto* destination=reinterpret_cast<sockaddr*>(&query.destination);destination->sa_family=family;
    destination->sa_len=family==AF_INET?sizeof(sockaddr_in):sizeof(sockaddr_in6);
    void* bytes=family==AF_INET?static_cast<void*>(&reinterpret_cast<sockaddr_in*>(destination)->sin_addr)
        :&reinterpret_cast<sockaddr_in6*>(destination)->sin6_addr;
    if(inet_pton(family,target,bytes)!=1)return {};
    query.header.rtm_msglen=sizeof(rt_msghdr)+destination->sa_len;
    const Fd fd{socket(PF_ROUTE,SOCK_RAW,family)};if(fd.value<0)return {};
    if(write(fd.value,&query,query.header.rtm_msglen)!=query.header.rtm_msglen)return {};
    for(unsigned attempt=0;attempt<4;++attempt) {
        pollfd descriptor{fd.value,POLLIN,0};if(poll(&descriptor,1,50)<=0)return {};
        alignas(rt_msghdr) std::array<char,16384> buffer{};
        const auto size=read(fd.value,buffer.data(),buffer.size());if(size<static_cast<ssize_t>(sizeof(rt_msghdr)))return {};
        const auto* h=reinterpret_cast<const rt_msghdr*>(buffer.data());
        if(h->rtm_pid!=getpid()||h->rtm_seq!=query.header.rtm_seq||h->rtm_type!=RTM_GET)continue;
        if(h->rtm_errno==ESRCH||h->rtm_errno==ENETUNREACH)return {0,128,true};
        if(h->rtm_errno||h->rtm_msglen<sizeof(rt_msghdr)||h->rtm_msglen>size)return {};
        if(!(h->rtm_flags&RTF_UP)||(h->rtm_flags&(RTF_REJECT|RTF_BLACKHOLE)))return {0,128,true};
        unsigned prefix=(h->rtm_flags&RTF_HOST)?(family==AF_INET?32:128):0;
        const char* cursor=buffer.data()+sizeof(rt_msghdr);
        const char* end=buffer.data()+h->rtm_msglen;
        for(unsigned bit=1;bit<=RTA_BRD;bit<<=1)if(h->rtm_addrs&bit) {
            if(end-cursor<2)return {};
            const auto* value=reinterpret_cast<const sockaddr*>(cursor);
            // Darwin routing sockaddrs stay 32-bit aligned on 64-bit hosts.
            constexpr std::size_t alignment=sizeof(std::uint32_t);
            const std::size_t step=value->sa_len?((value->sa_len+alignment-1)&~(alignment-1)):alignment;
            if(step>static_cast<std::size_t>(end-cursor))return {};
            if(bit==RTA_NETMASK) {
                prefix=0;
                const auto offset=family==AF_INET?offsetof(sockaddr_in,sin_addr):offsetof(sockaddr_in6,sin6_addr);
                bool zero=false;
                for(unsigned i=0;i<(family==AF_INET?4:16);++i) {
                    const auto byte=offset+i<value->sa_len?static_cast<unsigned char>(cursor[offset+i]):0;
                    for(unsigned mask=128;mask;mask>>=1) {
                        if(byte&mask){if(zero)return {}; ++prefix;}else zero=true;
                    }
                }
            }
            cursor+=step;
        }
        return {h->rtm_index,prefix,true};
    }
#endif
    return {};
}
#endif
#endif
} // namespace

bool Overlaps(const std::string& a,const std::string& b) {
    Prefix x,y;if(!parse(a,x)||!parse(b,y)||x.size!=y.size)return false;
    const unsigned bits=std::min(x.bits,y.bits);
    for(unsigned i=0;i<bits;++i)if((x.bytes[i/8]&(128>>(i%8)))!=(y.bytes[i/8]&(128>>(i%8))))return false;
    return true;
}
std::vector<unsigned> OwnedInterfaces(const Snapshot& snapshot,const std::vector<Tun>& tuns) {
    std::vector<unsigned> result;
    for(const auto& tun:tuns) {
        unsigned match=0;bool ambiguous=false;
        for(const auto& link:snapshot.interfaces) {
            if(!link.up||!link.tunnel||(!tun.name.empty()&&tun.name!=link.name)||tun.addresses.empty())continue;
            if(!std::all_of(tun.addresses.begin(),tun.addresses.end(),[&](const auto& address){
                return std::any_of(link.addresses.begin(),link.addresses.end(),[&](const auto& actual){return sameAddress(address,actual);});
            }))continue;
            if(match&&match!=link.index)ambiguous=true;
            match=link.index;
        }
        if(match&&!ambiguous)result.push_back(match);
    }
    return result;
}
std::string Check(const Snapshot& snapshot,const Request& request) {
    for(const auto& link:snapshot.interfaces) {
        if(!link.up||contains(request.owned,link.index))continue;
        for(const auto& tun:request.tuns) {
            if(!tun.name.empty()&&tun.name==link.name)
                return "TUN 接口冲突：活动接口「"+link.name+"」已被占用；请先关闭对应 VPN/TUN 后重试";
            for(const auto& wanted:tun.addresses)for(const auto& actual:link.addresses)
                if(Overlaps(wanted,actual))return "TUN 地址冲突："+wanted+" 与活动接口「"+link.name+"」的 "+actual+
                    " 重叠；请先关闭对应 VPN/TUN 或调整其网段后重试";
        }
        if((request.proxyListening||!request.tuns.empty())&&
           link.tunnel&&(link.capturesIpv4||link.capturesIpv6)&&
           std::find(request.nativeInterfaces.begin(),request.nativeInterfaces.end(),link.name)==request.nativeInterfaces.end())
            return "网络接管冲突：其他活动隧道「"+link.name+"」正在接管 "+
                (link.capturesIpv4?"IPv4":"IPv6")+" 默认路由；请先关闭对应 VPN/TUN 后重试（系统代理出口也可能经过它）";
    }
    return {};
}

Snapshot Inspect() {
    Snapshot result;
#if !defined(__ANDROID__) && !defined(CLASHFLUX_IOS)
#ifdef _WIN32
    PMIB_IF_TABLE2 raw=nullptr;
    if(GetIfTable2(&raw)!=NO_ERROR)return result;
    const std::unique_ptr<MIB_IF_TABLE2,decltype(&FreeMibTable)> rows(raw,&FreeMibTable);
    PMIB_UNICASTIPADDRESS_TABLE addressesRaw=nullptr;
    if(GetUnicastIpAddressTable(AF_UNSPEC,&addressesRaw)!=NO_ERROR)return result;
    const std::unique_ptr<MIB_UNICASTIPADDRESS_TABLE,decltype(&FreeMibTable)> addresses(addressesRaw,&FreeMibTable);
    for(ULONG i=0;i<rows->NumEntries;++i) {
        const auto& row=rows->Table[i];Interface link;
        link.index=row.InterfaceIndex;link.name=utf8(row.Alias);link.up=row.OperStatus==IfOperStatusUp;
        // Generic software tunnels qualify; automatic IPv6 transition adapters
        // (Teredo/6to4/ISATAP) are not another proxy client's global TUN.
        link.tunnel=row.Type==IF_TYPE_PROP_VIRTUAL || (row.Type==IF_TYPE_TUNNEL &&
            (row.TunnelType==TUNNEL_TYPE_OTHER||row.TunnelType==TUNNEL_TYPE_NONE));
        for(ULONG j=0;j<addresses->NumEntries;++j) {
            const auto& address=addresses->Table[j];if(address.InterfaceIndex!=link.index||address.DadState!=IpDadStatePreferred)continue;
            const auto family=address.Address.si_family;
            const void* bytes=family==AF_INET?static_cast<const void*>(&address.Address.Ipv4.sin_addr):&address.Address.Ipv6.sin6_addr;
            if(family==AF_INET||family==AF_INET6)link.addresses.push_back(addressText(family,bytes,address.OnLinkPrefixLength));
        }
        result.interfaces.push_back(std::move(link));
    }
#else
    ifaddrs* raw=nullptr;if(getifaddrs(&raw)!=0)return result;
    const std::unique_ptr<ifaddrs,decltype(&freeifaddrs)> rows(raw,&freeifaddrs);
    for(auto row=raw;row;row=row->ifa_next) {
        if(!row->ifa_name)continue;
        const unsigned index=if_nametoindex(row->ifa_name);if(!index)continue;
        auto found=std::find_if(result.interfaces.begin(),result.interfaces.end(),[&](const auto& link){return link.index==index;});
        if(found==result.interfaces.end()) {
            Interface link;link.index=index;link.name=row->ifa_name;
            result.interfaces.push_back(std::move(link));found=std::prev(result.interfaces.end());
        }
        found->up=(row->ifa_flags&IFF_UP)&&(row->ifa_flags&IFF_RUNNING);
#ifdef __linux__
        std::ifstream flags("/sys/class/net/"+found->name+"/tun_flags");
        std::ifstream type("/sys/class/net/"+found->name+"/type");unsigned value=0;type>>value;
        found->tunnel=found->tunnel||flags.good()||value==65534;
#elif defined(__APPLE__)
        if(row->ifa_addr&&row->ifa_addr->sa_family==AF_LINK) {
            const auto type=reinterpret_cast<sockaddr_dl*>(row->ifa_addr)->sdl_type;
            found->tunnel=(row->ifa_flags&IFF_POINTOPOINT)&&type!=IFT_PPP;
        }
#endif
        if(!row->ifa_addr||!row->ifa_netmask)continue;
        const int family=row->ifa_addr->sa_family;if(family!=AF_INET&&family!=AF_INET6)continue;
        const void* bytes=family==AF_INET?static_cast<void*>(&reinterpret_cast<sockaddr_in*>(row->ifa_addr)->sin_addr):&reinterpret_cast<sockaddr_in6*>(row->ifa_addr)->sin6_addr;
        const auto* mask=family==AF_INET?reinterpret_cast<unsigned char*>(&reinterpret_cast<sockaddr_in*>(row->ifa_netmask)->sin_addr):reinterpret_cast<unsigned char*>(&reinterpret_cast<sockaddr_in6*>(row->ifa_netmask)->sin6_addr);
        unsigned prefix=0;for(unsigned i=0;i<(family==AF_INET?4:16);++i)for(unsigned bit=128;bit;bit>>=1)if(mask[i]&bit)++prefix;
        found->addresses.push_back(addressText(family,bytes,prefix));
    }
#ifdef __linux__
    // A TUN without a hardware address can have a null AF_PACKET sockaddr in
    // getifaddrs. Query its kernel link type in this namespace, rather than
    // relying solely on possibly bind-mounted or unavailable sysfs metadata.
    const Fd control{socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0)};
    if(control.value>=0)for(auto& link:result.interfaces) {
        ifreq request{};
        std::strncpy(request.ifr_name,link.name.c_str(),IFNAMSIZ-1);
        if(ioctl(control.value,SIOCGIFHWADDR,&request)==0&&request.ifr_hwaddr.sa_family==65534)
            link.tunnel=true;
    }
#endif
#endif
    result.interfacesKnown=true;
    const auto low4=lookup("1.0.0.1"),high4=lookup("200.0.0.1");
    const auto low6=lookup("2001:db8::1"),high6=lookup("8000::1");
    result.routesKnown=low4.known&&high4.known&&low6.known&&high6.known;
    for(auto& link:result.interfaces) {
        link.capturesIpv4=low4.known&&high4.known&&low4.index==link.index&&high4.index==link.index&&low4.prefix<=1&&high4.prefix<=1;
        link.capturesIpv6=low6.known&&high6.known&&low6.index==link.index&&high6.index==link.index&&low6.prefix<=1&&high6.prefix<=1;
    }
#endif
    return result;
}
} // namespace clashflux::network_conflict
