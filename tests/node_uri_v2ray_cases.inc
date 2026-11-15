// Public API contracts for the supported Xray-style VLESS / Trojan URI subset.
namespace node_uri_v2ray_tests {
const std::string uuid = "11111111-2222-3333-4444-555555555555";
const std::string password = " exact Trojan +%40: key ";
using Query = std::vector<std::pair<std::string, std::string>>;

std::string Uri(bool vless, const Query& query = {}, std::string credential = {}, std::string name = "VT fixture") {
    std::string uri = std::string(vless ? "vless://" : "trojan://") +
        node_uri_ss_tests::Percent(credential.empty() ? (vless ? uuid : password) : credential) + "@example.test:443";
    for (const auto& [key, value] : query)
        uri += (uri.find('?') == std::string::npos ? "?" : "&") + node_uri_ss_tests::Percent(key) + "=" + node_uri_ss_tests::Percent(value);
    return uri + "#" + node_uri_ss_tests::Percent(name);
}

json Expected(bool vless, bool tls) {
    json result{{"type", vless ? "vless" : "trojan"}, {"server", "example.test"}, {"server_port", 443}};
    result[vless ? "uuid" : "password"] = vless ? uuid : password;
    if (tls) result["tls"] = {{"enabled", true}, {"server_name", "example.test"}, {"utls", {{"enabled", true}, {"fingerprint", "chrome"}}}};
    return result;
}

unsigned accepted = 0, rejected = 0;
void Accept(const std::string& body, const json& expected, const std::filesystem::path& fixtures = {},
        const std::string& name = "VT fixture", unsigned approximate = 0) {
    ++accepted;
    const auto result = node_uri_ss_tests::Compile(body);
    const auto node = protocol_entry_tests::Node(result, "p6-main", name);
    check(result.error.empty() && !node.empty(), "VLESS/Trojan positive URI compiles: " + std::to_string(accepted));
    if (!node.empty()) check(protocol_entry_tests::SemanticNode(node) == expected,
        "VLESS/Trojan preserves independently expected defaults and fields: " + std::to_string(accepted));
    const auto notes = protocol_entry_tests::NodeNotes(result, "p6-main");
    check(notes.size() == approximate && std::ranges::all_of(notes, [](const auto& note) {
        return note.at("level") == int(singbox::Fidelity::Approx);
    }), "URI records only the declared fingerprint approximations");
    std::string error;
    check(singbox::DetectProfileFormat(body) == singbox::ProfileFormat::NodeSubscription &&
        singbox::ValidateProfileDocument(body, error), "VLESS/Trojan passes ordinary document recognition and validation");
    check(singbox::InspectProfileRules(body, {}).original.size() == 1, "VLESS/Trojan has the same generated selection policy");
    singbox::CompileOptions options; options.profileYaml = body; options.mainConnectionId = "p6-main";
    const auto inventory = singbox::inspectProfileSourceObjects(options);
    check(inventory.error.empty() && inventory.json.empty() && std::ranges::any_of(inventory.sourceObjects, [&](const auto& object) {
        return object.sourceId == "p6-main" && object.objectId == name && object.kind == vpn::TargetKind::Node;
    }), "URI inventory preserves stable source identity without publishing a runnable plan");
    if (!fixtures.empty() && !result.json.empty()) {
        std::filesystem::create_directories(fixtures);
        std::ofstream(fixtures / ("vt-uri-" + std::to_string(accepted) + ".json")) << result.json;
    }
}

void Reject(const std::string& body) {
    ++rejected;
    const auto result = node_uri_ss_tests::Compile(body);
    std::string error;
    check(!result.error.empty() && result.json.empty() && !singbox::ValidateProfileDocument(body, error),
        "VLESS/Trojan invalid input rejects the entire candidate: " + std::to_string(rejected));
    const auto safe = [](const auto& message) {
        return message.find("PRIVATE") == std::string::npos && message.find("11111111") == std::string::npos &&
            message.find("://") == std::string::npos;
    };
    check(safe(result.error) && safe(error) && std::ranges::all_of(result.fidelity, [&](const auto& note) {
        return safe(note.detail) && safe(note.action);
    }), "URI failures expose neither credentials nor raw links");
}

void Entries() {
    const RuleSetTestDirectory directory;
    const json vmess{{"ps", "VM fixture"}, {"add", "example.test"}, {"port", 443}, {"id", uuid}, {"net", "tcp"}};
    const auto mixed = Uri(true, {{"security", "tls"}}) + "\n" + Uri(false, {}, {}, "Trojan fixture") +
        "\n" + node_uri_ss_tests::Uri() + "\nvmess://" + encodeNodeFixture(vmess.dump());
    for (const auto* kind : {"file", "http"}) {
        const auto [options, result] = protocol_entry_tests::CompileProvider(encodeNodeFixture(mixed), kind, directory.path,
            true, {}, "global-client-fingerprint: firefox\n");
        check(result.error.empty() && protocol_entry_tests::Node(result, "p5-main", "VT fixture")["tls"]["utls"]["fingerprint"] == "chrome" &&
            protocol_entry_tests::Node(result, "p5-main", "Trojan fixture")["tls"]["utls"]["fingerprint"] == "chrome",
            "VLESS/Trojan URI defaults are format defaults, independent of the owning Clash fingerprint");
        const auto inventory = singbox::inspectProfileSourceObjects(options);
        check(inventory.error.empty() && inventory.json.empty() && inventory.sourceObjects.size() == result.sourceObjects.size(),
            "four-protocol mixed provider exposes the same inactive object inventory");
        auto filteredOptions = options;
        filteredOptions.profileYaml.insert(filteredOptions.profileYaml.find("    type:"), "    exclude-type: vless|trojan\n");
        if (std::string_view(kind) == "http") {
            filteredOptions.proxyProviderContents.clear();
            for (const auto& resource : singbox::compileConfig(filteredOptions).httpProxyProviders)
                filteredOptions.proxyProviderContents[resource.cacheKey] = mixed;
        }
        const auto filtered = singbox::compileConfig(filteredOptions);
        check(filtered.error.empty() && protocol_entry_tests::Node(filtered, "p5-main", "VT fixture").empty() &&
            protocol_entry_tests::Node(filtered, "p5-main", "Trojan fixture").empty() &&
            !protocol_entry_tests::Node(filtered, "p5-main", "SS fixture").empty() &&
            !protocol_entry_tests::Node(filtered, "p5-main", "VM fixture").empty(), "provider filters actual vless/trojan URI types");
        const auto [badOptions, bad] = protocol_entry_tests::CompileProvider(mixed + "\n" + Uri(false, {{"security", "none"}}, "PRIVATE", "excluded"),
            kind, directory.path, false, "^VT fixture$");
        check(!bad.error.empty() && bad.json.empty(), "invalid excluded Trojan rejects the complete URI collection before filtering");
    }
#if !defined(__ANDROID__) && !defined(CLASHFLUX_IOS)
    singbox::CompileOptions sources; sources.mainConnectionId = "p6-main"; sources.profileYaml = Uri(true);
    sources.auxiliarySources = {{"p6-aux", "VLESS secondary", Uri(true), true}};
    sources.globalRules = {{.match = vpn::MatchKind::DomainSuffix, .pattern = "secondary.test", .connectionId = "p6-aux",
        .id = "p6-vless", .tier = vpn::RuleTier::SourcePolicy, .targetKind = vpn::TargetKind::Node, .targetObject = "VT fixture"}};
    const auto composed = singbox::compileConfig(sources);
    const auto main = protocol_entry_tests::Node(composed, "p6-main", "VT fixture"), aux = protocol_entry_tests::Node(composed, "p6-aux", "VT fixture");
    check(composed.error.empty() && !main.empty() && !aux.empty() && main["tag"] != aux["tag"] &&
        protocol_entry_tests::SemanticNode(main) == protocol_entry_tests::SemanticNode(aux), "same-name VLESS nodes retain source isolation and semantics");
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
    check(rule_provider_cache::Prepare(prepare, operations, good, error) && commits == 1, "production Prepare approves complete mixed URI snapshots before commit");
    if (good.httpProxyProviders.empty()) { check(false, "prepared URI resource is retained"); return; }
    const auto resource = good.httpProxyProviders.front();
    const auto cached = singbox::ReadHttpProxyProviderCache(resource, cache, error);
    check(cached == mixed, "approved provider cache retains exact mixed-format bytes");
    prepare.forceProviderRefreshSources = {"p6-prepare"};
    for (int failure = 0; failure != 2; ++failure) {
        response = failure == 0 ? mixed + "\nvless://PRIVATE" : mixed; approve = failure == 0;
        singbox::CompileResult rejectedResult;
        check(!rule_provider_cache::Prepare(prepare, operations, rejectedResult, error) && commits == 1 &&
            singbox::ReadHttpProxyProviderCache(resource, cache, error) == cached, "URI syntax/candidate failure preserves approved cache without another commit");
    }
}

void Run(const std::filesystem::path& fixtures = {}) {
    accepted = rejected = 0;
    Accept(Uri(true), Expected(true, false), fixtures);
    Accept(Uri(false), Expected(false, true), fixtures);
    for (const bool vless : {true, false}) {
        for (const auto* transport : {"tcp", "raw", "ws", "httpupgrade", "http", "h2", "grpc"}) {
            Query query{{"security", "tls"}, {"type", transport}, {"sni", "tls.example.test"}};
            auto expected = Expected(vless, true); expected["tls"]["server_name"] = "tls.example.test";
            const std::string type(transport);
            if (type == "grpc") {
                query.emplace_back("serviceName", "service+%2F"); query.emplace_back("mode", "gun"); query.emplace_back("authority", "");
                expected["transport"] = {{"type", "grpc"}, {"service_name", "service+%2F"}};
            } else if (type != "tcp" && type != "raw") {
                query.emplace_back("host", "host.example.test"); query.emplace_back("path", "/path+%2F?x=1&y=2");
                expected["transport"] = {{"type", type == "h2" ? "http" : type}, {"path", "/path+%2F?x=1&y=2"}};
                if (type == "ws") expected["transport"]["headers"] = {{"Host", "host.example.test"}};
                else if (type == "httpupgrade") expected["transport"]["host"] = "host.example.test";
                else expected["transport"]["host"] = json::array({"host.example.test"});
            } else query.emplace_back("headerType", "none");
            query.emplace_back("alpn", "h2,http/1.1"); expected["tls"]["alpn"] = {"h2", "http/1.1"};
            Accept(Uri(vless, query), expected, fixtures);
        }
        for (const auto* fp : {"firefox", "random", "chrome_psk"}) {
            auto expected = Expected(vless, true); expected["tls"]["utls"]["fingerprint"] = fp;
            Accept(Uri(vless, {{"security", "tls"}, {"fp", fp}}), expected, fixtures, "VT fixture", std::string_view(fp) == "firefox" ? 0 : 1);
        }
        for (const bool insecure : {false, true}) {
            auto expected = Expected(vless, true); if (insecure) expected["tls"]["insecure"] = true;
            Accept(Uri(vless, {{"security", "tls"}, {"insecure", insecure ? "1" : "false"}, {"allowInsecure", insecure ? "true" : "0"}}), expected, fixtures);
        }
        for (const Query query : std::vector<Query>{
                {{"unknown", "PRIVATE"}}, {{"type", ""}}, {{"type", "quic"}}, {{"type", "xhttp"}}, {{"type", "kcp"}},
                {{"security", ""}}, {{"security", "xtls"}}, {{"headerType", "http"}}, {{"host", "PRIVATE"}}, {{"path", "/PRIVATE"}},
                {{"serviceName", "PRIVATE"}}, {{"authority", "PRIVATE"}}, {{"mode", "gun"}},
                {{"security", "tls"}, {"sni", ""}}, {{"security", "tls"}, {"sni", "one"}, {"sni", "two"}},
                {{"security", "tls"}, {"fp", "none"}}, {{"security", "tls"}, {"alpn", ""}}, {{"security", "tls"}, {"alpn", "h2,,http/1.1"}},
                {{"security", "tls"}, {"alpn", "h2, http/1.1"}}, {{"security", "tls"}, {"alpn", std::string(256, 'x')}},
                {{"security", "tls"}, {"insecure", "yes"}}, {{"security", "tls"}, {"allowInsecure", "TRUE"}},
                {{"security", "tls"}, {"insecure", "1"}, {"allowInsecure", "false"}}, {{"security", "tls"}, {"pbk", "PRIVATE"}},
                {{"security", "tls"}, {"sid", ""}}, {{"security", "tls"}, {"spx", ""}}, {{"security", "tls"}, {"pqv", ""}},
                {{"type", "ws"}, {"path", ""}}, {{"type", "ws"}, {"path", "relative"}}, {{"type", "ws"}, {"host", ""}},
                {{"type", "ws"}, {"serviceName", "PRIVATE"}}, {{"type", "grpc"}}, {{"type", "grpc"}, {"serviceName", "s"}, {"mode", "multi"}},
                {{"type", "grpc"}, {"serviceName", "s"}, {"authority", "PRIVATE"}}, {{"type", "grpc"}, {"serviceName", "s"}, {"path", "/PRIVATE"}},
                {{"security", "tls"}, {"type", "http"}, {"alpn", "http/1.1"}}, {{"security", "tls"}, {"type", "h2"}, {"alpn", "h2,h3"}},
                {{"security", "tls"}, {"packetEncoding", "packetaddr"}}, {{"security", "tls"}, {"ech", "PRIVATE"}}, {{"security", "tls"}, {"pcs", "PRIVATE"}}
            }) Reject(Uri(vless, query));
    }
    for (const auto* credential : {" PRIVATE+%40:user ", "😀中文", "  "}) {
        auto expected = Expected(false, true); expected["password"] = credential;
        Accept(Uri(false, {}, credential), expected, fixtures);
    }
    auto raw = Expected(false, true); raw["password"] = "user:PRIVATE++%2F";
    Accept("trojan://user:PRIVATE+%2B%252F@example.test:443#VT%20fixture", raw, fixtures);
    auto ws = Expected(true, false); ws["transport"] = {{"type", "ws"}, {"path", "/"}};
    Accept(Uri(true, {{"type", "ws"}}), ws, fixtures);
    auto flow = Expected(true, false); flow["flow"] = ""; Accept(Uri(true, {{"flow", ""}}), flow, fixtures);
    flow = Expected(true, true); flow["flow"] = "xtls-rprx-vision";
    Accept(Uri(true, {{"security", "tls"}, {"flow", "xtls-rprx-vision"}}), flow, fixtures);
    const auto pbk = encodeNodeFixture(std::string(32, 'a'), true);
    for (const auto* sid : {"", "abcd", "ABCDEF0123456789"}) {
        auto expected = flow; expected["tls"]["reality"] = {{"enabled", true}, {"public_key", pbk}, {"short_id", sid}};
        Accept(Uri(true, {{"security", "reality"}, {"fp", "chrome"}, {"pbk", pbk}, {"sid", sid}, {"flow", "xtls-rprx-vision"}}), expected, fixtures);
    }
    auto ipv6 = Expected(true, false); ipv6["server"] = "2001:db8::1";
    Accept("VLESS://" + uuid + "@[2001:db8::1]:443#VT%20fixture", ipv6, fixtures);
    Accept(encodeNodeFixture(Uri(false)), Expected(false, true), fixtures);
    Accept(Uri(false, {}, {}, "节点+%2F"), Expected(false, true), fixtures, "节点+%2F");
    Accept("vless://" + uuid + "@example.test:443", Expected(true, false), fixtures, "VLESS 1");
    Accept("trojan://" + node_uri_ss_tests::Percent(password) + "@example.test:443#%20", Expected(false, true), fixtures, "Trojan 1");
    auto http = Expected(true, true); http["transport"] = {{"type", "http"}, {"path", "/"}, {"host", {"one.test", "two.test"}}};
    Accept(Uri(true, {{"security", "tls"}, {"type", "h2"}, {"host", "one.test,two.test"}}), http, fixtures);
    auto reality = Expected(true, true); reality["tls"]["reality"] = {{"enabled", true}, {"public_key", pbk}};
    Accept(Uri(true, {{"security", "reality"}, {"fp", "chrome"}, {"pbk", pbk}}), reality, fixtures);
    for (const Query query : std::vector<Query>{
            {{"security", "none"}, {"sni", "s"}}, {{"fp", "chrome"}}, {{"alpn", "h2"}}, {{"insecure", "false"}},
            {{"encryption", ""}}, {{"encryption", "mlkem768x25519plus.native.0rtt"}}, {{"flow", "xtls-rprx-vision"}},
            {{"flow", "unknown"}}, {{"security", "tls"}, {"flow", "xtls-rprx-vision"}, {"type", "ws"}}, {{"type", "http"}},
            {{"security", "reality"}}, {{"security", "reality"}, {"pbk", pbk}}, {{"security", "reality"}, {"fp", "chrome"}},
            {{"security", "reality"}, {"fp", "chrome"}, {"pbk", "PRIVATE"}},
            {{"security", "reality"}, {"fp", "chrome"}, {"pbk", pbk + "="}},
            {{"security", "reality"}, {"fp", "chrome"}, {"pbk", pbk}, {"sid", "a"}},
            {{"security", "reality"}, {"fp", "chrome"}, {"pbk", pbk}, {"sid", "xyz0"}},
            {{"security", "reality"}, {"fp", "chrome"}, {"pbk", pbk}, {"sid", "012345678901234567"}},
            {{"security", "reality"}, {"fp", "chrome"}, {"pbk", pbk}, {"insecure", "false"}},
            {{"security", "reality"}, {"fp", "chrome"}, {"pbk", pbk}, {"type", "ws"}}
        }) Reject(Uri(true, query));
    for (const Query query : std::vector<Query>{{{"security", "none"}}, {{"security", "reality"}}, {{"flow", ""}}, {{"encryption", "none"}}})
        Reject(Uri(false, query));
    Reject(Uri(true, {}, "PRIVATE")); Reject(Uri(true, {}, uuid + ":PRIVATE"));
    Reject("trojan://@example.test:443"); Reject(Uri(false, {}, std::string("PRIVATE\0", 8)));
    Reject("trojan://PRIVATE@example.test:443?sni=one&%73ni=two");
    Reject(Uri(true) + "\n" + Uri(false));
    Reject(Uri(false, {}, std::string(64 * 1024, 'a')));
    Entries();
    std::println("VLESS/Trojan URI: {} positive cases, {} rejection cases, mixed provider/preparation boundaries passed", accepted, rejected);
}
} // namespace node_uri_v2ray_tests
