// Independent public-entry expectations for HY2, TUIC v5 and AnyTLS URI subsets.
namespace node_uri_secure_tests {
using Query = std::vector<std::pair<std::string, std::string>>;
const std::string uuid = "11111111-2222-3333-4444-555555555555", password = " exact URI +%40: 😀 key ";

std::string Uri(std::string scheme, const Query& query = {}, std::string host = "example.test:443", std::string name = "secure fixture") {
    auto user = node_uri_ss_tests::Percent(password);
    if (scheme == "tuic") user = node_uri_ss_tests::Percent(uuid) + ":" + user;
    std::string result = scheme + "://" + user + "@" + host + "/";
    for (const auto& [key, value] : query) result += (result.find('?') == std::string::npos ? "?" : "&") +
        node_uri_ss_tests::Percent(key) + "=" + node_uri_ss_tests::Percent(value);
    return result + "#" + node_uri_ss_tests::Percent(name);
}

json Expected(const std::string& kind) {
    json node{{"type", kind}, {"server", "example.test"}, {"server_port", 443}, {"password", password},
        {"tls", {{"enabled", true}, {"server_name", "example.test"}}}};
    if (kind == "tuic") node["uuid"] = uuid;
    if (kind == "anytls") node["tls"]["utls"] = {{"enabled", true}, {"fingerprint", "chrome"}};
    return node;
}
unsigned accepted = 0, rejected = 0;

void Accept(const std::string& body, const json& expected, const std::filesystem::path& fixtures = {},
        const std::string& name = "secure fixture", unsigned approximate = 0) {
    ++accepted;
    const auto result = node_uri_ss_tests::Compile(body);
    const auto node = protocol_entry_tests::Node(result, "p6-main", name);
    check(result.error.empty() && !node.empty(), "HY2/TUIC/AnyTLS URI compiles: " + std::to_string(accepted));
    if (!node.empty()) check(protocol_entry_tests::SemanticNode(node) == expected,
        "URI keeps independently expected authentication, defaults and protocol fields: " + std::to_string(accepted));
    const auto notes = protocol_entry_tests::NodeNotes(result, "p6-main");
    check(notes.size() == approximate && std::ranges::all_of(notes, [](const auto& note) {
        return note.at("level") == int(singbox::Fidelity::Approx);
    }), "URI accounts only for its declared approximations");
    std::string error;
    check(singbox::DetectProfileFormat(body) == singbox::ProfileFormat::NodeSubscription && singbox::ValidateProfileDocument(body, error),
        "secure URI uses ordinary profile recognition and document validation");
    check(singbox::InspectProfileRules(body, {}).original.size() == 1, "secure URI projects the generated selection policy");
    singbox::CompileOptions options; options.profileYaml = body; options.mainConnectionId = "p6-main";
    const auto inventory = singbox::inspectProfileSourceObjects(options);
    check(inventory.error.empty() && inventory.json.empty() && std::ranges::any_of(inventory.sourceObjects, [&](const auto& object) {
        return object.sourceId == "p6-main" && object.objectId == name && object.kind == vpn::TargetKind::Node;
    }), "secure URI inventory retains source identity without publishing a runnable candidate");
    if (!fixtures.empty() && !result.json.empty()) {
        std::filesystem::create_directories(fixtures);
        std::ofstream(fixtures / ("secure-uri-" + std::to_string(accepted) + ".json")) << result.json;
    }
}

void Reject(const std::string& body) {
    ++rejected;
    const auto result = node_uri_ss_tests::Compile(body); std::string error;
    check(!result.error.empty() && result.json.empty() && !singbox::ValidateProfileDocument(body, error),
        "secure URI rejects complete invalid candidates: " + std::to_string(rejected));
    const auto safe = [](const auto& text) {
        return text.find("PRIVATE") == std::string::npos && text.find("11111111") == std::string::npos && text.find("://") == std::string::npos;
    };
    check(safe(result.error) && safe(error) && std::ranges::all_of(result.fidelity, [&](const auto& note) {
        return safe(note.detail) && safe(note.action);
    }), "URI errors omit raw credentials, UUIDs and links");
}

void Entries() {
    const RuleSetTestDirectory directory;
    const json vmess{{"ps", "VM fixture"}, {"add", "example.test"}, {"port", 443}, {"id", uuid}, {"net", "tcp"}};
    const auto mixed = Uri("hy2", {}, "example.test:443", "HY fixture") + "\n" + Uri("tuic", {}, "example.test:443", "TUIC fixture") +
        "\n" + Uri("anytls") + "\n" + node_uri_v2ray_tests::Uri(true) + "\n" + node_uri_v2ray_tests::Uri(false, {}, {}, "Trojan fixture") +
        "\n" + node_uri_ss_tests::Uri() + "\nvmess://" + encodeNodeFixture(vmess.dump());
    for (const auto* kind : {"file", "http"}) {
        const auto [options, result] = protocol_entry_tests::CompileProvider(encodeNodeFixture(mixed), kind, directory.path,
            true, {}, "global-client-fingerprint: random\n");
        const auto hy = protocol_entry_tests::Node(result, "p5-main", "HY fixture"), tuic = protocol_entry_tests::Node(result, "p5-main", "TUIC fixture"),
            any = protocol_entry_tests::Node(result, "p5-main", "secure fixture");
        check(result.error.empty() && !hy.empty() && !tuic.empty() && !any.empty() && !hy["tls"].contains("utls") &&
            !tuic["tls"].contains("utls") && any["tls"]["utls"]["fingerprint"] == "chrome" && protocol_entry_tests::NodeNotes(result, "p5-main").empty(),
            "seven-protocol provider preserves QUIC TLS and AnyTLS format defaults without inherited random fingerprints");
        const auto inventory = singbox::inspectProfileSourceObjects(options);
        check(inventory.error.empty() && inventory.json.empty() && inventory.sourceObjects.size() == result.sourceObjects.size(),
            "mixed secure URI provider has an equivalent inactive inventory");
        auto filteredOptions = options;
        filteredOptions.profileYaml.insert(filteredOptions.profileYaml.find("    type:"), "    exclude-type: hysteria2|tuic|anytls\n");
        if (std::string_view(kind) == "http") {
            filteredOptions.proxyProviderContents.clear();
            for (const auto& resource : singbox::compileConfig(filteredOptions).httpProxyProviders)
                filteredOptions.proxyProviderContents[resource.cacheKey] = mixed;
        }
        const auto filtered = singbox::compileConfig(filteredOptions);
        check(filtered.error.empty() && protocol_entry_tests::Node(filtered, "p5-main", "HY fixture").empty() &&
            protocol_entry_tests::Node(filtered, "p5-main", "TUIC fixture").empty() && protocol_entry_tests::Node(filtered, "p5-main", "secure fixture").empty() &&
            !protocol_entry_tests::Node(filtered, "p5-main", "SS fixture").empty() && !protocol_entry_tests::Node(filtered, "p5-main", "Trojan fixture").empty(),
            "provider uses canonical hysteria2/tuic/anytls URI types, including hy2 scheme aliases");
        const auto [badOptions, bad] = protocol_entry_tests::CompileProvider(mixed + "\nanytls://PRIVATE@example.test:443?insecure=bad#excluded",
            kind, directory.path, false, "^HY fixture$");
        check(!bad.error.empty() && bad.json.empty(), "malformed excluded secure URI rejects the complete collection before filtering");
    }
#if !defined(__ANDROID__) && !defined(CLASHFLUX_IOS)
    singbox::CompileOptions sources; sources.mainConnectionId = "p6-main"; sources.profileYaml = Uri("anytls");
    sources.auxiliarySources = {{"p6-aux", "secure secondary", mixed, true}};
    sources.globalRules = {{.match = vpn::MatchKind::DomainSuffix, .pattern = "secondary.test", .connectionId = "p6-aux",
        .id = "p6-secure", .tier = vpn::RuleTier::SourcePolicy, .targetKind = vpn::TargetKind::Node, .targetObject = "secure fixture"}};
    const auto composed = singbox::compileConfig(sources);
    const auto main = protocol_entry_tests::Node(composed, "p6-main", "secure fixture"), aux = protocol_entry_tests::Node(composed, "p6-aux", "secure fixture");
    check(composed.error.empty() && !main.empty() && !aux.empty() && main["tag"] != aux["tag"] &&
        protocol_entry_tests::SemanticNode(main) == protocol_entry_tests::SemanticNode(aux), "same-name AnyTLS sources retain isolated identities and exact credentials");
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
    check(rule_provider_cache::Prepare(prepare, operations, good, error) && commits == 1, "Prepare approves complete seven-protocol URI snapshots before commit");
    if (good.httpProxyProviders.empty()) { check(false, "prepared secure provider identity is retained"); return; }
    const auto resource = good.httpProxyProviders.front();
    const auto cached = singbox::ReadHttpProxyProviderCache(resource, cache, error);
    check(cached == mixed, "approved cache keeps complete original secure URI bytes");
    prepare.forceProviderRefreshSources = {"p6-prepare"};
    for (int failure = 0; failure < 2; ++failure) {
        response = failure == 0 ? mixed + "\nhy2://PRIVATE@example.test:0" : mixed; approve = failure == 0;
        singbox::CompileResult rejectedResult;
        check(!rule_provider_cache::Prepare(prepare, operations, rejectedResult, error) && commits == 1 &&
            singbox::ReadHttpProxyProviderCache(resource, cache, error) == cached, "secure URI syntax/candidate failure preserves the approved cache and commit count");
    }
}

void Run(const std::filesystem::path& fixtures = {}) {
    accepted = rejected = 0;
    for (const std::string scheme : {"hy2", "hysteria2", "tuic", "anytls"}) {
        const auto type = scheme == "hy2" ? "hysteria2" : scheme;
        Accept(Uri(scheme), Expected(type), fixtures);
        for (const auto* credential : {" PRIVATE+%2F: key ", "😀中文", "  "}) {
            auto expected = Expected(type); expected["password"] = credential;
            const auto user = (scheme == "tuic" ? uuid + ":" : "") + node_uri_ss_tests::Percent(credential);
            Accept(scheme + "://" + user + "@example.test:443#secure%20fixture", expected, fixtures);
        }
        for (const auto* host : {"[2001:db8::1]:8443", "192.0.2.1:1"}) {
            auto expected = Expected(type); expected["server"] = std::string_view(host).starts_with('[') ? "2001:db8::1" : "192.0.2.1";
            expected["server_port"] = std::string_view(host).starts_with('[') ? 8443 : 1;
            expected["tls"]["server_name"] = expected["server"];
            Accept(Uri(scheme, {}, host), expected, fixtures);
        }
        for (const bool insecure : {false, true}) {
            auto expected = Expected(type); expected["tls"]["server_name"] = "tls.example.test";
            if (insecure) expected["tls"]["insecure"] = true;
            Accept(Uri(scheme, {{"sni", "tls.example.test"}, {"insecure", insecure ? "1" : "0"}}), expected, fixtures);
        }
        for (const Query query : std::vector<Query>{{{"unknown", "PRIVATE"}}, {{"sni", ""}}, {{"sni", "one"}, {"sni", "two"}},
                {{"insecure", ""}}, {{"insecure", "2"}}, {{"insecure", "TRUE"}}, {{"insecure", "yes"}},
                {{"security", "none"}}, {{"pinSHA256", ""}}, {{"ech", "PRIVATE"}}, {{"pbk", "PRIVATE"}}, {{"sid", ""}},
                {{"type", "ws"}}, {{"path", "/PRIVATE"}}, {{"udp", "false"}}}) Reject(Uri(scheme, query));
        for (const auto* host : {"example.test:", "example.test:0", "example.test:65536", "example.test:+443", "[::1%25lo]:443", "[:::1]:443", "::1:443"})
            Reject(Uri(scheme, {}, host));
        Reject(scheme + "://PRIVATE@example.test:443/path");
        Reject(scheme + "://PRIVATE@example.test:443?sni=a&%73ni=b");
        Reject(scheme + "://PRIVATE%00@example.test:443");
        Reject(scheme + "://PRIVATE@@example.test:443");
        Reject(scheme + "://PRIVATE@example.test:443#" + std::string(64 * 1024, 'a'));
    }
    for (const auto* scheme : {"hy2", "hysteria2", "anytls"}) {
        const std::string type = std::string_view(scheme) == "anytls" ? "anytls" : "hysteria2";
        Accept(Uri(scheme, {}, "example.test"), Expected(type), fixtures);
        for (const auto* value : {"true", "false"}) Reject(Uri(scheme, {{"insecure", value}}));
    }
    auto emptyHy = Expected("hysteria2"); emptyHy["password"] = "";
    Accept("HY2://example.test#secure%20fixture", emptyHy, fixtures);
    Accept("hysteria2://@example.test/#secure%20fixture", emptyHy, fixtures);
    for (const auto* obfs : {"salamander", "gecko"}) {
        auto expected = Expected("hysteria2"); expected["obfs"] = {{"type", obfs}, {"password", " PRIVATE obfs +%40: "}};
        Accept(Uri("hy2", {{"obfs", obfs}, {"obfs-password", " PRIVATE obfs +%40: "}}), expected, fixtures);
    }
    for (const auto* ports : {"443,5000-5002,65535", "443-443"}) {
        auto expected = Expected("hysteria2"); expected.erase("server_port"); expected["hop_interval"] = "30s"; expected["hop_interval_max"] = "30s";
        expected["server_ports"] = std::string_view(ports) == "443-443" ? json::array({"443:443"}) : json::array({"443:443", "5000:5002", "65535:65535"});
        Accept(Uri("hy2", {}, "example.test:" + std::string(ports)), expected, fixtures);
    }
    std::string ports;
    auto bounded = Expected("hysteria2"); bounded.erase("server_port"); bounded["hop_interval"] = "30s"; bounded["hop_interval_max"] = "30s"; bounded["server_ports"] = json::array();
    for (unsigned i = 1; i <= 128; ++i) {
        if (i != 1) ports += ',';
        ports += std::to_string(i); bounded["server_ports"].push_back(std::to_string(i) + ":" + std::to_string(i));
    }
    Accept(Uri("hy2", {}, "example.test:" + ports), bounded, fixtures);
    Reject(Uri("hy2", {}, "example.test:" + ports + ",129"));
    for (const auto* value : {"0-5", "9-2", "1-65536", "1--2", "1-2-3", "1,,2", "1,", ",1", "1,1", "1-3,3-5", "1%2C2", "1-", "-1", "1,abc"})
        Reject(Uri("hy2", {}, "example.test:" + std::string(value)));
    for (const Query query : std::vector<Query>{{{"obfs", ""}, {"obfs-password", "PRIVATE"}}, {{"obfs", "none"}, {"obfs-password", "PRIVATE"}},
            {{"obfs", "salamander"}}, {{"obfs-password", "PRIVATE"}}, {{"obfs", "gecko"}, {"obfs-password", ""}},
            {{"alpn", "h3"}}, {{"fp", "chrome"}}, {{"hop-interval", "5"}}, {{"upmbps", "100"}}, {{"mport", "1,2"}}}) Reject(Uri("hy2", query));
    for (const auto* kind : {"tuic", "anytls"}) {
        auto expected = Expected(kind); expected["tls"]["alpn"] = {"h3", "test+%2F"};
        Accept(Uri(kind, {{"alpn", "h3,test+%2F"}, {"security", "tls"}}), expected, fixtures);
        for (const auto* alpn : {"", "h3,,test", "h3, test"}) Reject(Uri(kind, {{"alpn", alpn}}));
        Reject(Uri(kind, {{"alpn", std::string(256, 'a')}}));
    }
    for (const auto* mode : {"native", "quic"}) {
        auto expected = Expected("tuic"); expected["udp_relay_mode"] = mode; expected["zero_rtt_handshake"] = true;
        expected["tls"]["disable_sni"] = true; expected["congestion_control"] = "bbr";
        Accept(Uri("tuic", {{"udp_relay_mode", mode}, {"udp-relay-mode", mode}, {"reduce-rtt", "1"}, {"zero_rtt_handshake", "true"},
            {"disable_sni", "true"}, {"disable-sni", "1"}, {"congestion_control", "bbr"}, {"congestion-controller", "bbr"},
            {"insecure", "0"}, {"allow_insecure", "false"}}), expected, fixtures);
    }
    for (const auto* cc : {"cubic", "new_reno"}) {
        auto expected = Expected("tuic"); expected["congestion_control"] = cc;
        Accept(Uri("tuic", {{"congestion_control", cc}, {"reduce_rtt", "false"}, {"disable_sni", "0"}}), expected, fixtures);
    }
    auto emptyTuic = Expected("tuic"); emptyTuic["password"] = "";
    Accept("tuic://" + uuid + ":@example.test:443#secure%20fixture", emptyTuic, fixtures);
    for (const Query query : std::vector<Query>{{{"allow_insecure", "1"}, {"insecure", "false"}}, {{"disable_sni", "0"}, {"disable-sni", "1"}},
            {{"reduce_rtt", "false"}, {"zero-rtt-handshake", "true"}}, {{"udp_relay_mode", "quic"}, {"udp-relay-mode", "native"}},
            {{"congestion_control", "bbr"}, {"congestion-controller", "cubic"}}, {{"congestion_control", ""}}, {{"congestion_control", "reno"}},
            {{"udp_relay_mode", ""}}, {{"udp_relay_mode", "stream"}}, {{"udp_over_stream", "true"}}, {{"heartbeat", "10s"}},
            {{"fp", "chrome"}}, {{"token", "PRIVATE"}}, {{"version", "4"}}, {{"reduce_rtt", "2"}}}) Reject(Uri("tuic", query));
    Reject("tuic://PRIVATE@example.test:443"); Reject("tuic://" + uuid + "%3APRIVATE@example.test:443");
    Reject("tuic://PRIVATE:password@example.test:443"); Reject(Uri("tuic", {}, "example.test"));
    Reject(Uri("tuic", {}, "example.test:443,444")); Reject(Uri("tuic", {}, "example.test:443-444"));
    Reject("anytls://example.test"); Reject("anytls://@example.test"); Reject(Uri("anytls", {}, "example.test:443,444"));
    for (const auto* fp : {"firefox", "chrome_psk", "random"}) {
        auto expected = Expected("anytls"); expected["tls"]["utls"]["fingerprint"] = fp;
        Accept(Uri("anytls", {{"fp", fp}}), expected, fixtures, "secure fixture", std::string_view(fp) == "firefox" ? 0 : 1);
    }
    Reject(Uri("anytls", {{"fp", ""}})); Reject(Uri("anytls", {{"idle_session_timeout", "30"}}));
    Accept(encodeNodeFixture(Uri("hy2")), Expected("hysteria2"), fixtures);
    auto ipAny = Expected("anytls"); ipAny["tls"]["server_name"] = "127.0.0.1";
    Accept(Uri("anytls", {{"sni", "127.0.0.1"}}), ipAny, fixtures);
    for (const auto* kind : {"hy2", "anytls"}) {
        auto expected = Expected(std::string_view(kind) == "hy2" ? "hysteria2" : "anytls");
        expected["server"] = expected["tls"]["server_name"] = "2001:db8::1";
        Accept(Uri(kind, {}, "[2001:db8::1]"), expected, fixtures);
        Reject(Uri(kind, {}, "[::1]tail"));
    }
    Accept("anytls://" + node_uri_ss_tests::Percent(password) + "@example.test", Expected("anytls"), fixtures, "AnyTLS 1");
    Accept("TUIC://" + uuid + ":" + node_uri_ss_tests::Percent(password) + "@example.test:443", Expected("tuic"), fixtures, "TUIC 1");
    Reject("ss://aes-256-gcm:PRIVATE@example.test");
    Accept("hy2://key@example.test", [&] { auto expected = Expected("hysteria2"); expected["password"] = "key"; return expected; }(), fixtures, "Hysteria2 1");
    Reject(Uri("hy2") + "\n" + Uri("anytls"));
    Entries();
    std::println("HY2/TUIC/AnyTLS URI: {} positive cases, {} rejection cases, mixed provider/preparation boundaries passed", accepted, rejected);
}
} // namespace node_uri_secure_tests
