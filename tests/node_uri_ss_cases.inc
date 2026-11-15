namespace node_uri_ss_tests {

std::string Percent(std::string_view text) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char c : text) { result += '%'; result += digits[c >> 4]; result += digits[c & 15]; }
    return result;
}

std::string Uri(std::string_view password = "exact password", std::string_view method = "aes-256-gcm",
        std::string_view host = "example.test:443", bool encoded = false, std::string_view name = "SS fixture") {
    const auto userinfo = encoded ? encodeNodeFixture(std::string(method) + ":" + std::string(password), true) :
        Percent(method) + ":" + Percent(password);
    return "ss://" + userinfo + "@" + std::string(host) + "#" + Percent(name);
}

json Expected(std::string password = "exact password", std::string method = "aes-256-gcm", std::string host = "example.test", int port = 443) {
    return {{"type", "shadowsocks"}, {"method", std::move(method)}, {"password", std::move(password)},
        {"server", std::move(host)}, {"server_port", port}};
}

singbox::CompileResult Compile(const std::string& body) {
    singbox::CompileOptions options; options.profileYaml = body; options.mainConnectionId = "p6-main";
    return singbox::compileConfig(options);
}

unsigned accepted = 0, rejected = 0;

void Accept(const std::string& body, const json& expected, const std::filesystem::path& fixtures = {},
        const std::string& name = "SS fixture") {
    ++accepted;
    const auto result = Compile(body);
    const auto node = protocol_entry_tests::Node(result, "p6-main", name);
    check(result.error.empty() && !node.empty(), "SIP002 positive fixture compiles: " + std::to_string(accepted));
    if (!node.empty()) check(protocol_entry_tests::SemanticNode(node) == expected,
        "SIP002 preserves independently expected credentials, endpoint and plugin defaults: " + std::to_string(accepted));
    check(protocol_entry_tests::NodeNotes(result, "p6-main").empty(), "standard SIP002 mappings need no node approximation");
    std::string error;
    check(singbox::DetectProfileFormat(body) == singbox::ProfileFormat::NodeSubscription &&
        singbox::ValidateProfileDocument(body, error), "SIP002 is recognized through the ordinary profile document boundary");
    const auto projection = singbox::InspectProfileRules(body, {});
    check(projection.error.empty() && projection.original.size() == 1, "SIP002 shares the generated selection policy projection");
    singbox::CompileOptions options; options.profileYaml = body; options.mainConnectionId = "p6-main";
    const auto inventory = singbox::inspectProfileSourceObjects(options);
    check(inventory.error.empty() && inventory.json.empty() && std::ranges::any_of(inventory.sourceObjects, [&](const auto& object) {
        return object.sourceId == "p6-main" && object.objectId == name && object.kind == vpn::TargetKind::Node;
    }), "SIP002 inventory retains source identity without creating a runnable plan");
    if (!fixtures.empty() && !result.json.empty()) {
        std::filesystem::create_directories(fixtures);
        std::ofstream(fixtures / ("ss-uri-" + std::to_string(accepted) + ".json")) << result.json;
    }
}

void Reject(const std::string& body) {
    ++rejected;
    const auto result = Compile(body);
    check(!result.error.empty() && result.json.empty(), "SIP002 invalid input rejects the complete candidate: " + std::to_string(rejected));
    std::string error;
    check(!singbox::ValidateProfileDocument(body, error), "SIP002 format rejection is consistent at document validation");
    const auto safe = [](const std::string& message) {
        return message.find("PRIVATE") == std::string::npos && message.find("ss://") == std::string::npos &&
            message.find("vmess://") == std::string::npos && message.find("11111111") == std::string::npos;
    };
    check(safe(result.error) && safe(error) && std::ranges::all_of(result.fidelity, [&](const auto& note) {
        return safe(note.detail) && safe(note.action);
    }), "URI diagnostics contain line/field reasons without credentials or raw links");
}

void ProviderAndPreparation() {
    const RuleSetTestDirectory directory;
    const auto ss = Uri(" PRIVATE +%40: secret ");
    const json vmess{{"ps", "VM fixture"}, {"add", "vm.example.test"}, {"port", "443"},
        {"id", "11111111-2222-3333-4444-555555555555"}, {"net", "tcp"}};
    const auto mixed = ss + "\nvmess://" + encodeNodeFixture(vmess.dump());
    for (const auto* kind : {"file", "http"}) {
        const auto [options, result] = protocol_entry_tests::CompileProvider(encodeNodeFixture(mixed), kind, directory.path);
        const auto ssNode = protocol_entry_tests::Node(result, "p5-main", "SS fixture");
        check(result.error.empty() && !ssNode.empty() && ssNode["password"] == " PRIVATE +%40: secret " &&
            !protocol_entry_tests::Node(result, "p5-main", "VM fixture").empty(), "mixed SS/VMess provider keeps both protocol models");
        const auto inventory = singbox::inspectProfileSourceObjects(options);
        check(inventory.error.empty() && inventory.json.empty() && inventory.sourceObjects.size() == result.sourceObjects.size(),
            "mixed URI provider has an equivalent inactive inventory");
        auto filteredOptions = options;
        const auto at = filteredOptions.profileYaml.find("    type:");
        filteredOptions.profileYaml.insert(at, "    exclude-type: ss\n");
        if (std::string_view(kind) == "http") {
            filteredOptions.proxyProviderContents.clear();
            const auto discovery = singbox::compileConfig(filteredOptions);
            check(discovery.httpProxyProviders.size() == 1, "SS type filter participates in provider identity");
            for (const auto& resource : discovery.httpProxyProviders)
                filteredOptions.proxyProviderContents[resource.cacheKey] = mixed;
        }
        const auto filtered = singbox::compileConfig(filteredOptions);
        check(filtered.error.empty() && protocol_entry_tests::Node(filtered, "p5-main", "SS fixture").empty() &&
            !protocol_entry_tests::Node(filtered, "p5-main", "VM fixture").empty(), "URI exclude-type uses ss for SS instead of labeling every URI vmess");
        const auto [badOptions, failed] = protocol_entry_tests::CompileProvider(mixed + "\nss://PRIVATE", kind, directory.path, false, "^VM fixture$");
        check(!failed.error.empty() && failed.json.empty(), "malformed excluded SS line still rejects a URI provider before filtering");
    }
#if !defined(__ANDROID__) && !defined(CLASHFLUX_IOS)
    singbox::CompileOptions sources;
    sources.mainConnectionId = "p6-main"; sources.profileYaml = ss;
    sources.auxiliarySources = {{"p6-aux", "SS secondary", mixed, true}};
    sources.globalRules = {{.match = vpn::MatchKind::DomainSuffix, .pattern = "secondary.test",
        .connectionId = "p6-aux", .id = "p6-ss", .tier = vpn::RuleTier::SourcePolicy,
        .targetKind = vpn::TargetKind::Node, .targetObject = "SS fixture"}};
    const auto composed = singbox::compileConfig(sources);
    const auto main = protocol_entry_tests::Node(composed, "p6-main", "SS fixture"), aux = protocol_entry_tests::Node(composed, "p6-aux", "SS fixture");
    check(composed.error.empty() && !main.empty() && !aux.empty() && main["tag"] != aux["tag"] && main["password"] == aux["password"],
        "SS URI secondary export preserves credentials and separates same-name source identities");
#endif
    const auto cache = directory.path / "cache";
    std::filesystem::create_directory(cache);
    singbox::CompileOptions prepare;
    prepare.mainConnectionId = "p6-prepare"; prepare.proxyProviderCacheDir = cache.string();
    prepare.profileYaml = protocol_entry_tests::ProviderDocument(mixed, "http");
    auto response = mixed;
    bool approve = true;
    int commits = 0, checks = 0;
    rule_provider_cache::Operations operations;
    operations.fetchProxy = [&](const auto&, const auto& path, std::string&) { std::ofstream(path, std::ios::binary) << response; return true; };
    operations.check = [&](const auto&, const auto& result, std::string& error) {
        ++checks;
        if (!approve) error = "deliberate candidate rejection";
        return approve && result.error.empty() && !result.json.empty();
    };
    operations.commit = [&](const auto& source, const auto& target, std::string& error) {
        ++commits; std::error_code ec; std::filesystem::rename(source, target, ec); error = ec.message(); return !ec;
    };
    singbox::CompileResult good;
    std::string error;
    check(rule_provider_cache::Prepare(prepare, operations, good, error) && commits == 1 && checks == 1,
        "production preparation accepts a complete mixed URI collection before committing its cache");
    if (good.httpProxyProviders.empty()) { check(false, "prepared URI provider identity is retained"); return; }
    const auto resource = good.httpProxyProviders.front();
    const auto cached = singbox::ReadHttpProxyProviderCache(resource, cache, error);
    check(cached == mixed, "prepared SS/VMess cache preserves original response bytes");
    prepare.forceProviderRefreshSources = {"p6-prepare"};
    for (int failure = 0; failure < 2; ++failure) {
        response = failure == 0 ? mixed + "\nss://PRIVATE" : mixed;
        approve = failure == 0;
        singbox::CompileResult rejectedResult;
        check(!rule_provider_cache::Prepare(prepare, operations, rejectedResult, error) && commits == 1 &&
            singbox::ReadHttpProxyProviderCache(resource, cache, error) == cached,
            "URI format/candidate failure leaves the previously approved raw provider cache intact");
    }
}

int WriteProvider(const std::filesystem::path& input, const std::filesystem::path& output) {
    std::ifstream file(input, std::ios::binary);
    const std::string body{std::istreambuf_iterator<char>(file), {}};
    const auto [options, result] = protocol_entry_tests::CompileProvider(body, "http", input.parent_path());
    if (!file || !result.error.empty() || result.json.empty() || failures) return 1;
    std::ofstream stream(output); stream << result.json; stream.close(); return stream ? 0 : 1;
}

void Run(const std::filesystem::path& fixtures = {}) {
    accepted = rejected = 0;
    for (const auto* password : {"exact password", " PRIVATE +%40:;=\\?/#@ ", "%2F", "+", "😀中文", "  "}) {
        for (const bool base64 : {false, true}) Accept(Uri(password, "aes-256-gcm", "example.test:443", base64), Expected(password), fixtures);
    }
    Accept("SS://aes-256-gcm:p+%2B%252F%40%3A@example.test:443#SS%20fixture", Expected("p++%2F@:"), fixtures);
    Accept(Uri("key", "aes-128-gcm", "[2001:db8::1]:8443"), Expected("key", "aes-128-gcm", "2001:db8::1", 8443), fixtures);
    Accept(Uri("key", "aes-128-gcm", "[::ffff:192.0.2.1]:443"), Expected("key", "aes-128-gcm", "::ffff:192.0.2.1"), fixtures);
    Accept("ss://aes-256-gcm:key@%65xample.test:443/#%20SS%20fixture%20", Expected("key"), fixtures);
    for (const auto* method : {"none", "aes-128-gcm", "aes-192-gcm", "aes-256-gcm", "chacha20-ietf-poly1305", "xchacha20-ietf-poly1305",
            "aes-128-ctr", "aes-192-ctr", "aes-256-ctr", "aes-128-cfb", "aes-192-cfb", "aes-256-cfb", "rc4-md5", "chacha20-ietf", "xchacha20"})
        Accept(Uri("key", method), Expected("key", method), fixtures);
    for (const auto* method : {"2022-blake3-aes-128-gcm", "2022-blake3-aes-256-gcm", "2022-blake3-chacha20-poly1305"}) {
        const auto key = encodeNodeFixture(std::string(std::string_view(method).find("128") != std::string_view::npos ? 16 : 32, 'a'));
        Accept(Uri(key, method), Expected(key, method), fixtures);
        Reject(Uri(key, method, "example.test:443", true));
    }
    const auto plusKey = encodeNodeFixture(std::string(32, static_cast<char>(0xfb)));
    Accept(Uri(plusKey, "2022-blake3-aes-256-gcm"), Expected(plusKey, "2022-blake3-aes-256-gcm"), fixtures);
    for (const auto& [plugin, canonical] : std::vector<std::pair<std::string, std::string>>{
            {"obfs-local", "obfs=http"}, {"obfs-local;obfs=tls", "obfs=tls"},
            {"obfs-local;obfs=http;obfs-host=edge.test", "obfs=http;obfs-host=edge.test"},
            {"v2ray-plugin", "mode=websocket;mux=1"}, {"v2ray-plugin;tls", "mode=websocket;mux=1;tls"},
            {"v2ray-plugin;path=;mux=0", "mode=websocket;path=;mux=0"},
            {"v2ray-plugin;host=edge.test;path=/a?b=1&c=2;tls;mux=0", "mode=websocket;host=edge.test;path=/a?b\\=1&c\\=2;mux=0;tls"},
            {"v2ray-plugin;host=edge.test;path=/a\\;b\\=c\\\\d\\:e%2F+", "mode=websocket;host=edge.test;path=/a\\;b\\=c\\\\d:e%2F+;mux=1"}}) {
        auto expected = Expected("key"); expected["plugin"] = plugin.starts_with("obfs") ? "obfs-local" : "v2ray-plugin";
        expected["plugin_opts"] = canonical;
        Accept("ss://aes-256-gcm:key@example.test:443/?plugin=" + Percent(plugin) + "#SS%20fixture", expected, fixtures);
    }
    const auto raw = Uri("key");
    for (const auto& body : {raw, encodeNodeFixture(raw), encodeNodeFixture(raw, true), "\xEF\xBB\xBF # header\r\n  " + raw + "\r\n"})
        Accept(body, Expected("key"), fixtures);
    Accept("ss://aes-256-gcm:key@example.test:443", Expected("key"), fixtures, "Shadowsocks 1");
    Accept(Uri("key", "aes-256-gcm", "example.test:443", false, "节点选择"), Expected("key"), fixtures, "节点选择");
    for (const auto* component : {"%", "%0", "%GG", "%00", "%0A", "%7f", "%C0%AF", "%ED%A0%80", "%F4%90%80%80"})
        Reject("ss://aes-256-gcm:" + std::string(component) + "@example.test:443#SS");
    for (const auto* bad : {"ss://", "ss://PRIVATE", "ss://aes-256-gcm:@example.test:443", "ss://:PRIVATE@example.test:443",
            "ss://aes-256-gcm:PRIVATE@example.test", "ss://aes-256-gcm:PRIVATE@example.test:0", "ss://aes-256-gcm:PRIVATE@example.test:65536",
            "ss://aes-256-gcm:PRIVATE@example.test:+443", "ss://aes-256-gcm:PRIVATE@[:::1]:443", "ss://aes-256-gcm:PRIVATE@::1:443",
            "ss://aes-256-gcm:PRIVATE@[::1%25lo]:443", "ss://aes-256-gcm:PRIVATE@host%2Ftail:443",
            "ss://aes-256-gcm:PRIVATE@host%252Ftail:443", "ss://aes-256-gcm:PRIVATE@@example.test:443",
            "ss://aes-256-gcm:PRIVATE@example.test:443/path", "ss://aes-256-gcm:PRIVATE@example.test:443#first#second",
            "ss://aes-256-gcm:PRIVATE@example.test:443#raw space", "ss://unknown:PRIVATE@example.test:443", "ss://Zh@example.test:443",
            "ss://aes-256-gcm:PRIVATE@example.test:443?token=PRIVATE", "ss://aes-256-gcm:PRIVATE@example.test:443?plugin",
            "ss://aes-256-gcm:PRIVATE@example.test:443?plugin=obfs-local&", "ss://aes-256-gcm:PRIVATE@example.test:443?&plugin=obfs-local",
            "ss://aes-256-gcm:PRIVATE@example.test:443#DIRECT"}) Reject(bad);
    for (const auto* plugin : {"", "PRIVATE", "obfs-local;obfs=quic", "obfs-local;obfs-host=", "obfs-local;obfs-host=x;obfs-host=y",
            "obfs-local;;obfs=http", "obfs-local;obfs-host=PRIVATE\\", "obfs-local;obfs-host=PRIVATE\\q", "v2ray-plugin;tls=false",
            "v2ray-plugin;mode=quic", "v2ray-plugin;host=", "v2ray-plugin;mux=2", "v2ray-plugin;cert=PRIVATE", "v2ray-plugin;server"})
        Reject("ss://aes-256-gcm:PRIVATE@example.test:443?plugin=" + Percent(plugin));
    Reject("ss://aes-256-gcm:PRIVATE@example.test:443?plugin=obfs-local&%70lugin=obfs-local");
    Reject("ss://" + encodeNodeFixture("aes-256-gcm:🙂") + "@example.test:443"); // Standard '+' is not Base64URL.
    Reject("ss://" + encodeNodeFixture("aes-256-gcm:PRIVATE@example.test:443") + "?plugin=obfs-local"); // Historical wrapping does not carry SIP002 query.
    Reject(raw + "\n" + raw);
    Reject(raw + "\nvmess://" + encodeNodeFixture(R"({"ps":"SS fixture","add":"example.test","port":443,"id":"11111111-2222-3333-4444-555555555555"})"));
    Reject(raw + "\nvless://PRIVATE");
    Reject("ss://aes-256-gcm:PRIVATE@example.test:443#" + std::string(64 * 1024, 'a'));
    std::string many;
    for (int i = 0; i < 4096; ++i) many += Uri("key", "aes-256-gcm", "example.test:443", true, "SS " + std::to_string(i)) + "\n";
    check(Compile(many).error.empty(), "SIP002 accepts exactly 4096 unique nodes");
    Reject(many + Uri("PRIVATE", "aes-256-gcm", "example.test:443", false, "excess"));
    ProviderAndPreparation();
    std::println("SIP002: {} positive cases, {} rejection cases, mixed provider/preparation boundaries passed", accepted, rejected);
}

} // namespace node_uri_ss_tests
