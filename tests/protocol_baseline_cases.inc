// Frozen before the first protocol migration. All credentials are synthetic.
json protocolObservation(const singbox::CompileResult& result) {
    auto notes = json::array();
    for (const auto& note : result.fidelity)
        notes.push_back({{"scope", int(note.scope)}, {"level", int(note.level)},
            {"subject", note.subject}, {"detail", note.detail},
            {"action", note.action}, {"sourceId", note.sourceId}});
    return {{"outbounds", result.json.empty() ? json{} : json::parse(result.json).at("outbounds")},
        {"error", result.error}, {"fidelity", std::move(notes)}, {"warnings", result.warnings}};
}

json protocolNodeNotes(const json& notes) {
    auto result = json::array();
    for (const auto& note : notes)
        if (note["scope"] == int(singbox::FidelityScope::Node)) result.push_back(note);
    return result;
}

void testProtocolClashBaseline(std::string_view filename, const std::filesystem::path& fixtures) {
    std::ifstream input(std::filesystem::path(__FILE__).parent_path() / "fixtures" / filename);
    check(bool(input), "protocol baseline fixture is available");
    if (!input) return;
    const auto baseline = json::parse(input);
    unsigned validProtocols = 0;
    for (const auto& fixture : baseline.at("cases")) {
        const auto name = fixture.at("name").get<std::string>();
        singbox::CompileOptions options;
        options.mainConnectionId = "protocol-baseline";
        options.profileYaml = fixture.at("yaml").get<std::string>();
        const auto result = singbox::compileConfig(options);
        const auto actual = protocolObservation(result);
        check(actual.dump() == fixture.at("expected").dump(), "protocol frozen baseline: " + name);
        if (name.starts_with("valid-")) {
            ++validProtocols;
            check(result.error.empty() && actual["outbounds"].size() == 2,
                "protocol baseline retains the declared node: " + name);
            const auto& node = actual["outbounds"][1];
            check(node["server"] == "example.test", "protocol baseline preserves server: " + name);
            if (name == "valid-hysteria")
                check(node["down_mbps"] == 24 && node["connection_receive_window"] == 1048576 &&
                    node["stream_receive_window"] == 2097152, "HY1 units and window directions have independent expectations");
            if (name == "valid-http")
                check(node["username"] == " fixture user " && node["password"] == " fixture pass " &&
                    node["tls"]["utls"]["fingerprint"] == "chrome", "HTTP credentials and TLS defaults are preserved");
            if (name == "valid-socks5")
                check(node["type"] == "socks" && node["username"] == "" && node["network"] == "tcp" &&
                    !node.contains("tls"), "SOCKS empty credentials and explicit UDP restriction are preserved");
            if (!fixtures.empty()) {
                std::filesystem::create_directories(fixtures);
                std::ofstream output(fixtures / (name + ".json"));
                output << result.json;
                output.close();
                check(bool(output), "protocol native check fixture written: " + name);
            }
        }

        if (!fixtures.empty() && filename == "protocol-p2-baseline.json" && !result.json.empty()) {
            static const std::set<std::string> suffixes{"plain", "udp-false", "packet-empty", "packet-xudp",
                "packet-addr", "legacy-udp", "tls-on", "ws", "ws-old", "ws-override", "grpc",
                "grpc-empty", "http", "h2-empty", "httpupgrade", "global-fingerprint", "padding-false",
                "empty-encryption"};
            const auto separator = name.find('-');
            if (suffixes.contains(name.substr(separator + 1))) {
                std::filesystem::create_directories(fixtures);
                std::ofstream output(fixtures / (name + ".json"));
                output << result.json;
                output.close();
                check(bool(output), "P2 native check fixture written: " + name);
            }
        }
        if (!fixtures.empty() && (filename == "protocol-p3-baseline.json" || filename == "protocol-p4-baseline.json") &&
            fixture.value("native_check", false)) {
            check(result.error.empty() && !result.json.empty(), "protocol native fixture compiles: " + name);
            std::filesystem::create_directories(fixtures);
            std::ofstream output(fixtures / (name + ".json"));
            output << result.json;
            output.close();
            check(bool(output), "protocol native check fixture written: " + name);
        }

        // A collection has a different failure boundary from a main profile,
        // but each participating node must retain the same mapping and notes.
        const auto start = options.profileYaml.find("proxies:\n");
        const auto end = options.profileYaml.find("rules:\n", start);
        std::string collection = options.profileYaml.substr(0, start) +
            "proxy-providers:\n  fixture:\n    type: inline\n    payload:\n";
        std::istringstream lines(options.profileYaml.substr(start + 9, end - start - 9));
        for (std::string line; std::getline(lines, line);) collection += "    " + line + "\n";
        options.profileYaml = collection + "rules:\n  - MATCH,DIRECT\n";
        const auto provider = singbox::compileConfig(options);
        const auto observed = protocolObservation(provider);
        check(protocolNodeNotes(actual["fidelity"]) == protocolNodeNotes(observed["fidelity"]),
            "provider and main profile keep identical node diagnostics: " + name);
        if (actual["outbounds"].is_array() && actual["outbounds"].size() == 2)
            check(provider.error.empty() && observed["outbounds"].dump() == actual["outbounds"].dump(),
                "provider and main profile keep identical protocol output: " + name);
        else
            check(!provider.error.empty() && provider.json.empty(),
                "provider refuses a partial collection: " + name);
    }
    if (filename == "protocol-baseline.json")
        check(validProtocols == 12, "protocol baseline covers twelve supported protocol families");
}

void testProtocolUriBaseline(const std::filesystem::path& fixtures) {
    std::ifstream input(std::filesystem::path(__FILE__).parent_path() / "fixtures/protocol-p2-uri-baseline.json");
    check(bool(input), "P2 URI frozen baseline fixture is available");
    if (!input) return;
    const auto baseline = json::parse(input);
    for (const auto& fixture : baseline.at("cases")) {
        const auto name = fixture.at("name").get<std::string>();
        singbox::CompileOptions options;
        options.mainConnectionId = "protocol-baseline";
        options.profileYaml = fixture.at("yaml").get<std::string>();
        const auto result = singbox::compileConfig(options);
        check(protocolObservation(result).dump() == fixture.at("expected").dump(),
            "VMess URI frozen baseline: " + name);
        if (!result.json.empty()) {
            std::string error;
            check(singbox::ValidateProfileDocument(options.profileYaml, error),
                "typed URI still supports read-only syntax validation: " + name);
            const auto rules = singbox::InspectProfileRules(options.profileYaml, {});
            check(rules.error.empty() && rules.original.size() == 1 &&
                rules.original.front().raw.starts_with("MATCH,"), "typed URI exposes its generated default policy: " + name);
            const auto inventory = singbox::inspectProfileSourceObjects(options);
            check(inventory.error.empty() && inventory.sourceObjects.size() == 3,
                "typed URI exposes its node, selector and DIRECT in the source inventory: " + name);
            if (!fixtures.empty() && !name.starts_with("uri-sni-")) {
                std::filesystem::create_directories(fixtures);
                std::ofstream output(fixtures / (name + ".json"));
                output << result.json;
                output.close();
                check(bool(output), "P2 URI native check fixture written: " + name);
            }
        }
    }
}

void testProtocolBaseline(const std::filesystem::path& fixtures = {}) {
    testProtocolClashBaseline("protocol-baseline.json", fixtures);
    testProtocolClashBaseline("protocol-p2-baseline.json", fixtures);
    testProtocolClashBaseline("protocol-p3-baseline.json", fixtures);
    testProtocolClashBaseline("protocol-p4-baseline.json", fixtures);
    testProtocolUriBaseline(fixtures);
}
