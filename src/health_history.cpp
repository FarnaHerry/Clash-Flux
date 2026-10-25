#include "health_history.h"
#include <array>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <random>
#include <sstream>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace clashflux::health {
namespace {
constexpr std::size_t MaxBytes = 128 * 1024;
using Fields = std::array<std::uint64_t, 33>;
std::uint64_t age(std::int64_t value) { return value < 0 ? 0 : std::min<std::uint64_t>(value,86400000) + 1; }
Fields fields(const HistorySample& s) {
    return {1,s.at,s.session,s.epoch,s.instance,s.revision,s.lifecycle,
        s.tunIntent,s.tunRunning,s.proxyIntent,
        static_cast<unsigned>(s.streams.control),static_cast<unsigned>(s.streams.nativeControl),
        static_cast<unsigned>(s.streams.traffic.state),static_cast<unsigned>(s.streams.connections.state),
        s.streams.traffic.generation,s.streams.connections.generation,
        age(s.streams.traffic.validAgeMs),age(s.streams.connections.validAgeMs),
        s.streams.traffic.valid,s.streams.traffic.consumed,s.streams.traffic.invalid,
        s.streams.connections.valid,s.streams.connections.consumed,s.streams.connections.invalid,
        static_cast<unsigned>(s.network.interface),static_cast<unsigned>(s.network.route),
        static_cast<unsigned>(s.network.proxy),age(s.networkAgeMs),
        s.pptpConnected,s.pptpUnknown,static_cast<unsigned>(s.network.dns),
        age(s.streams.controlAgeMs),age(s.streams.nativeAgeMs)};
}
bool changed(const HistorySample& a, const HistorySample& b) {
    auto x=fields(a),y=fields(b);
    for (unsigned i : {2u,3u,4u,5u,6u,7u,8u,9u,10u,11u,12u,13u,24u,25u,26u,28u,29u,30u})
        if(x[i]!=y[i])return true;
    return false;
}
std::string encode(const HistorySample& sample) {
    std::string line;
    for(auto n:fields(sample)) { line+=std::to_string(n);line+=' '; }
    line.back()='\n';return line;
}
bool decode(std::string_view line, HistorySample& s) {
    Fields f{};std::size_t at=0;
    for(auto& n:f) {
        while(at<line.size()&&line[at]==' ')++at;
        const auto result=std::from_chars(line.data()+at,line.data()+line.size(),n);
        if(result.ec!=std::errc{}||result.ptr==line.data()+at)return false;
        at=static_cast<std::size_t>(result.ptr-line.data());
    }
    if(at!=line.size()||f[0]!=1||f[6]>3||f[7]>1||f[8]>1||f[9]>1||f[10]>6||f[11]>6||
        f[12]>6||f[13]>6||f[24]>14||f[25]>14||f[26]>4||f[27]>86400001||f[30]>14||
        f[31]>86400001||f[32]>86400001||
        f[16]>86400001||f[17]>86400001||f[28]>4096||f[29]>4096)return false;
    s.at=f[1];s.session=f[2];s.epoch=f[3];s.instance=f[4];s.revision=f[5];s.lifecycle=static_cast<unsigned>(f[6]);
    s.tunIntent=f[7];s.tunRunning=f[8];s.proxyIntent=f[9];
    s.streams.control=static_cast<Control>(f[10]);s.streams.nativeControl=static_cast<Control>(f[11]);
    s.streams.traffic.state=static_cast<State>(f[12]);s.streams.connections.state=static_cast<State>(f[13]);
    s.streams.traffic.generation=f[14];s.streams.connections.generation=f[15];
    s.streams.traffic.validAgeMs=static_cast<std::int64_t>(f[16])-1;
    s.streams.connections.validAgeMs=static_cast<std::int64_t>(f[17])-1;
    s.streams.traffic.valid=f[18];s.streams.traffic.consumed=f[19];s.streams.traffic.invalid=f[20];
    s.streams.connections.valid=f[21];s.streams.connections.consumed=f[22];s.streams.connections.invalid=f[23];
    s.network.interface=static_cast<network_probe::Evidence>(f[24]);s.network.route=static_cast<network_probe::Evidence>(f[25]);
    s.network.proxy=static_cast<network_probe::Proxy>(f[26]);s.networkAgeMs=static_cast<std::int64_t>(f[27])-1;
    s.pptpConnected=static_cast<unsigned>(f[28]);s.pptpUnknown=static_cast<unsigned>(f[29]);
    s.network.dns=static_cast<network_probe::Evidence>(f[30]);
    s.streams.controlAgeMs=static_cast<std::int64_t>(f[31])-1;s.streams.nativeAgeMs=static_cast<std::int64_t>(f[32])-1;
    return true;
}
std::string describe(const HistorySample& s) {
    static constexpr std::array names{"stopped","starting","running","failed"};
    return "at="+std::to_string(s.at)+" session="+std::to_string(s.session)+" epoch="+std::to_string(s.epoch)+
        " instance="+std::to_string(s.instance)+" revision="+std::to_string(s.revision)+
        " lifecycle="+names[std::min(s.lifecycle,3u)]+" tun-intent="+(s.tunIntent?"on":"off")+
        " tun-running="+(s.tunRunning?"on":"off")+" proxy-intent="+(s.proxyIntent?"on":"off")+
        " control="+std::string(Name(s.streams.control))+" native="+std::string(Name(s.streams.nativeControl))+
        " control-age="+std::to_string(s.streams.controlAgeMs)+" native-age="+std::to_string(s.streams.nativeAgeMs)+
        " traffic="+std::string(Name(s.streams.traffic.state))+" connections="+std::string(Name(s.streams.connections.state))+
        " traffic-age="+std::to_string(s.streams.traffic.validAgeMs)+" traffic-valid="+std::to_string(s.streams.traffic.valid)+
        " traffic-consumed="+std::to_string(s.streams.traffic.consumed)+" traffic-invalid="+std::to_string(s.streams.traffic.invalid)+
        " connection-age="+std::to_string(s.streams.connections.validAgeMs)+" connection-valid="+std::to_string(s.streams.connections.valid)+
        " connection-consumed="+std::to_string(s.streams.connections.consumed)+" connection-invalid="+std::to_string(s.streams.connections.invalid)+
        " os-interface="+std::string(network_probe::Name(s.network.interface))+" os-route-sample="+
        std::string(network_probe::Name(s.network.route))+" manual-proxy="+std::string(network_probe::Name(s.network.proxy))+
        " os-age="+std::to_string(s.networkAgeMs)+" pptp-connected="+std::to_string(s.pptpConnected)+
        " pptp-unknown="+std::to_string(s.pptpUnknown)+" dns="+std::string(network_probe::Name(s.network.dns))+"\n";
}
}
bool History::Load(const std::filesystem::path& directory) {
    if(loaded_)return true;
    file_=directory/"health-history.txt";
    std::deque<HistorySample> loadedRecent,loadedIncidents;
    std::error_code ec;
    const auto size=std::filesystem::file_size(file_,ec);
    if(!ec&&size<=MaxBytes) {
        std::ifstream input(file_,std::ios::binary);
        if(!input)return false;
        std::string bytes(MaxBytes+1,'\0');input.read(bytes.data(),static_cast<std::streamsize>(bytes.size()));
        bytes.resize(static_cast<std::size_t>(input.gcount()));
        if(input.bad()||bytes.size()>MaxBytes)return false;
        std::istringstream lines(bytes);std::string line;
        while(std::getline(lines,line)) {
            if(line.empty()||line.size()>1024)continue;
            const bool event=line.front()=='E';
            if(!event&&line.front()!='S')continue;
            HistorySample sample;
            if(!decode(std::string_view(line).substr(1),sample))continue;
            auto& rows=event?loadedIncidents:loadedRecent;
            rows.push_back(sample);
            if(rows.size()>(event?64u:128u))rows.pop_front();
        }
    } else if(ec&&ec!=std::errc::no_such_file_or_directory)return false;
    loadedRecent.insert(loadedRecent.end(),recent_.begin(),recent_.end());
    loadedIncidents.insert(loadedIncidents.end(),incidents_.begin(),incidents_.end());
    while(loadedRecent.size()>128)loadedRecent.pop_front();
    while(loadedIncidents.size()>64)loadedIncidents.pop_front();
    recent_=std::move(loadedRecent);incidents_=std::move(loadedIncidents);
    loaded_=true;return true;
}
bool History::Flush() {
    struct Temporary {
        std::filesystem::path path,directory;
        ~Temporary(){std::error_code ec;if(!path.empty())std::filesystem::remove(path,ec);
            if(!directory.empty())std::filesystem::remove(directory,ec);}
    } temporary;
    std::error_code ec;std::filesystem::create_directories(file_.parent_path(),ec);if(ec)return false;
    for(unsigned attempt=0;attempt<8;++attempt) {
        const auto directory=file_.parent_path()/(".health-history-"+std::to_string(std::random_device{}()));
        if(std::filesystem::create_directory(directory,ec)) {
            temporary.directory=directory;temporary.path=directory/"record.tmp";break;
        }
        if(ec&&ec!=std::errc::file_exists)return false;
    }
    if(temporary.path.empty())return false;
    std::string bytes;
    for(const auto& row:incidents_)bytes+='E'+encode(row);
    const auto boundary=bytes.size();
    for(const auto& row:recent_)bytes+='S'+encode(row);
    // Pathological maximum-width counters still cannot exceed the file cap.
    while(bytes.size()>MaxBytes) {
        const auto end=bytes.find('\n',boundary<bytes.size()?boundary:0);
        if(end==std::string::npos)return false;
        const auto begin=boundary<bytes.size()?boundary:0;
        bytes.erase(begin,end-begin+1);
    }
    std::ofstream out(temporary.path,std::ios::binary|std::ios::trunc);
    out<<bytes;
    out.close();if(!out)return false;
#ifdef _WIN32
    if(!MoveFileExW(temporary.path.c_str(),file_.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return false;
#else
    std::filesystem::rename(temporary.path,file_,ec);if(ec)return false;
#endif
    dirty_=false;return true;
}
bool History::Store(const std::filesystem::path& directory, HistorySample sample) {
    std::lock_guard lock(mutex_);
    const bool diskReady=Load(directory);
    const bool event=recent_.empty()||changed(recent_.back(),sample);
    if(event) {
        if(!recent_.empty())incidents_.push_back(recent_.back());
        incidents_.push_back(sample);
        while(incidents_.size()>64)incidents_.pop_front();
    }
    if(event||recent_.empty()||sample.at<recent_.back().at||sample.at-recent_.back().at>=5000) {
        recent_.push_back(sample);if(recent_.size()>128)recent_.pop_front();dirty_=true;
    }
    if(!diskReady)return persisted_=false;
    if(dirty_&&(event||sample.at<lastWrite_||sample.at-lastWrite_>=5000)) {
        lastWrite_=sample.at;persisted_=Flush();
    }
    return persisted_;
}
std::string History::Report(const std::filesystem::path& directory) {
    std::lock_guard lock(mutex_);
    if(!Load(directory))persisted_=false;
    std::string out=std::string("health-history-v1 persisted=")+(persisted_?"yes":"no; memory-only pending retry")+
        " recent-limit=128 incident-limit=64 max-file-bytes=131072\nincidents:\n";
    for(const auto& row:incidents_)out+=describe(row);
    out+="recent-samples:\n";
    const auto begin=recent_.size()>32?recent_.size()-32:0;
    for(auto i=begin;i<recent_.size();++i)out+=describe(recent_[i]);
    return out;
}
} // namespace clashflux::health
