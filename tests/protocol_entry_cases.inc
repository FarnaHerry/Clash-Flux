// Public-entry contract checks: source identities, collection failure boundaries,
// read-only projections and native DOM preservation, independent of private DTOs.
namespace protocol_entry_tests {

json LoadCases(const char* filename) {
    std::ifstream input(std::filesystem::path(__FILE__).parent_path() / "fixtures" / filename);
    check(bool(input), "P5 frozen entry fixtures are available");
    return input ? json::parse(input).at("cases") : json::array();
}

json Node(const singbox::CompileResult& result, std::string_view source, std::string_view name) {
    if (result.json.empty()) return {};
    const auto config = json::parse(result.json);
    for (const auto& object : result.sourceObjects) {
        if (object.sourceId != source || object.objectId != name || object.kind != vpn::TargetKind::Node) continue;
        for (const auto& out : config.at("outbounds"))
            if (out.value("tag", "") == object.tag) return out;
    }
    return {};
}

json SemanticNode(json value) { value.erase("tag"); return value; }

json NodeNotes(const singbox::CompileResult& result, std::string_view source) {
    auto notes = json::array();
    for (const auto& note : protocolNodeNotes(protocolObservation(result).at("fidelity")))
        if (note.at("sourceId") == source) notes.push_back(note);
    return notes;
}

void CheckNotes(const singbox::CompileResult& result, const json& fixture, const std::string& source) {
    auto expected = protocolNodeNotes(fixture.at("expected").at("fidelity"));
    for (auto& note : expected) note["sourceId"] = source;
    check(NodeNotes(result, source) == expected, "P5 entry retains complete ordered node diagnostics: " + fixture.at("name").get<std::string>());
}

void CheckCompiled(const singbox::CompileResult& result, const json& fixture,
        const std::string& source, const std::string& name) {
    const auto node = Node(result, source, name);
    check(result.error.empty() && !node.empty(), "P5 public entry compiles the declared node: " + fixture.at("name").get<std::string>());
    if (!node.empty()) check(SemanticNode(node) == SemanticNode(fixture.at("expected").at("outbounds")[1]),
        "P5 entry agrees with independently frozen protocol fields: " + fixture.at("name").get<std::string>());
    CheckNotes(result, fixture, source);
}

void CheckCatalog(const singbox::CompileOptions& options, const singbox::CompileResult& compiled,
        const json& fixture, const std::string& name) {
    const auto inventory = singbox::inspectProfileSourceObjects(options);
    const auto node = Node(compiled, options.mainConnectionId, name);
    check(inventory.error.empty() && inventory.json.empty() && inventory.ruleSetResources.empty() &&
        inventory.httpRuleProviders.empty(), "P5 object inspection publishes no runnable plan or rule download request");
    check(!node.empty() && std::ranges::any_of(inventory.sourceObjects, [&](const auto& object) {
        return object.sourceId == options.mainConnectionId && object.objectId == name &&
            object.kind == vpn::TargetKind::Node && object.tag == node.at("tag").get<std::string>();
    }), "P5 inactive inventory retains the compiled node identity");
    CheckNotes(inventory, fixture, options.mainConnectionId);
}

std::string ProviderDocument(const std::string& body, std::string_view kind, const std::string& filter = {}) {
    std::string document = "proxy-providers:\n  fixture:\n    type: " + std::string(kind) + "\n";
    if (!filter.empty()) document += "    filter: " + json(filter).dump() + "\n";
    if (kind == "inline") {
        const auto start = body.find("proxies:\n"), end = body.find("\nrules:", start);
        check(start != std::string::npos && end != std::string::npos, "P5 inline fixture contains Clash nodes");
        if (start == std::string::npos || end == std::string::npos) return {};
        document += "    payload:\n";
        std::istringstream lines(body.substr(start + 9, end - start - 9));
        for (std::string line; std::getline(lines, line);) document += "    " + line + "\n";
    } else document += kind == "http" ? "    url: https://fixtures.invalid/nodes\n" : "    path: nodes.yaml\n";
    return document + "proxy-groups:\n  - {name: entry-group, type: select, use: [fixture]}\nrules: ['MATCH,entry-group']\n";
}

std::pair<singbox::CompileOptions, singbox::CompileResult> CompileProvider(const std::string& body,
        std::string_view kind, const std::filesystem::path& directory, bool valid = true,
        const std::string& filter = {}, const std::string& prefix = {}) {
    singbox::CompileOptions options;
    options.mainConnectionId = "p5-main"; options.mainSourceName = "P5 provider";
    options.ruleProviderDir = directory.string();
    options.profileYaml = prefix + ProviderDocument(body, kind, filter);
    if (kind == "file") std::ofstream(directory / "nodes.yaml", std::ios::binary | std::ios::trunc) << body;
    auto result = singbox::compileConfig(options);
    if (kind == "http") {
        check(result.httpProxyProviders.size() == 1, "P5 HTTP collection is discovered without downloading");
        if (result.httpProxyProviders.size() == 1) {
            const auto resource = result.httpProxyProviders.front();
            std::string error;
            check(singbox::ValidateHttpProxyProvider(resource, body, error) == valid,
                "P5 collection validation preserves its complete-body boundary");
            options.proxyProviderContents[resource.cacheKey] = body;
            result = singbox::compileConfig(options);
        }
    }
    if (kind == "file") {
        std::string error;
        check(singbox::ReadRuleProviderText(directory / "nodes.yaml", 8 * 1024 * 1024, error) == body,
            "P5 validation and compilation leave the provider source bytes unchanged");
    }
    return {std::move(options), std::move(result)};
}

void RunSource(const json& fixture, bool uri, const std::filesystem::path& directory) {
    const auto body = fixture.at("yaml").get<std::string>();
    const std::string name = uri ? "URI fixture" : "values";
    singbox::CompileOptions options;
    options.mainConnectionId = "p5-main"; options.mainSourceName = "P5 main"; options.profileYaml = body;
    std::string error;
    check(singbox::DetectProfileFormat(body) == (uri ? singbox::ProfileFormat::NodeSubscription : singbox::ProfileFormat::ClashYaml) &&
        singbox::ValidateProfileDocument(body, error), "P5 format/document validation accepts the existing format");
    const auto projection = singbox::InspectProfileRules(body, {});
    const auto effective = singbox::InspectProfileRules(body, {}, false);
    check(projection.error.empty() && projection.original.size() == 1 && effective.error.empty() &&
        effective.original.empty() && effective.rules.size() == 1, "P5 rule inspection retains the existing terminal policy");
    const auto compiled = singbox::compileConfig(options);
    CheckCompiled(compiled, fixture, options.mainConnectionId, name);
    CheckCatalog(options, compiled, fixture, name);
    for (const auto* kind : {"inline", "file", "http"}) {
        if (uri && std::string_view(kind) == "inline") continue;
        const auto [provider, result] = CompileProvider(body, kind, directory);
        CheckCompiled(result, fixture, provider.mainConnectionId, name);
        CheckCatalog(provider, result, fixture, name);
    }
#if !defined(__ANDROID__) && !defined(CLASHFLUX_IOS)
    options.auxiliarySources = {{"p5-aux", "P5 secondary", body, true}};
    options.globalRules = {{.match = vpn::MatchKind::DomainSuffix, .pattern = "entry.invalid",
        .connectionId = "p5-aux", .id = "p5-entry", .tier = vpn::RuleTier::SourcePolicy,
        .targetKind = vpn::TargetKind::Node, .targetObject = name}};
    const auto composed = singbox::compileConfig(options);
    CheckCompiled(composed, fixture, "p5-main", name);
    CheckCompiled(composed, fixture, "p5-aux", name);
    const auto main = Node(composed, "p5-main", name), auxiliary = Node(composed, "p5-aux", name);
    check(!main.empty() && !auxiliary.empty() && main.at("tag") != auxiliary.at("tag"),
        "P5 same-named nodes retain separate source identities and dependency closures");
#endif
    std::stop_source cancelled; cancelled.request_stop();
    const auto stopped = singbox::compileConfig(options, cancelled.get_token());
    check(stopped.json.empty() && !stopped.error.empty() && stopped.sourceObjects.empty(),
        "P5 cancelled preparation publishes no configuration or object directory");
    check(options.profileYaml == body, "P5 public entries retain the original profile content");
}

void RunBoundaries(const std::filesystem::path& directory) {
    const std::string body = "proxies:\n  - {name: values, type: socks5, server: example.test, port: 443}\n"
        "  - {name: bad, type: unknown, server: example.test, port: 443}\nrules: ['MATCH,DIRECT']\n";
    singbox::CompileOptions main; main.mainConnectionId = "p5-main"; main.profileYaml = body;
    std::string error;
    check(singbox::ValidateProfileDocument(body, error) && singbox::InspectProfileRules(body, {}).error.empty(),
        "P5 document/rule inspection is distinct from approving node capabilities");
    const auto partial = singbox::compileConfig(main);
    check(partial.error.empty() && !Node(partial, "p5-main", "values").empty() &&
        std::ranges::any_of(partial.fidelity, [](const auto& note) {
            return note.subject == "bad" && note.level == singbox::Fidelity::Unsupported;
        }), "P5 ordinary Clash retains the valid node and explicitly accounts for the rejected one");
    for (const auto* kind : {"inline", "file", "http"}) {
        const auto [options, result] = CompileProvider(body, kind, directory, false);
        check(!result.error.empty() && result.json.empty(), "P5 a participating bad provider node rejects the complete collection");
        const auto inventory = singbox::inspectProfileSourceObjects(options);
        check(!inventory.error.empty() && inventory.sourceObjects.empty(), "P5 a failed collection never publishes a partial inventory");
        const auto [filteredOptions, filtered] = CompileProvider(body, kind, directory, true, "^values$");
        check(filtered.error.empty() && !Node(filtered, "p5-main", "values").empty() &&
            NodeNotes(filtered, "p5-main").empty(), "P5 YAML collection filtering precedes conversion of excluded nodes");
    }
    const json uriNode{{"ps", "values"}, {"add", "example.test"}, {"port", "443"},
        {"id", "11111111-2222-3333-4444-555555555555"}, {"net", "tls"}, {"tls", "tls"}};
    const auto uri = "vmess://" + encodeNodeFixture(uriNode.dump());
    auto ordinary = uriNode; ordinary["ps"] = "keep"; ordinary["net"] = "tcp";
    const auto two = uri + "\nvmess://" + encodeNodeFixture(ordinary.dump());
    for (const auto* kind : {"file", "http"}) {
        const auto [selectedOptions, selected] = CompileProvider(two, kind, directory, true, "^values$");
        const auto notes = NodeNotes(selected, "p5-main");
        check(selected.error.empty() && !Node(selected, "p5-main", "values").empty() && notes.size() == 1 &&
            notes[0].at("detail").get<std::string>().find("net=tls") != std::string::npos,
            "P5 URI provider records the participating TLS transport alias exactly once");
        const auto [excludedOptions, excluded] = CompileProvider(two, kind, directory, true, "^keep$");
        check(excluded.error.empty() && !Node(excluded, "p5-main", "keep").empty() && NodeNotes(excluded, "p5-main").empty(),
            "P5 filtered-out URI aliases do not enter the node ledger");
        const auto [badOptions, bad] = CompileProvider(uri + "\nss://unsupported", kind, directory, false, "^values$");
        check(!bad.error.empty() && bad.json.empty(), "P5 URI syntax validation still rejects a bad excluded line before filtering");
        const auto [scopedOptions, scoped] = CompileProvider(uri, kind, directory, true, {}, "global-client-fingerprint: random\n");
        const auto node = Node(scoped, "p5-main", "values");
        const auto scopedNotes = NodeNotes(scoped, "p5-main");
        check(!node.empty() && node["tls"]["utls"]["fingerprint"] == "random" && scopedNotes.size() == 2 &&
            scopedNotes[0].at("detail").get<std::string>().find("net=tls") != std::string::npos &&
            scopedNotes[1].at("detail").get<std::string>().find("random") != std::string::npos,
            "P5 URI provider inherits its owning source fingerprint with ordered, distinct approximations");
#if !defined(__ANDROID__) && !defined(CLASHFLUX_IOS)
        auto composedOptions = scopedOptions;
        composedOptions.auxiliarySources = {{"p5-aux", "URI secondary", uri, true}};
        composedOptions.globalRules = {{.match = vpn::MatchKind::DomainSuffix, .pattern = "entry.invalid",
            .connectionId = "p5-aux", .id = "p5-scope", .tier = vpn::RuleTier::SourcePolicy,
            .targetKind = vpn::TargetKind::Node, .targetObject = "values"}};
        const auto composed = singbox::compileConfig(composedOptions);
        const auto auxiliary = Node(composed, "p5-aux", "values");
        check(!auxiliary.empty() && auxiliary["tls"]["utls"]["fingerprint"] == "chrome" &&
            NodeNotes(composed, "p5-aux").size() == 1, "P5 URI secondary source does not inherit the main source fingerprint");
#endif
    }
    main.profileYaml = R"json({"outbounds":[{"type":"future-native","tag":"native","future":{"enabled":false,"count":18446744073709551615,"signed":-9223372036854775808,"ratio":1.25,"bytes":" exact +%40 ","items":[null,{},[]]}}],"route":{"final":"native"},"future_root":{"keep":true}})json";
    const auto native = json::parse(main.profileYaml);
    const auto compiled = singbox::compileConfig(main);
    check(singbox::ValidateProfileDocument(main.profileYaml, error) && compiled.error.empty() && !compiled.json.empty(),
        "P5 native JSON remains a DOM path without typed-protocol capability approval");
    if (!compiled.json.empty()) {
        const auto output = json::parse(compiled.json);
        check(output.at("outbounds")[0].dump() == native.at("outbounds")[0].dump() &&
            output.at("outbounds")[0]["future"]["count"].is_number_unsigned() &&
            output.at("future_root") == native.at("future_root"),
            "P5 native unknown fields, uint64, signed bounds, floats, null and containers survive managed merging");
    }
    check(!singbox::inspectProfileSourceObjects(main).error.empty(), "P5 native JSON does not accidentally enable a Clash object inventory");
}

void Run() {
    const RuleSetTestDirectory directory;
    unsigned families = 0;
    for (const auto& fixture : LoadCases("protocol-baseline.json")) {
        if (!fixture.at("name").get<std::string>().starts_with("valid-")) continue;
        RunSource(fixture, false, directory.path); ++families;
    }
    check(families == 12, "P5 public entry matrix covers all twelve existing protocol families");
    for (const auto& fixture : LoadCases("protocol-p4-baseline.json")) {
        const auto name = fixture.at("name").get<std::string>();
        if (name == "p4-hysteria-plain" || name == "p4-anytls-udp-false" || name == "p4-ssh-key-dual")
            RunSource(fixture, false, directory.path);
    }
    for (const auto& fixture : LoadCases("protocol-p2-uri-baseline.json")) {
        const auto name = fixture.at("name").get<std::string>();
        if (name == "uri-tls" || name == "uri-ws" || name == "uri-fp-6") RunSource(fixture, true, directory.path);
    }
    RunBoundaries(directory.path);
}

} // namespace protocol_entry_tests
