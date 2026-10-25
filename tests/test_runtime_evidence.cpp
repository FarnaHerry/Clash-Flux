#include "../src/runtime_network_probe.h"
#include "../src/health_history.h"
#include "../src/ui/diagnostic_export_file.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <thread>
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
#else
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
using namespace std::chrono_literals;
using namespace clashflux;
using network_probe::Evidence;
void require(bool value,const char* what){if(!value)throw std::runtime_error(what);}
std::string read(const std::filesystem::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
struct TestRoot {
    std::filesystem::path path;
    TestRoot(){for(unsigned i=0;i<8;++i){path=std::filesystem::temp_directory_path()/
        ("clashflux-health-evidence-"+std::to_string(std::random_device{}()));
        if(std::filesystem::create_directory(path))return;}throw std::runtime_error("Cannot allocate test directory");}
    ~TestRoot(){std::error_code ec;std::filesystem::remove_all(path,ec);}
};
#ifdef _WIN32
using SocketValue=SOCKET;constexpr auto InvalidSocket=INVALID_SOCKET;
void closeSocket(SocketValue s){closesocket(s);}
std::string loopbackName(){SOCKADDR_INET address{};address.si_family=AF_INET;InetPtonA(AF_INET,"127.0.0.1",&address.Ipv4.sin_addr);
    MIB_IPFORWARD_ROW2 route{};SOCKADDR_INET source{};
    require(GetBestRoute2(nullptr,0,nullptr,&address,0,&route,&source)==0,"loopback route missing");
    wchar_t name[IF_MAX_STRING_SIZE+1]{};require(ConvertInterfaceLuidToAlias(&route.InterfaceLuid,name,IF_MAX_STRING_SIZE+1)==0,"loopback alias missing");
    char text[1024]{};WideCharToMultiByte(CP_UTF8,0,name,-1,text,sizeof(text),nullptr,nullptr);return text;}
#else
using SocketValue=int;constexpr auto InvalidSocket=-1;
void closeSocket(SocketValue s){close(s);}
std::string loopbackName(){return if_nametoindex("lo")?"lo":"lo0";}
#endif
struct DnsFixture {
    SocketValue socket=InvalidSocket;
    std::jthread worker;
    unsigned short port=0;
    std::atomic<int> mode{0},requests{0};
    DnsFixture(){
        socket=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);require(socket!=InvalidSocket,"UDP fixture socket");
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        require(bind(socket,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"UDP fixture bind");
#ifdef _WIN32
        int size=sizeof(address);
#else
        socklen_t size=sizeof(address);
#endif
        require(getsockname(socket,reinterpret_cast<sockaddr*>(&address),&size)==0,"UDP fixture port");port=ntohs(address.sin_port);
        worker=std::jthread([this](std::stop_token stop){while(!stop.stop_requested()){
            fd_set set;FD_ZERO(&set);FD_SET(socket,&set);timeval timeout{0,20000};
            if(select(static_cast<int>(socket+1),&set,nullptr,nullptr,&timeout)<=0)continue;
            std::vector<std::uint8_t> packet(4096);sockaddr_in peer{};
#ifdef _WIN32
            int length=sizeof(peer);
#else
            socklen_t length=sizeof(peer);
#endif
            const auto n=recvfrom(socket,reinterpret_cast<char*>(packet.data()),static_cast<int>(packet.size()),0,reinterpret_cast<sockaddr*>(&peer),&length);
            if(n<12)continue;++requests;packet.resize(n);const auto kind=mode.load();if(kind==4)continue;
            packet[2]=0x81;packet[3]=kind==1?0x83:kind==3?0x82:0x80;
            if(kind==2)packet[0]^=1;
            sendto(socket,reinterpret_cast<const char*>(packet.data()),static_cast<int>(packet.size()),0,reinterpret_cast<sockaddr*>(&peer),length);
        }});
    }
    ~DnsFixture(){worker.request_stop();worker.join();if(socket!=InvalidSocket)closeSocket(socket);}
};
int main(){try{
#ifdef _WIN32
    WSADATA data{};require(WSAStartup(MAKEWORD(2,2),&data)==0,"winsock fixture init");
    struct WinsockGuard{~WinsockGuard(){WSACleanup();}} guard;
#endif
    const auto question=network_probe::DnsQuestion(0x1234);
    auto reply=question;reply[2]=0x81;reply[3]=0x80;
    require(network_probe::ValidateDns(question,reply)==Evidence::Response,"valid DNS reply rejected");
    auto bad=reply;bad[0]^=1;require(network_probe::ValidateDns(question,bad)==Evidence::InvalidResponse,"wrong DNS id accepted");
    bad=reply;bad[14]^=1;require(network_probe::ValidateDns(question,bad)==Evidence::InvalidResponse,"wrong DNS question accepted");
    bad=reply;bad[2]|=2;require(network_probe::ValidateDns(question,bad)==Evidence::InvalidResponse,"truncated DNS accepted");
    bad=reply;bad[7]=1;require(network_probe::ValidateDns(question,bad)==Evidence::InvalidResponse,"missing DNS payload accepted");
    bad=reply;bad[7]=1;bad.insert(bad.end(),{0xc0,static_cast<std::uint8_t>(reply.size()),0,1,0,1,0,0,0,1,0,0});
    require(network_probe::ValidateDns(question,bad)==Evidence::InvalidResponse,"cyclic DNS pointer accepted");
    DnsFixture server;
    network_probe::Request request;request.interfaceName=loopbackName();request.address="127.0.0.1";
    request.routeDestination=request.dnsAddress="127.0.0.1";request.dnsPort=server.port;request.dnsRoundTrip=true;
    const auto healthy=network_probe::Inspect(request);
#if defined(_WIN32) || defined(__linux__)
    const bool routeEvidence=healthy.route==Evidence::ViaTun&&healthy.dnsRoute==Evidence::ViaTun;
    // Route backend exists: the production gate only probes DNS via the TUN.
    const bool dnsRoundTrip=true;
#else
    // The production route reader currently has no macOS route-table backend;
    // both route fields explicitly report Unsupported there, and the
    // production gate conservatively skips the probe (no physical-network leak).
    const bool routeEvidence=healthy.route==Evidence::Unsupported&&healthy.dnsRoute==Evidence::Unsupported;
    const bool dnsRoundTrip=false;
#endif
    require(healthy.interface==Evidence::Present&&routeEvidence&&
        healthy.dns==(dnsRoundTrip?Evidence::Response:Evidence::NotChecked),
        "real loopback OS route or bound DNS probe failed");
    if(dnsRoundTrip){
    server.mode=1;require(network_probe::Inspect(request).dns==Evidence::NegativeResponse,"negative DNS response misclassified");
    server.mode=2;require(network_probe::Inspect(request).dns==Evidence::InvalidResponse,"spoofed DNS response accepted");
    server.mode=3;require(network_probe::Inspect(request).dns==Evidence::ServerError,"DNS failure reported healthy");
    server.mode=4;require(network_probe::Inspect(request,{},80ms).dns==Evidence::Timeout,"silent DNS not bounded");
    std::stop_source stop;std::jthread cancel([&]{std::this_thread::sleep_for(30ms);stop.request_stop();});
    const auto began=std::chrono::steady_clock::now();
    require(network_probe::Inspect(request,stop.get_token()).dns==Evidence::Cancelled,"DNS cancellation ignored");
    require(std::chrono::steady_clock::now()-began<400ms,"DNS cancellation slow");
    }
    const auto count=server.requests.load();request.interfaceName="ClashFluxHealthMissingInterface";
    const auto missing=network_probe::Inspect(request);
    require(missing.interface!=Evidence::Present&&missing.dns==Evidence::NotChecked&&server.requests==count,"missing interface leaked DNS");
    request.interfaceName=loopbackName();request.dnsRoundTrip=false;
    require(network_probe::Inspect(request).dns==Evidence::NotChecked&&server.requests==count,"background observation sent DNS");

    TestRoot root;health::History history;health::HistorySample sample;sample.session=7;sample.lifecycle=2;sample.tunIntent=true;
    sample.tunRunning=true;sample.network=healthy;sample.streams.traffic.state=health::State::Fresh;
    for(unsigned i=0;i<500;++i){sample.at=10000+1000*i;sample.epoch=1+i/100;
        sample.streams.traffic.valid=i;sample.streams.traffic.consumed=i?i-1:0;
        sample.streams.traffic.state=i==200?health::State::Stale:health::State::Fresh;
        require(history.Store(root.path,sample),"health history write failed");}
    require(std::filesystem::file_size(root.path/"health-history.txt")<=128*1024,"history file unbounded");
    const auto report=history.Report(root.path);
    require(report.size()<96*1024&&report.find("traffic=stale")!=std::string::npos,"incident lost to periodic ring");
    health::History reopened;require(reopened.Report(root.path)==report,"history not preserved across reopen");
    const auto file=root.path/"health-history.txt",backup=root.path/"history-backup.txt";
    std::filesystem::rename(file,backup);std::filesystem::create_directory(file);
    sample.at+=6000;sample.lifecycle=3;
    require(!history.Store(root.path,sample)&&history.Report(root.path).find("persisted=no")!=std::string::npos,"write failure hidden");
    std::filesystem::remove(file);std::filesystem::rename(backup,file);sample.at+=6000;
    require(history.Store(root.path,sample),"dirty history not retried");
    health::History retry;require(retry.Report(root.path).find("lifecycle=failed")!=std::string::npos,"failed write lost after retry");
    const auto blocked=root.path/"blocked";
    {std::ofstream out(blocked);out<<"fixture";}
    health::History initiallyBlocked;sample.session=9;sample.at+=6000;
    require(!initiallyBlocked.Store(blocked,sample)&&initiallyBlocked.Report(blocked).find("session=9")!=std::string::npos,
        "initial storage failure lost memory evidence");
    std::filesystem::remove(blocked);std::filesystem::create_directory(blocked);sample.session=10;sample.at+=6000;
    require(initiallyBlocked.Store(blocked,sample),"initial storage failure never recovered");
    health::History recovered;
    require(recovered.Report(blocked).find("session=9")!=std::string::npos&&recovered.Report(blocked).find("session=10")!=std::string::npos,
        "recovery dropped pending session evidence");
    {std::ofstream out(file,std::ios::app);out<<"Eendpoint=https://secret.invalid/?token=private\n";}
    health::History redacted;require(redacted.Report(root.path).find("secret.invalid")==std::string::npos,"untrusted history text exported");
    auto exportFile=ui::DiagnosticExportFile::Prepare(root.path,report);const auto path=exportFile->Path();
    require(read(path)==report,"export changed report");auto second=ui::DiagnosticExportFile::Prepare(root.path,report);
    require(second->Path()!=path,"concurrent export overwrite");exportFile.reset();
    require(!std::filesystem::exists(path)&&std::filesystem::exists(second->Path()),"export ownership cleanup failed");
    std::cout<<"runtime_evidence: OS routes, DNS faults/cancel, bounded history/reopen/retry/redaction/export passed\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
