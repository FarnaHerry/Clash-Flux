std::string geoTestVarint(std::uint64_t n) {
    std::string out;
    do { out += static_cast<char>((n & 127) | (n > 127 ? 128 : 0)); n >>= 7; } while (n);
    return out;
}
std::string geoTestField(unsigned id, std::string_view bytes) {
    return geoTestVarint((id << 3) | 2) + geoTestVarint(bytes.size()) + std::string(bytes);
}
std::string geoTestNumber(unsigned id, std::uint64_t n) { return geoTestVarint(id << 3) + geoTestVarint(n); }

void testGeoData(const std::filesystem::path& fixtures = {}) {
    const auto domain = [](unsigned type, std::string_view name) {
        return geoTestField(2, geoTestNumber(1, type) + geoTestField(2, name));
    };
    const auto attributed = geoTestField(2, geoTestNumber(1, 3) + geoTestField(2, "both.test") +
        geoTestField(3, geoTestField(1, "CN") + geoTestNumber(2, 0)) +
        geoTestField(3, geoTestField(1, "ADS") + geoTestNumber(3, 7)));
    const auto site = geoTestField(1, geoTestField(1, "TEST") + domain(0, "tracking") +
        domain(1, "^regex[0-9]+\\.test$") + domain(2, "ads.test") + domain(3, "full.test") + attributed);
    const auto cidr = [](std::string_view bytes, unsigned prefix) {
        return geoTestField(2, geoTestField(1, bytes) + geoTestNumber(2, prefix));
    };
    const auto ip = geoTestField(1, geoTestField(1, "CN") +
        cidr(std::string("\x7f\x00\x00\x02", 4), 32) +
        cidr(std::string("\x20\x01\x0d\xb8", 4) + std::string(12, '\0'), 32));
    const std::string source = "geodata-mode: true\ngeo-auto-update: true\ngeo-update-interval: 48\n"
        "geox-url: {geosite: 'https://geo.test/site.dat', geoip: 'https://geo.test/ip.dat'}\n"
        "rules: ['GEOSITE,test,REJECT', 'GEOIP,CN,REJECT,no-resolve', 'MATCH,DIRECT']\n";
    std::string error;
    const auto declarations = singbox::InspectExternalResources(source, error, "profile-geo");
    check(error.empty() && declarations.size() == 2 && !declarations[0].cacheKey.empty() &&
          declarations[0].intervalSeconds == 48 * 3600, "Geo declarations expose full identity and interval");
    singbox::CompileOptions options;
    options.mainConnectionId = "profile-geo"; options.profileYaml = source;
    const auto discovered = singbox::compileConfig(options);
    check(discovered.httpRuleProviders.size() == 2 && !discovered.error.empty(), "Geo missing snapshots carry both download requirements");
    if (discovered.httpRuleProviders.size() != 2) return;
    const auto resource = discovered.httpRuleProviders.front();
    check(singbox::ValidateHttpRuleProvider(resource, site, error), "valid GeoSite protobuf");
    for (const auto& bad : {site.substr(0, site.size() - 1), site + site,
            geoTestField(1, geoTestField(1, "TEST") + domain(4, "unknown.test")),
            geoTestField(1, geoTestField(1, "TEST") + domain(2, "good.test") + geoTestNumber(99, 1)),
            std::string("\x0a\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\x01", 12)})
        check(!singbox::ValidateHttpRuleProvider(resource, bad, error), "Geo truncated, duplicate, future or malformed data is rejected");
    auto ipResource = discovered.httpRuleProviders.back();
    check(singbox::ValidateHttpRuleProvider(ipResource, ip, error), "IPv4 and IPv6 GeoIP protobuf");
    check(!singbox::ValidateHttpRuleProvider(ipResource,
        geoTestField(1, geoTestField(1, "CN") + cidr(std::string(4, '\0'), 33)), error), "GeoIP invalid prefix rejected");
    rule_provider_cache::StagingDirectory staging;
    staging.path = std::filesystem::temp_directory_path() /
        ("clashflux-geo-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(staging.path);
    options.ruleProviderCacheDir = staging.path.string();
    const auto missingCache = singbox::InspectExternalResourceCache(declarations[0], staging.path);
    check(missingCache.supported && !missingCache.present && !missingCache.valid, "Geo status reports missing cache without fetching");
    rule_provider_cache::Operations operations;
    unsigned downloads = 0;
    operations.fetch = [&](const auto& r, const auto& file, std::string&) {
        ++downloads;
        std::ofstream out(file, std::ios::binary); out << (r.behavior == "geosite" ? site : ip); return bool(out);
    };
    operations.check = [](const auto&, const auto& result, std::string&) { return result.error.empty() && !result.json.empty(); };
    operations.commit = [](const auto& from, const auto& to, std::string& reason) {
        std::error_code ec; std::filesystem::rename(from, to, ec); if (ec) reason = ec.message(); return !ec;
    };
    singbox::CompileResult compiled;
    check(rule_provider_cache::Prepare(options, operations, compiled, error) && downloads == 2,
          "Geo uses staged production preparation, one transfer per database");
    if (compiled.json.empty()) return;
    const auto cached = singbox::InspectExternalResourceCache(declarations[0], staging.path);
    check(cached.valid && cached.present && cached.bytes == site.size(), "Geo status reports validated raw bytes");
    const auto config = json::parse(compiled.json);
    const auto sets = config["route"]["rule_set"];
    check(sets.size() == 2 && sets[0]["type"] == "inline" && sets[1]["type"] == "inline", "Geo lowers to native inline rule sets");
    check(sets[0]["rules"][0]["domain_suffix"][0] == "ads.test" &&
          sets[0]["rules"][0]["domain"][0] == "full.test" &&
          sets[0]["rules"][0]["domain_keyword"][0] == "tracking" &&
          sets[1]["rules"][0]["ip_cidr"].size() == 2, "Geo domain types and dual-stack CIDRs preserved");
    auto inverseOptions = options;
    inverseOptions.ruleProviderContents[ipResource.cacheKey] = geoTestField(1,
        geoTestField(1, "CN") + cidr(std::string("\x7f\x00\x00\x01", 4), 32) + geoTestNumber(3, 1));
    const auto inverted = singbox::compileConfig(inverseOptions);
    check(inverted.error.empty() && json::parse(inverted.json)["route"]["rule_set"][1]["rules"][0].value("invert", false),
          "GeoIP inverse_match preserved by native headless invert");
    if (!fixtures.empty()) {
        for (const auto* kind : {"site", "ip"}) {
            auto selected = config;
            auto& rules = selected["route"]["rules"];
            std::erase_if(rules.get_ref<json::array_t&>(), [&](const auto& rule) {
                return rule.contains("rule_set") && rule["rule_set"][0] != sets[std::string(kind) == "site" ? 0 : 1]["tag"];
            });
            std::ofstream(fixtures / (std::string("geo-") + kind + ".json")) << selected.dump(2);
            if (std::string(kind) == "ip" && !inverted.json.empty()) {
                selected["route"]["rule_set"] = json::parse(inverted.json)["route"]["rule_set"];
                std::ofstream(fixtures / "geo-ip-inverse.json") << selected.dump(2);
            }
        }
    }
    auto altered = options;
    altered.mainConnectionId = "other-profile";
    altered.ruleProviderContents.clear();
    check(!singbox::compileConfig(altered).error.empty(), "Geo caches are isolated by stable source ID");
    altered = options; altered.profileYaml = source + "# edit\n";
    altered.profileYaml.replace(altered.profileYaml.find("GEOSITE,test"), 12, "GEOSITE,missing");
    check(!singbox::compileConfig(altered).error.empty(), "missing custom category never falls back to built-in data");
    altered = options;
    altered.profileYaml.replace(altered.profileYaml.find("GEOSITE,test"), 12, "GEOSITE,test@cn");
    auto filtered = singbox::compileConfig(altered);
    check(filtered.error.empty() && json::parse(filtered.json)["route"]["rule_set"][0]["rules"][0]["domain"] == json::array({"both.test"}),
          "Geo attribute selection uses presence, including false boolean values");
    altered.profileYaml.replace(altered.profileYaml.find("test@cn"), 7, "test@cn@ads");
    filtered = singbox::compileConfig(altered);
    check(filtered.error.empty() && json::parse(filtered.json)["route"]["rule_set"][0]["rules"][0]["domain"] == json::array({"both.test"}),
          "Geo multiple attributes use AND and ignore attribute value type");
    if (!fixtures.empty()) std::ofstream(fixtures / "geo-filter.json") << filtered.json;
    altered.profileYaml.replace(altered.profileYaml.find("test@cn@ads"), 11, "test@absent");
    filtered = singbox::compileConfig(altered);
    check(filtered.error.empty() && json::parse(filtered.json)["route"]["rule_set"][0]["rules"][0]["mode"] == "and" &&
          json::parse(filtered.json)["route"]["rule_set"][0]["rules"][0]["rules"].size() == 2,
          "empty Geo attribute result uses a native contradiction, never a match-all rule");
    if (!fixtures.empty()) std::ofstream(fixtures / "geo-empty.json") << filtered.json;
    altered = options;
    altered.profileYaml.replace(altered.profileYaml.find("GEOSITE,test"), 12, "GEOSITE,test@@cn");
    check(!singbox::compileConfig(altered).error.empty(), "empty Geo selector rejected");
    auto adapter = singbox::CompileOptions{};
    adapter.tunInbound = true; adapter.tunMtu = 1500; adapter.tunStrictRoute = false;
    auto adapterResult = singbox::compileConfig(adapter);
    check(adapterResult.error.empty() && json::parse(adapterResult.json)["inbounds"].back()["mtu"] == 1500 &&
          json::parse(adapterResult.json)["inbounds"].back()["strict_route"] == false, "adapter settings reach generated TUN");
    adapter.profileYaml = R"({"inbounds":[{"type":"tun","tag":"native-tun","address":["172.19.0.1/30"],"mtu":1600}],"outbounds":[{"type":"direct","tag":"DIRECT"}]})";
    adapterResult = singbox::compileConfig(adapter);
    check(adapterResult.error.empty() && json::parse(adapterResult.json)["inbounds"][0]["mtu"] == 1600, "native MTU preserved before explicit override");
    adapter.overrideTunMtu = true;
    adapterResult = singbox::compileConfig(adapter);
    check(adapterResult.error.empty() && json::parse(adapterResult.json)["inbounds"][0]["mtu"] == 1500, "explicit MTU override applies to native TUN");
    adapter.tunMtu = 100;
    check(!singbox::compileConfig(adapter).error.empty(), "invalid MTU rejected in compiler boundary");
    auto mmdb = singbox::CompileOptions{};
    mmdb.profileYaml = "geox-url: {mmdb: 'https://geo.test/country.mmdb'}\nrules: ['GEOIP,CN,REJECT', 'MATCH,DIRECT']";
    check(!singbox::compileConfig(mmdb).error.empty(), "custom MMDB cannot silently select built-in data");
    mmdb.profileYaml = "geox-url: {asn: 'https://geo.test/asn.mmdb'}\nrules: ['IP-ASN,123,REJECT', 'MATCH,DIRECT']";
    check(!singbox::compileConfig(mmdb).error.empty(), "custom ASN MMDB cannot silently select ipverse");
    const auto noAuto = singbox::InspectExternalResources("geox-url: {geosite: 'https://geo.test/site.dat'}", error);
    check(noAuto.size() == 1 && noAuto[0].intervalSeconds == 0, "Geo auto update defaults to disabled");
}
