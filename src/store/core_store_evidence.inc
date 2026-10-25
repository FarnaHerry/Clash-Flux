// All callers run on worker threads. Queries never change data-plane state.
    clashflux::health::Snapshot runtimeHealthSnapshot(const CoreSnapshot& core) {
        auto health=streams_.health();
#if defined(__ANDROID__)
        health.runtimeEpoch=core.runtimeEpoch;
        health.control=core.binderAvailable?clashflux::health::Control::Responsive:clashflux::health::Control::Unreachable;
        auto& status=health.traffic;
        status.generation=core.clientGeneration;status.valid=core.statusSequence;status.validAgeMs=core.statusAgeMs;
        status.connected=core.statusConnected;status.repairs=core.statusRepairs;
        const auto& state=core.statusHealth;
        status.state=state=="fresh"?clashflux::health::State::Fresh:state=="stale"?clashflux::health::State::Stale:
            state=="waiting"?clashflux::health::State::Waiting:state=="reconnecting"?clashflux::health::State::Reconnecting:clashflux::health::State::Paused;
        if(status.valid&&!core.trafficAvailable)status.state=clashflux::health::State::Unsupported;
        health.connections=status;health.connections.periodic=false;
#else
        static_cast<void>(core);
#endif
        return health;
    }
    clashflux::network_probe::Request networkRequestLocked(bool dns) {
        clashflux::network_probe::Request request;
        request.mixedPort=snapshot().mixedPort;
        const bool runningTun=snapshot().state==core::CoreState::Running&&snapshot().tunEnabled;
        const auto config=nlohmann::json::parse(lastAppliedJson_,nullptr,false);
        if(!config.is_object()||!config.contains("inbounds"))return request;
        unsigned count=0;
        for(const auto& inbound:config["inbounds"]) {
            if(inbound.value("type","")!="tun")continue;
            if(++count>1)return {}; // No guessing which of several TUNs owns the path.
            request.interfaceName=inbound.value("interface_name","");
            if(!inbound.contains("address")||!inbound["address"].is_array())continue;
            for(const auto& address:inbound["address"]) {
                if(!address.is_string())continue;
                const auto cidr=address.get<std::string>();
                if(cidr.find(':')!=std::string::npos)continue;
                const auto slash=cidr.find('/');
                request.address=cidr.substr(0,slash);
                if(inbound.value("dns_mode","hijack")=="disabled")break;
                if(inbound.contains("dns_address")) {
                    const auto& list=inbound["dns_address"];
                    if(list.is_array())for(const auto& server:list)
                        if(server.is_string()&&server.get<std::string>().find(':')==std::string::npos) {
                            request.dnsAddress=server.get<std::string>();break;
                        }
                } else if(slash!=std::string::npos) {
                    // sing-box 1.14: default DNS is the next address inside the first prefix.
                    unsigned prefix=33;const auto bits=std::string_view(cidr).substr(slash+1);
                    const auto parsed=std::from_chars(bits.data(),bits.data()+bits.size(),prefix);
                    std::array<unsigned,4> octets{};bool valid=true;std::size_t at=0;
                    for(auto& octet:octets) {
                        const auto end=request.address.find('.',at);
                        const auto part=std::string_view(request.address).substr(at,end==std::string::npos?end:end-at);
                        const auto value=std::from_chars(part.data(),part.data()+part.size(),octet);
                        valid&=value.ec==std::errc{}&&value.ptr==part.data()+part.size()&&octet<256;
                        at=end==std::string::npos?request.address.size():end+1;
                    }
                    if(valid&&parsed.ec==std::errc{}&&parsed.ptr==bits.data()+bits.size()&&prefix>0&&prefix<32) {
                        const std::uint32_t ip=(octets[0]<<24)|(octets[1]<<16)|(octets[2]<<8)|octets[3];
                        const auto mask=0xffffffffu<<(32-prefix),next=ip+1;
                        if(next>ip&&(next&mask)==(ip&mask))request.dnsAddress=std::format("{}.{}.{}.{}",
                            next>>24,(next>>16)&255,(next>>8)&255,next&255);
                    }
                }
                break;
            }
        }
        if(!count&&lastTunProbe_) {request=*lastTunProbe_;request.mixedPort=snapshot().mixedPort;}
        request.dnsRoundTrip=dns&&runningTun;
        if(count==1&&!request.address.empty())lastTunProbe_=request;
        return request;
    }

    // Persist only typed, redacted evidence. Slow OS reads are cached with an
    // epoch and age; the report cannot silently reuse another instance's checks.
    void captureHealthEvidence(bool queryOperatingSystem) noexcept {
        try {
        CoreSnapshot before;
        clashflux::network_probe::Request request;
        {
            std::unique_lock owner(lifecycleMutex_,std::try_to_lock);
            if(!owner.owns_lock())return;
            before=snapshot();
            if(queryOperatingSystem)request=networkRequestLocked(false);
        }
        std::optional<clashflux::network_probe::Result> network;
        if(queryOperatingSystem) {
            network=clashflux::network_probe::Inspect(request);
            if(request.address.empty()&&(before.state!=core::CoreState::Running||!before.tunEnabled))
                network->interface=clashflux::network_probe::Evidence::NotChecked;
        }
        std::unique_lock owner(lifecycleMutex_,std::try_to_lock);
        if(!owner.owns_lock())return;
        const auto after=snapshot();
        if(before.runtimeEpoch!=after.runtimeEpoch||before.state!=after.state||before.planRevision!=after.planRevision)return;
        if(network) {observedNetwork_=*network;observedNetworkEpoch_=after.runtimeEpoch;
            observedNetworkRunning_=after.state==core::CoreState::Running;observedNetworkTun_=after.tunEnabled;
            observedNetworkAt_=std::chrono::steady_clock::now();}
        clashflux::health::HistorySample sample;
        sample.at=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        sample.session=healthSession_;sample.epoch=after.runtimeEpoch;sample.instance=after.nativeStartedAt;sample.revision=after.planRevision;
        sample.lifecycle=after.state==core::CoreState::Running?2:after.state==core::CoreState::Starting?1:after.state==core::CoreState::Failed?3:0;
        sample.tunRunning=after.state==core::CoreState::Running&&after.tunEnabled;
        sample.tunIntent=tunEnabled();sample.proxyIntent=systemProxyEnabled();sample.streams=runtimeHealthSnapshot(after);
        if(observedNetworkAt_!=std::chrono::steady_clock::time_point{}&&observedNetworkEpoch_==after.runtimeEpoch&&
            observedNetworkRunning_==(after.state==core::CoreState::Running)&&observedNetworkTun_==after.tunEnabled) {
            sample.network=observedNetwork_;
            sample.networkAgeMs=std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now()-observedNetworkAt_).count();
        }
        // The native session monitor owns RAS/IPC I/O. This cached routing set
        // states what the accepted plan currently includes, not a new RAS poll.
        {
            std::lock_guard lock(mutex_);
            for(const auto& session:nativeSessions_)if(session.connected&&session.kind==vpn::ConnectionKind::Pptp) {
                const auto found=pptpEvidence_.find(session.id);
                if(found==pptpEvidence_.end()||!found->second)++sample.pptpUnknown;
                else if(*found->second)++sample.pptpConnected;
            }
        }
        healthCaptureFailed_.store(!healthHistory_.Store(cfg::coreWorkDir(),sample));
        } catch(...) {healthCaptureFailed_.store(true);}
    }
    void recordPptpEvidence(const std::string& id,std::optional<bool> evidence) {
        std::lock_guard lock(mutex_);pptpEvidence_[id]=evidence;
    }
