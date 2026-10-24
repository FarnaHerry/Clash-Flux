// On-demand diagnostics only. Never writes settings, restarts data plane or
// exports endpoints, credentials, profile names/configuration, or destinations.
    std::string runtimeDiagnostics(const std::string& target, std::stop_token cancel) {
        const auto before = snapshot();
        const auto evidence = streams_.health();
        std::string report = "Clash-Flux runtime diagnostics v1\n";
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
            report += "control=" + std::string(result.status == 401 || result.status == 403 ? "authentication-error" :
                !result.ok ? "unreachable" : clashflux::wire::DecodeVersion(result.body) ? "responsive" : "invalid-response") + "\n";
        } else report += "control=not-checked\n";
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
        report += "system-proxy=not-checked; TUN interface, OS routing and DNS interception=not-checked\n";
        if (target.empty()) report += "target-path=not-checked (explicit HTTP(S) target required)\n";
        else if (before.state != core::CoreState::Running) report += "target-path=not-checked (runtime not ready)\n";
        else if (cancel.stop_requested()) report += "target-path=cancelled\n";
        else {
            const auto result = api::probeThroughProxy(target, before.mixedPort, cancel);
            report += "target-path=" + std::string(result.ok ? "http-response-received" : "failed-or-inconclusive") +
                " phase=" + result.failurePhase + " http=" + std::to_string(result.status) + " " + result.error + "\n";
            report += "path=running mixed inbound; follows current rules; system proxy/TUN path and kernel DNS are not independently verified\n";
        }
        const auto after = snapshot();
        if (cancel.stop_requested() || before.runtimeEpoch != after.runtimeEpoch || before.state != after.state ||
            before.planRevision != after.planRevision || before.mode != after.mode || before.tunEnabled != after.tunEnabled ||
            evidence.runtimeEpoch != streams_.health().runtimeEpoch) {
            return "diagnostic=cancelled-or-superseded; runtime changed, results discarded\n";
        }
        report += "Scope: sampled evidence and this target only; no general Internet health guarantee.\n";
        return report;
    }
