// On-demand diagnostics only. Never writes settings, restarts data plane or
// exports endpoints, credentials, profile names/configuration, or destinations.
    std::string runtimeDiagnostics(const std::string& target, std::stop_token cancel) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        const auto remaining = [&] {
            return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count());
        };
        CoreSnapshot before;
        std::vector<std::string> pptpIds;
        clashflux::network_probe::Request networkRequest;
        std::vector<std::string> transportAddresses;
        {
            std::unique_lock owner(lifecycleMutex_, std::try_to_lock);
            if (!owner.owns_lock()) return "diagnostic=maintenance-in-progress; retry after the current operation\n";
            before = snapshot();
            networkRequest=networkRequestLocked(true);
            for (const auto& session : nativeSessions_)
                if (session.connected && session.kind == vpn::ConnectionKind::Pptp) {
                    pptpIds.push_back(session.id);
                    if(transportAddresses.size()<16&&!session.transportAddress.empty())transportAddresses.push_back(session.transportAddress);
                }
        }
        const auto evidence = streams_.health();
        std::string report = "Clash-Flux runtime diagnostics v3\n";
        report += "lifecycle=" + std::string(core::stateName(before.state)) + "\n";
        report += "runtime-epoch=" + std::to_string(before.runtimeEpoch) + "\n";
#if defined(__ANDROID__)
        report += "control-binder=" + std::string(before.binderAvailable ? "responsive" : "unconfirmed") +
            " age-ms=" + std::to_string(before.binderAgeMs) + "\n";
        report += "status=" + before.statusHealth + " sequence=" + std::to_string(before.statusSequence) +
            " age-ms=" + std::to_string(before.statusAgeMs) + " generation=" + std::to_string(before.clientGeneration) +
            " repairs=" + std::to_string(before.statusRepairs) + "\n";
        report += "traffic-available=" + std::string(before.trafficAvailable ? "yes" : "no-or-unconfirmed") + "\n";
#else
        if (before.state == core::CoreState::Running && !cancel.stop_requested()) {
            const auto result = api().probeVersion();
            report += "control=" + std::string(clashflux::health::Name(clashflux::health::ProbeControl(
                result.ok, result.status, static_cast<bool>(clashflux::wire::DecodeVersion(result.body))))) + "\n";
#if !defined(CLASHFLUX_IOS)
            api::NativeApi native(cfg::nativeControllerBaseUrl(), secret());
            const auto state = native.runtime(cancel);
            report += "native-control=" + std::string(clashflux::health::Name(
                clashflux::health::NativeControl(state.ok, state.failurePhase))) + "\n";
            if (state.ok) report += "native-api-version=" + std::to_string(state.apiVersion) +
                " instance=" + std::string(!before.nativeStartedAt ? "startup-unconfirmed" :
                    state.startedAt == before.nativeStartedAt ? "matches-startup" : "changed-since-startup") + "\n";
#else
            report += "native-control=unsupported-on-this-platform\n";
#endif
        } else report += "control=not-checked; native-control=not-checked\n";
        unsigned connected = 0, disconnected = 0, unconfirmed = 0;
        for (const auto& id : pptpIds) {
            if (cancel.stop_requested() || remaining() < 2500) { ++unconfirmed; continue; }
            const auto alive = service::pptpSessionEvidence(id);
            if (!alive) ++unconfirmed;
            else if (*alive) ++connected;
            else ++disconnected;
        }
        report += "pptp-session-evidence connected=" + std::to_string(connected) +
            " disconnected=" + std::to_string(disconnected) + " unconfirmed=" + std::to_string(unconfirmed) + "\n";
        const auto channel = [&](std::string_view name, const clashflux::health::ChannelSnapshot& value) {
            report += std::string(name) + "=" + std::string(clashflux::health::Name(value.state)) +
                " age-ms=" + std::to_string(value.validAgeMs) + " received=" + std::to_string(value.received) +
                " valid=" + std::to_string(value.valid) + " consumed=" + std::to_string(value.consumed) +
                " invalid=" + std::to_string(value.invalid) + " generation=" + std::to_string(value.generation) +
                " repairs=" + std::to_string(value.repairs) + "\n";
        };
        channel("traffic", evidence.traffic);
        channel("connections", evidence.connections);
        for (const auto& event : streams_.recentHealthEvents()) report += event + "\n";
#endif
        report += "tun-runtime-reported=" + std::string(before.tunEnabled ? "enabled" : "disabled") +
            " tun-intent=" + std::string(tunEnabled() ? "on" : "off") + "\n";
        auto network=clashflux::network_probe::Inspect(networkRequest,cancel,
            std::chrono::milliseconds(std::max(0L,std::min(1200L,remaining()))));
        if(networkRequest.address.empty()&&(before.state!=core::CoreState::Running||!before.tunEnabled))
            network.interface=clashflux::network_probe::Evidence::NotChecked;
        report += "os-tun-interface="+std::string(clashflux::network_probe::Name(network.interface))+" scope=first-IPv4-address\n";
        const bool expectedTun=before.state==core::CoreState::Running&&before.tunEnabled;
        const bool knownInterface=network.interface==clashflux::network_probe::Evidence::Present||
            network.interface==clashflux::network_probe::Evidence::Absent||network.interface==clashflux::network_probe::Evidence::Down||
            network.interface==clashflux::network_probe::Evidence::AddressMismatch;
        report += "tun-plan-os-consistency="+std::string(!knownInterface?"unknown":
            expectedTun==(network.interface==clashflux::network_probe::Evidence::Present)?"matches":"mismatch")+"\n";
        report += "os-route-sample="+std::string(clashflux::network_probe::Name(network.route))+" scope=documentation-address; no packet sent\n";
        report += "system-proxy-manual="+std::string(clashflux::network_probe::Name(network.proxy))+" scope=manual-proxy-only; PAC/WPAD not checked\n";
        report += "tun-dns-route="+std::string(clashflux::network_probe::Name(network.dnsRoute))+
            " tun-dns-roundtrip="+std::string(clashflux::network_probe::Name(network.dns))+" scope=reserved-invalid-name; not Internet availability\n";
        unsigned transportBypass=0,transportThroughTun=0,transportUnknown=0;
        for(const auto& address:transportAddresses) {
            if(cancel.stop_requested()||remaining()<250){++transportUnknown;continue;}
            auto request=networkRequest;request.dnsRoundTrip=false;request.routeDestination=address;
            const auto result=clashflux::network_probe::Inspect(request,cancel,std::chrono::milliseconds(100));
            if(result.route==clashflux::network_probe::Evidence::OtherInterface)++transportBypass;
            else if(result.route==clashflux::network_probe::Evidence::ViaTun)++transportThroughTun;
            else ++transportUnknown;
        }
        report += "pptp-transport-os-route outside-tun="+std::to_string(transportBypass)+
            " through-tun="+std::to_string(transportThroughTun)+" unknown="+std::to_string(transportUnknown)+"\n";
        if (target.empty()) report += "target-path=not-checked (explicit HTTP(S) target required)\n";
        else if (before.state != core::CoreState::Running) report += "target-path=not-checked (runtime not ready)\n";
        else if (cancel.stop_requested()) report += "target-path=cancelled\n";
        else {
            const auto result = api::probeThroughProxy(target, before.mixedPort, cancel, remaining());
            report += "target-path=" + std::string(result.ok ? "http-response-received" : "failed-or-inconclusive") +
                " phase=" + result.failurePhase + " http=" + std::to_string(result.status) + "\n";
            report += "path=running mixed inbound; follows current rules; target TUN path and target DNS resolution are not verified by this HTTP request\n";
        }
        std::unique_lock owner(lifecycleMutex_, std::try_to_lock);
        if (!owner.owns_lock()) return "diagnostic=cancelled-or-superseded; runtime operation in progress\n";
        const auto after = snapshot();
        if (cancel.stop_requested() || before.runtimeEpoch != after.runtimeEpoch || before.state != after.state ||
            before.planRevision != after.planRevision || before.mode != after.mode || before.tunEnabled != after.tunEnabled ||
            evidence.runtimeEpoch != streams_.health().runtimeEpoch) {
            return "diagnostic=cancelled-or-superseded; runtime changed, results discarded\n";
        }
        report += "Scope: sampled evidence and this target only; no general Internet health guarantee.\n";
        observedNetwork_=network;observedNetworkEpoch_=before.runtimeEpoch;
        observedNetworkAt_=std::chrono::steady_clock::now();
        observedNetworkRunning_=before.state==core::CoreState::Running;observedNetworkTun_=before.tunEnabled;
        captureHealthEvidence(false);
        report += std::string("health-history-capture=")+(healthCaptureFailed_.load()?"failed":"ok")+"\n";
        // The typed history is safe to include in both clipboard and file exports.
        report += healthHistory_.Report(cfg::coreWorkDir());
        return report;
    }
