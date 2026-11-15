// Public API contracts and independently authored format/target expectations.
namespace node_input_tests {
unsigned legacyAccepted = 0, legacyRejected = 0;
std::size_t LegacyEvents(const singbox::CompileResult& result) {
    return std::ranges::count(result.warnings, "已读取 SS 历史整串 Base64 格式；密码按原文字节保留，不进行百分号解码");
}
std::string Legacy(std::string_view password = " exact +%40: key ", std::string_view host = "example.test:443",
        std::string_view method = "aes-256-gcm", bool padding = true, std::string_view name = "legacy fixture") {
    auto body = encodeNodeFixture(std::string(method) + ":" + std::string(password) + "@" + std::string(host));
    if (!padding) while (body.ends_with('=')) body.pop_back();
    return "ss://" + body + "#" + node_uri_ss_tests::Percent(name);
}
void LegacyAccept(const std::string& body, const json& expected, const std::filesystem::path& fixtures = {},
        const std::string& name = "legacy fixture") {
    ++legacyAccepted;
    const auto result = node_uri_ss_tests::Compile(body);
    const auto node = protocol_entry_tests::Node(result, "p6-main", name);
    check(result.error.empty() && !node.empty(), "legacy SS public input compiles: " + std::to_string(legacyAccepted));
    if (!node.empty()) check(protocol_entry_tests::SemanticNode(node) == expected, "legacy SS retains raw credential and endpoint bytes");
    check(LegacyEvents(result) == 1 && protocol_entry_tests::NodeNotes(result, "p6-main").empty(),
        "lossless legacy decoding has a format diagnostic without false node approximation");
    std::string error;
    check(singbox::ValidateProfileDocument(body, error) && singbox::InspectProfileRules(body, {}).original.size() == 1,
        "legacy SS shares ordinary document validation and rule projection");
    if (!fixtures.empty() && !result.json.empty()) {
        std::filesystem::create_directories(fixtures);
        std::ofstream(fixtures / ("legacy-ss-" + std::to_string(legacyAccepted) + ".json")) << result.json;
    }
}
void LegacyReject(const std::string& body) {
    ++legacyRejected;
    const auto result = node_uri_ss_tests::Compile(body); std::string error;
    check(!result.error.empty() && result.json.empty() && !singbox::ValidateProfileDocument(body, error),
        "invalid legacy SS rejects the complete candidate: " + std::to_string(legacyRejected));
    const auto safe = [](const auto& text) { return text.find("PRIVATE") == std::string::npos && text.find("://") == std::string::npos; };
    check(safe(result.error) && safe(error) && std::ranges::all_of(result.fidelity, [&](const auto& note) {
        return safe(note.detail) && safe(note.action);
    }), "legacy errors contain reasons without raw passwords or URI payloads");
}

void LegacyEntries() {
    const RuleSetTestDirectory directory;
    const auto legacy = Legacy(" PRIVATE @/#?+%00%2F: key ");
    const auto modern = node_uri_v2ray_tests::Uri(false, {}, {}, "modern fixture");
    const auto mixed = legacy + "\n" + modern;
    for (const auto* kind : {"file", "http"}) {
        const auto [options, result] = protocol_entry_tests::CompileProvider(encodeNodeFixture(mixed), kind, directory.path);
        check(result.error.empty() && LegacyEvents(result) == 1 &&
            protocol_entry_tests::Node(result, "p5-main", "legacy fixture")["password"] == " PRIVATE @/#?+%00%2F: key ",
            "legacy mixed providers keep exact authentication and one recognition event");
        const auto inventory = singbox::inspectProfileSourceObjects(options);
        check(inventory.error.empty() && inventory.json.empty() && inventory.sourceObjects.size() == result.sourceObjects.size() && LegacyEvents(inventory) == 1,
            "legacy inactive inventory keeps source identities and the same format diagnostic");
        const auto [excludedOptions, excluded] = protocol_entry_tests::CompileProvider(mixed, kind, directory.path, true, "^modern fixture$");
        check(excluded.error.empty() && LegacyEvents(excluded) == 0 && protocol_entry_tests::Node(excluded, "p5-main", "legacy fixture").empty(),
            "excluded legacy nodes produce neither a member nor a compatibility event");
        const auto [badOptions, bad] = protocol_entry_tests::CompileProvider(mixed + "\nss://AB==#excluded", kind, directory.path, false, "^modern fixture$");
        check(!bad.error.empty() && bad.json.empty(), "invalid excluded legacy input rejects the entire URI collection");
    }
#if !defined(__ANDROID__) && !defined(CLASHFLUX_IOS)
    singbox::CompileOptions sources; sources.mainConnectionId = "p6-main"; sources.profileYaml = legacy;
    sources.auxiliarySources = {{"p6-aux", "legacy secondary", mixed, true}};
    sources.globalRules = {{.match = vpn::MatchKind::DomainSuffix, .pattern = "secondary.test", .connectionId = "p6-aux",
        .id = "p6-legacy", .tier = vpn::RuleTier::SourcePolicy, .targetKind = vpn::TargetKind::Node, .targetObject = "legacy fixture"}};
    const auto composed = singbox::compileConfig(sources);
    const auto main = protocol_entry_tests::Node(composed, "p6-main", "legacy fixture"), aux = protocol_entry_tests::Node(composed, "p6-aux", "legacy fixture");
    check(composed.error.empty() && !main.empty() && !aux.empty() && main["tag"] != aux["tag"] &&
        protocol_entry_tests::SemanticNode(main) == protocol_entry_tests::SemanticNode(aux), "legacy secondary keeps credentials and separates same-name identities");
#endif
    const auto cache = directory.path / "cache"; std::filesystem::create_directory(cache);
    singbox::CompileOptions prepare; prepare.mainConnectionId = "p6-prepare"; prepare.proxyProviderCacheDir = cache.string();
    prepare.profileYaml = protocol_entry_tests::ProviderDocument(mixed, "http");
    auto response = mixed; bool approve = true; int commits = 0;
    rule_provider_cache::Operations operations;
    operations.fetchProxy = [&](const auto&, const auto& path, std::string&) { std::ofstream(path, std::ios::binary) << response; return true; };
    operations.check = [&](const auto&, const auto& result, std::string& error) {
        if (!approve) error = "deliberate candidate rejection";
        return approve && result.error.empty() && !result.json.empty();
    };
    operations.commit = [&](const auto& source, const auto& target, std::string& error) {
        ++commits; std::error_code ec; std::filesystem::rename(source, target, ec); error = ec.message(); return !ec;
    };
    singbox::CompileResult good; std::string error;
    check(rule_provider_cache::Prepare(prepare, operations, good, error) && commits == 1, "Prepare approves complete legacy/modern snapshots before commit");
    if (good.httpProxyProviders.empty()) { check(false, "prepared legacy provider identity is retained"); return; }
    const auto resource = good.httpProxyProviders.front();
    const auto cached = singbox::ReadHttpProxyProviderCache(resource, cache, error);
    prepare.forceProviderRefreshSources = {"p6-prepare"};
    for (int failure = 0; failure < 2; ++failure) {
        response = failure == 0 ? mixed + "\nss://AB==" : mixed; approve = failure == 0;
        singbox::CompileResult rejectedResult;
        check(!rule_provider_cache::Prepare(prepare, operations, rejectedResult, error) && commits == 1 && cached == mixed &&
            singbox::ReadHttpProxyProviderCache(resource, cache, error) == cached, "legacy format/candidate failure preserves approved raw cache and commit count");
    }
}

void LegacyRun(const std::filesystem::path& fixtures = {}) {
    legacyAccepted = legacyRejected = 0;
    for (const auto* password : {"exact key", " PRIVATE @/#?+%00%2F: key ", "%00", "%252F", "😀中文", "  "})
        for (const bool padding : {false, true}) LegacyAccept(Legacy(password, "example.test:443", "aes-256-gcm", padding), node_uri_ss_tests::Expected(password), fixtures);
    LegacyAccept(Legacy("key", "[2001:db8::1]:8443", "chacha20-ietf-poly1305"), node_uri_ss_tests::Expected("key", "chacha20-ietf-poly1305", "2001:db8::1", 8443), fixtures);
    LegacyAccept(Legacy("key", "example.test:443", "aes-256-cfb"), node_uri_ss_tests::Expected("key", "aes-256-cfb"), fixtures);
    const auto raw = Legacy();
    LegacyAccept(encodeNodeFixture(raw), node_uri_ss_tests::Expected(" exact +%40: key "), fixtures);
    LegacyAccept("SS://" + raw.substr(5), node_uri_ss_tests::Expected(" exact +%40: key "), fixtures);
    LegacyAccept("ss://Y2hhY2hhMjAtaWV0Zi1wb2x5MTMwNTp0ZXN0LyFAIzpAMTkyLjE2OC4xMDAuMTo4ODg4#example-server",
        node_uri_ss_tests::Expected("test/!@#:", "chacha20-ietf-poly1305", "192.168.100.1", 8888), fixtures, "example-server");
    LegacyAccept("ss://" + encodeNodeFixture("aes-256-gcm:key@example.test:443"), node_uri_ss_tests::Expected("key"), fixtures, "Shadowsocks 1");
    for (const auto* host : {"example.test", "example.test:0", "example.test:65536", "[::1%25lo]:443", "::1:443", "[:::1]:443",
            "example.test:443/path", "example.test:443?plugin=x", "example.test:443#name", "%65xample.test:443"}) LegacyReject(Legacy("PRIVATE", host));
    for (const auto* body : {"PRIVATE", "aes-256-gcm:PRIVATE", "aes-256-gcm:@example.test:443", "unknown:PRIVATE@example.test:443",
            "ss://aes-256-gcm:PRIVATE@example.test:443", "aes-256-gcm:PRIVATE\n@example.test:443"}) LegacyReject("ss://" + encodeNodeFixture(body));
    LegacyReject("ss://AB=="); LegacyReject("ss://YWVzLTI1Ni1nY2068J-YgEBleGFtcGxlLnRlc3Q6NDQz");
    LegacyReject(Legacy(encodeNodeFixture(std::string(32, 'a')), "example.test:443", "2022-blake3-aes-256-gcm"));
    LegacyReject(raw.substr(0, raw.find('#')) + "?plugin=obfs-local#legacy%20fixture"); LegacyReject(raw + "#second");
    LegacyReject("ss://" + encodeNodeFixture("aes-256-gcm:PRIVATE@example.test:443") + "?plugin=obfs-local");
    LegacyReject("ss://" + encodeNodeFixture(std::string("aes-256-gcm:PRIVATE") + char(0) + "@example.test:443"));
    LegacyReject(Legacy("PRIVATE", "example.test:443", "aes-256-gcm", true, "DIRECT"));
    LegacyReject(Legacy("PRIVATE", "example.test:443", "aes-256-gcm", true, std::string(64 * 1024, 'a')));
    LegacyReject(raw + "\n" + node_uri_ss_tests::Uri("PRIVATE", "aes-256-gcm", "example.test:443", false, "legacy fixture"));
    LegacyReject(raw + "\nssr://PRIVATE");
    LegacyEntries();
    std::println("Legacy SS: {} positive cases, {} rejection cases, mixed provider/preparation boundaries passed", legacyAccepted, legacyRejected);
}

json CatalogJson() {
    const auto catalog = singbox::NodeSubscriptionCapabilities();
    auto formats = json::array();
    for (const auto& format : catalog.formats) formats.push_back({{"id", format.id}, {"protocol", format.protocol},
        {"native_type", format.nativeType}, {"schemes", format.schemes}, {"case_sensitive", format.caseSensitive}, {"legacy", format.legacy}});
    return {{"schema_version", 1}, {"audited_core_version", catalog.auditedCoreVersion},
        {"audited_core_revision", catalog.auditedCoreRevision}, {"formats", formats}};
}

void CatalogRun(const std::filesystem::path& fixtures = {}) {
    const auto catalog = CatalogJson();
    std::ifstream file(std::filesystem::path(__FILE__).parent_path() / "fixtures" / "node-input-catalog.json");
    check(bool(file), "independent input capability fixtures exist");
    if (!file) return;
    const auto cases = json::parse(file).at("cases");
    std::set<std::string> ids, tested;
    for (const auto& format : catalog.at("formats")) check(ids.insert(format.at("id")).second, "input capability IDs are unique");
    const RuleSetTestDirectory directory;
    for (const auto& fixture : cases) {
        const auto formatId = fixture.at("format").get<std::string>(); tested.insert(formatId);
        const auto format = std::ranges::find_if(catalog.at("formats"), [&](const auto& item) { return item.at("id") == formatId; });
        check(format != catalog.at("formats").end(), "every executable input fixture is registered: " + formatId);
        if (format == catalog.at("formats").end()) continue;
        for (const auto* field : {"protocol", "native_type", "schemes", "case_sensitive", "legacy"})
            check(format->at(field) == fixture.at(field), "capability metadata agrees with independent format expectations");
        const auto body = fixture.at("input").get<std::string>(), name = fixture.at("name").get<std::string>();
        const auto checkCompiled = [&](const singbox::CompileResult& result, const std::string& source) {
            const auto node = protocol_entry_tests::Node(result, source, name);
            check(result.error.empty() && !node.empty(), "registered input reaches the production model and emitter: " + formatId);
            if (!node.empty()) check(protocol_entry_tests::SemanticNode(node) == fixture.at("expected_node"), "registered input generates the independently expected native schema");
            const auto notes = protocol_entry_tests::NodeNotes(result, source);
            check(notes.size() == fixture.at("node_approx").get<unsigned>() && std::ranges::all_of(notes, [](const auto& note) {
                return note.at("level") == int(singbox::Fidelity::Approx);
            }), "registered input retains the declared mapping ledger");
            check(LegacyEvents(result) == unsigned(fixture.at("legacy").get<bool>()) && result.warnings.size() == result.fidelity.size() + LegacyEvents(result),
                "format recognition events and semantic notes remain distinct and complete");
        };
        singbox::CompileOptions options; options.mainConnectionId = "catalog-main"; options.profileYaml = body;
        const auto result = singbox::compileConfig(options); checkCompiled(result, "catalog-main");
        const auto inventory = singbox::inspectProfileSourceObjects(options);
        check(inventory.error.empty() && inventory.json.empty() && inventory.sourceObjects.size() == result.sourceObjects.size(), "registered inputs have an equivalent inactive inventory");
        for (const auto* kind : {"file", "http"}) {
            const auto [provider, compiled] = protocol_entry_tests::CompileProvider(body, kind, directory.path);
            checkCompiled(compiled, "p5-main");
        }
        for (const auto& schemeJson : fixture.at("schemes")) {
            auto scheme = schemeJson.get<std::string>();
            const auto suffix = body.substr(body.find("://"));
            options.profileYaml = scheme + suffix; checkCompiled(singbox::compileConfig(options), "catalog-main");
            std::ranges::transform(scheme, scheme.begin(), [](unsigned char c) { return char(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c); });
            options.profileYaml = scheme + suffix;
            const auto upper = singbox::compileConfig(options);
            if (fixture.at("case_sensitive")) check(!upper.error.empty() && upper.json.empty(), "VMess preserves its existing case-sensitive scheme boundary");
            else checkCompiled(upper, "catalog-main");
        }
        if (!fixtures.empty() && !result.json.empty()) {
            std::filesystem::create_directories(fixtures);
            std::ofstream(fixtures / ("input-" + fixture.at("id").get<std::string>() + ".json")) << result.json;
        }
    }
    check(ids == tested, "capability catalog and independent executable fixture coverage are identical");
    if (!fixtures.empty()) std::ofstream(fixtures / "catalog.json") << catalog.dump(2);
    std::println("Node input catalog: {} formats, {} independent fixtures, ordinary/provider/alias/ledger checks passed", ids.size(), cases.size());
}
} // namespace node_input_tests
