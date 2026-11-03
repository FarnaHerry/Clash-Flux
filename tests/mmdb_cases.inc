std::string mmdbTestValue(unsigned type, std::string body, unsigned count = 0) {
    const auto size = (type == 7 || type == 11) ? count : body.size();
    std::string result(1, static_cast<char>((type < 8 ? type : 0) << 5 | (size < 29 ? size : 29)));
    if (type >= 8) result += static_cast<char>(type - 7);
    if (size >= 29) result += static_cast<char>(size - 29);
    return result + body;
}
std::string mmdbTestString(std::string value) { return mmdbTestValue(2, std::move(value)); }
std::string mmdbTestInteger(unsigned type, std::uint32_t number) {
    std::string bytes;
    do { bytes.insert(bytes.begin(), static_cast<char>(number & 255)); number >>= 8; } while (number);
    return mmdbTestValue(type, bytes);
}
std::string mmdbTestMap(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string body;
    for (const auto& [key, value] : fields) body += mmdbTestString(key) + value;
    return mmdbTestValue(7, body, fields.size());
}
// Exact loopback IPv4 and a documentation IPv6 prefix. The same record pointer
// is deliberately shared, exercising memoization without changing set semantics.
std::string mmdbTestFile(unsigned width, bool ipv6, const std::string& type, const std::string& record) {
    struct Node { std::array<int, 2> child{-1, -1}; };
    std::vector<Node> nodes(1);
    const auto insert = [&](std::array<unsigned char, 16> ip, unsigned prefix) {
        unsigned node = 0;
        for (unsigned bit = 0; bit < prefix; ++bit) {
            const auto side = (ip[bit / 8] >> (7 - bit % 8)) & 1;
            if (bit + 1 == prefix) nodes[node].child[side] = -2;
            else {
                if (nodes[node].child[side] == -1) {
                    const auto next = nodes.size();
                    nodes[node].child[side] = next; nodes.emplace_back();
                }
                node = nodes[node].child[side];
            }
        }
    };
    if (ipv6) {
        insert({0,0,0,0,0,0,0,0,0,0,0,0,127,0,0,2}, 128);
        insert({0x20,1,0x0d,0xb8}, 32);
    } else insert({127,0,0,2}, 32);
    const auto n = static_cast<std::uint32_t>(nodes.size());
    std::string out;
    const auto big = [](std::uint32_t value, unsigned bytes) {
        std::string data(bytes, '\0');
        for (unsigned i = 0; i < bytes; ++i) data[bytes - i - 1] = static_cast<char>(value >> (i * 8));
        return data;
    };
    for (const auto& node : nodes) {
        std::array<std::uint32_t, 2> value;
        for (unsigned i = 0; i < 2; ++i) value[i] = node.child[i] == -1 ? n : node.child[i] == -2 ? n + 16 : node.child[i];
        if (width == 28) out += big(value[0], 3) + std::string(1, static_cast<char>(((value[0] >> 24) << 4) | (value[1] >> 24))) + big(value[1], 3);
        else out += big(value[0], width / 8) + big(value[1], width / 8);
    }
    out += std::string(16, '\0') + record + "\xab\xcd\xefMaxMind.com";
    out += mmdbTestMap({{"node_count", mmdbTestInteger(6, n)}, {"record_size", mmdbTestInteger(5, width)},
        {"ip_version", mmdbTestInteger(5, ipv6 ? 6 : 4)}, {"database_type", mmdbTestString(type)},
        {"binary_format_major_version", mmdbTestInteger(5, 2)}, {"binary_format_minor_version", mmdbTestInteger(5, 0)},
        {"build_epoch", mmdbTestInteger(9, 1)}});
    return out;
}

void testMmdb(const std::filesystem::path& fixtures = {}) {
    singbox::CompileOptions options;
    options.mainConnectionId = "mmdb-source";
    options.profileYaml = "geox-url: {mmdb: 'https://geo.test/country.mmdb'}\nrules: ['GEOIP,CN,REJECT,no-resolve','MATCH,DIRECT']\n";
    auto result = singbox::compileConfig(options);
    check(result.httpRuleProviders.size() == 1 && !result.error.empty(), "MMDB discovered without fallback to built-in Geo");
    if (result.httpRuleProviders.size() != 1) return;
    const auto resource = result.httpRuleProviders.front();
    std::string error;
    auto standard = mmdbTestMap({{"country", mmdbTestMap({{"iso_code", mmdbTestString("CN")}})}});
    const auto meta = mmdbTestValue(11, mmdbTestString("cn") + mmdbTestString("test"), 2);
    for (const auto width : {24, 28, 32}) {
        for (const auto& [type, record] : std::vector<std::pair<std::string, std::string>>{
                {"GeoLite2-Country", standard}, {"sing-geoip", mmdbTestString("cn")}, {"Meta-geoip0", meta}}) {
            const auto file = mmdbTestFile(width, true, type, record);
            check(singbox::ValidateHttpRuleProvider(resource, file, error), "MMDB three country schemas and record widths validate");
            options.ruleProviderContents[resource.cacheKey] = file;
            result = singbox::compileConfig(options);
            check(result.error.empty(), "MMDB full candidate lowers IPv4 and IPv6");
            if (!result.json.empty()) {
                const auto config = json::parse(result.json);
                const auto prefixes = config["route"]["rule_set"][0]["rules"][0]["ip_cidr"];
                check(std::ranges::find(prefixes, "127.0.0.2/32") != prefixes.end() && prefixes.size() == 3,
                    "MMDB IPv4-in-IPv6 tree emits explicit IPv4 prefix plus IPv6 prefixes");
                if (!fixtures.empty()) std::ofstream(fixtures / "geo-mmdb.json") << result.json;
            }
        }
    }
    const auto v4 = mmdbTestFile(24, false, "GeoLite2-Country", standard);
    check(singbox::ValidateHttpRuleProvider(resource, v4, error), "IPv4-only MMDB supported");
    for (auto bad : {v4.substr(0, v4.size() - 1), std::string("HTML error"),
            mmdbTestFile(24, false, "sing-geoip", mmdbTestString("bad@code")),
            mmdbTestFile(24, false, "Meta-geoip0", mmdbTestValue(11, mmdbTestInteger(6, 1), 1)),
            mmdbTestFile(24, false, "sing-geoip", std::string("\x20\x00", 2)),
            mmdbTestFile(24, false, "sing-geoip", mmdbTestString(std::string("\xff", 1))),
            mmdbTestFile(24, false, "GeoLite2-Country", mmdbTestMap({{"country", mmdbTestInteger(6, 1)}}))})
        check(!singbox::ValidateHttpRuleProvider(resource, bad, error), "MMDB truncated, invalid schema and pointer cycles rejected");
    auto nested = mmdbTestString("cn");
    for (unsigned i = 0; i < 70; ++i) nested = mmdbTestValue(11, nested, 1);
    check(!singbox::ValidateHttpRuleProvider(resource, mmdbTestFile(24, false, "Meta-geoip0", nested), error),
          "MMDB nesting is bounded before expansion");
    auto invalidPointer = v4; invalidPointer[0] = invalidPointer[1] = invalidPointer[2] = static_cast<char>(255);
    check(!singbox::ValidateHttpRuleProvider(resource, invalidPointer, error), "MMDB out-of-range tree pointer rejected");
    auto separator = v4; separator[32 * 6] = 1;
    check(!singbox::ValidateHttpRuleProvider(resource, separator, error), "MMDB invalid separator rejected");
    auto overflowNodes = v4;
    const auto nodeField = mmdbTestString("node_count") + mmdbTestInteger(6, 32);
    overflowNodes.replace(overflowNodes.find(nodeField), nodeField.size(),
        mmdbTestString("node_count") + mmdbTestValue(9, std::string(8, static_cast<char>(255))));
    check(!singbox::ValidateHttpRuleProvider(resource, overflowNodes, error), "MMDB node-count overflow rejected before tree arithmetic");
    auto cycle = v4; cycle[0] = cycle[1] = cycle[2] = 0;
    check(!singbox::ValidateHttpRuleProvider(resource, cycle, error), "MMDB tree cycle rejected");
    options.profileYaml = "geox-url: {asn: 'https://geo.test/asn.mmdb'}\nrules: ['IP-ASN,123,REJECT,no-resolve','MATCH,DIRECT']\n";
    options.ruleProviderContents.clear();
    result = singbox::compileConfig(options);
    check(result.httpRuleProviders.size() == 1 && result.httpRuleProviders[0].format == "mmdb", "custom ASN uses MMDB instead of ipverse");
    if (result.httpRuleProviders.size() == 1) {
        const auto asn = result.httpRuleProviders[0];
        for (const auto& [type, record] : std::vector<std::pair<std::string, std::string>>{
                {"GeoLite2-ASN", mmdbTestMap({{"autonomous_system_number", mmdbTestInteger(6, 123)}})},
                {"DBIP-ASN-Lite (compat=GeoLite2-ASN)", mmdbTestMap({{"autonomous_system_number", mmdbTestInteger(6, 123)}})},
                {"ipinfo generic_asn_free.mmdb", mmdbTestMap({{"asn", mmdbTestString("AS123")}})}}) {
            options.ruleProviderContents[asn.cacheKey] = mmdbTestFile(32, false, type, record);
            const auto vendor = singbox::compileConfig(options);
            check(vendor.error.empty() && json::parse(vendor.json)["route"]["rules"].dump().find("127.0.0.2/32") != std::string::npos,
                  "GeoLite2, DBIP and ipinfo ASN record schemas compile without changing the source");
        }
        const auto raw = mmdbTestFile(28, true, "GeoLite2-ASN", mmdbTestMap({{"autonomous_system_number", mmdbTestInteger(6, 123)}}));
        options.ruleProviderContents[asn.cacheKey] = raw;
        result = singbox::compileConfig(options);
        check(result.error.empty() && !result.json.empty(), "ASN MMDB compiles to native CIDRs");
        if (!fixtures.empty() && !result.json.empty()) std::ofstream(fixtures / "geo-asn-mmdb.json") << result.json;
        auto sourceOptions = options;
        sourceOptions.profileYaml.replace(sourceOptions.profileYaml.find("IP-ASN"), 6, "SRC-IP-ASN");
        sourceOptions.profileYaml.replace(sourceOptions.profileYaml.find(",no-resolve"), 11, "");
        const auto sourceResult = singbox::compileConfig(sourceOptions);
        check(sourceResult.error.empty() && json::parse(sourceResult.json)["route"]["rules"].dump().find("source_ip_cidr") != std::string::npos,
              "source ASN MMDB maps to source CIDRs");
        options.profileYaml.replace(options.profileYaml.find("123"), 3, "456");
        check(!singbox::compileConfig(options).error.empty(), "missing custom ASN refuses fallback to another source");
        if (!fixtures.empty()) {
            std::ofstream(fixtures / "country.mmdb", std::ios::binary) << v4;
            std::ofstream(fixtures / "asn.mmdb", std::ios::binary) << raw;
        }
    }
    const auto declarations = singbox::InspectExternalResources(
        "geox-url: {geoip: 'https://geo.test/unused.dat', mmdb: 'https://geo.test/country.mmdb'}", error, "mmdb-source");
    check(declarations.size() == 2 && declarations[0].cacheKey.empty() && !declarations[1].cacheKey.empty(),
        "inactive DAT is not mistaken for the selected MMDB declaration");
}
