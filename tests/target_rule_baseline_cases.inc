// R0: freeze public observations before introducing a sing-box target model.
// Recording is deliberately external: this test never rewrites expectations.
namespace target_rule_baseline {

json Rows(const std::vector<singbox::ProfileRule>& rows) {
    auto result = json::array();
    for (const auto& row : rows)
        result.push_back({{"anchor", row.anchor}, {"raw", row.raw}, {"type", row.type},
            {"pattern", row.pattern}, {"target", row.target}, {"editId", row.editId},
            {"origin", row.origin}, {"position", row.position}, {"editable", row.editable},
            {"enabled", row.enabled}, {"terminal", row.terminal}, {"implicit", row.implicit}});
    return result;
}

singbox::CompileOptions Options(const json& input) {
    singbox::CompileOptions options;
    options.profileYaml = input.at("source").get<std::string>();
    options.mainConnectionId = "target-r0";
    options.mainSourceName = "Target R0";
    options.mode = input.value("mode", "rule");
    options.proxyListening = input.value("listening", true);
    options.tunInbound = input.value("tun", false);
    if (input.contains("edits")) for (const auto& value : input.at("edits")) {
        singbox::ProfileRuleEdit edit;
        edit.id = value.at("id").get<std::string>();
        edit.operation = value.value("operation", edit.operation);
        edit.anchor = value.value("anchor", edit.anchor);
        edit.enabled = value.value("enabled", edit.enabled);
        edit.match = value.value("match", edit.match);
        edit.pattern = value.value("pattern", edit.pattern);
        edit.target = value.value("target", edit.target);
        edit.unavailable = value.value("unavailable", edit.unavailable);
        options.profileRuleEdits.rules.push_back(std::move(edit));
    }
    if (input.contains("auxiliary"))
        options.auxiliarySources.push_back({"target-r0-aux", "Secondary", input.at("auxiliary").get<std::string>()});
    if (input.contains("global_target"))
        options.globalRules = {{.match = vpn::MatchKind::DomainSuffix, .pattern = "global.test",
            .connectionId = input.at("global_target").get<std::string>(), .id = "r0-global",
            .tier = input.value("global_tier", "user") == "source" ? vpn::RuleTier::SourcePolicy : vpn::RuleTier::UserOverride,
            .targetKind = vpn::TargetKind::Group, .targetObject = "Pick"}};
    return options;
}

json Observe(const json& input) {
    const auto options = Options(input);
    const auto projected = singbox::InspectProfileRules(options.profileYaml, options.profileRuleEdits);
    const auto result = singbox::compileConfig(options);
    auto objects = json::array(), origins = json::array();
    for (const auto& object : result.sourceObjects)
        objects.push_back({{"sourceId", object.sourceId}, {"sourceName", object.sourceName},
            {"kind", int(object.kind)}, {"objectId", object.objectId}, {"tag", object.tag}});
    for (const auto& origin : result.rulePlan.origins)
        origins.push_back({{"index", origin.index}, {"ruleId", origin.ruleId},
            {"sourceId", origin.sourceId}, {"sourceName", origin.sourceName}, {"tier", origin.tier}});
    // Keep the serialized JSON as well as structural fields: integer/float
    // representations must not silently change during a target-model migration.
    auto observation = protocolObservation(result);
    observation["json"] = result.json;
    observation["projection"] = {{"error", projected.error}, {"rules", Rows(projected.rules)},
        {"original", Rows(projected.original)}};
    observation["objects"] = std::move(objects);
    observation["rulePlan"] = {{"count", result.rulePlan.ruleCount}, {"origins", std::move(origins)},
        {"labels", result.rulePlan.outboundLabels}};
    observation["participatingSources"] = result.participatingSources;
    return observation;
}

json Read(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("target baseline fixture unavailable: " + path.string());
    return json::parse(input);
}

void Run() {
    const auto fixture = Read(std::filesystem::path(__FILE__).parent_path() / "fixtures" / "target-rule-baseline.json");
    check(fixture.at("schema") == 1, "R0 frozen baseline schema");
    check(fixture.at("cases").size() == 42, "R0 baseline includes all declared cases");
    const auto before = failures;
    for (const auto& sample : fixture.at("cases")) {
#if !defined(__linux__) || defined(__ANDROID__)
        if (sample.value("platform", "") == "linux") continue;
#endif
        const auto name = sample.at("name").get<std::string>();
        const auto observed = Observe(sample);
        check(observed.dump() == sample.at("expected").dump(), "R0 complete target observation: " + name);
        // Expectations below were authored independently of the frozen output.
        const auto& contract = sample.at("contract");
        const auto error = observed.at("error").get<std::string>();
        check(error.find(contract.value("error_contains", "")) != std::string::npos,
            "R0 explicit failure contract: " + name);
        if (!contract.contains("error_contains"))
            check(error.empty() && !observed.at("json").get<std::string>().empty(), "R0 successful target: " + name);
        else check(observed.at("json").get<std::string>().empty(), "R0 failed candidate has no target: " + name);
        const auto projectionError = observed.at("projection").at("error").get<std::string>();
        check(projectionError.find(contract.value("projection_error_contains", "")) != std::string::npos,
            "R0 legacy anchor contract: " + name);
        if (!contract.contains("projection_error_contains"))
            check(projectionError.empty(), "R0 projection remains available: " + name);
        if (!error.empty()) continue;
        const auto config = json::parse(observed.at("json").get<std::string>());
        const auto nativeRules = config.at("route").at("rules").dump();
        for (const auto& domain : contract.value("present", json::array()))
            check(nativeRules.find(domain.get<std::string>()) != std::string::npos, "R0 target retains declared match: " + name);
        for (const auto& domain : contract.value("absent", json::array()))
            check(nativeRules.find(domain.get<std::string>()) == std::string::npos, "R0 target omits disabled/unsupported/post-final match: " + name);
        if (contract.contains("reject_domain")) {
            const auto domain = contract.at("reject_domain").get<std::string>();
            check(std::ranges::any_of(config.at("route").at("rules"), [&](const auto& rule) {
                return rule.value("domain_suffix", json::array()) == json::array({domain}) && rule.value("action", "") == "reject";
            }), "R0 unavailable global target preserves match and rejects: " + name);
        }
        if (contract.contains("fidelity_min"))
            check(observed.at("fidelity").size() >= contract.at("fidelity_min").get<std::size_t>(), "R0 omission is accounted: " + name);
    }
    if (failures == before) std::println("Target R0: {} frozen cases and independent contracts passed (platform-scoped cases apply)", fixture.at("cases").size());
}

int ObserveFile(const std::filesystem::path& path) {
    auto outputs = json::array();
    const auto inputs = Read(path);
    for (const auto& sample : inputs.at("cases"))
        outputs.push_back({{"name", sample.at("name")}, {"observed", Observe(sample)}});
    std::println("{}", outputs.dump());
    return 0;
}

int Benchmark(int count, bool capture = false) {
    if (count < 1 || count > 100000) return 2;
    singbox::CompileOptions options;
    options.mainConnectionId = "target-r0-benchmark";
    options.captureTargetSource = capture;
    options.profileYaml = "rules:\n";
    for (int index = 0; index < count; ++index)
        options.profileYaml += std::format("  - DOMAIN-SUFFIX,rule{}.test,DIRECT\n", index);
    options.profileYaml += "  - MATCH,DIRECT\n";
    using Clock = std::chrono::steady_clock;
    const auto time = [](auto began) { return std::chrono::duration<double, std::milli>(Clock::now()-began).count(); };
    const auto start = Clock::now();
    const auto projected = singbox::InspectProfileRules(options.profileYaml, {}, false);
    const auto projectionMs = time(start);
    const auto compiledStart = Clock::now();
    const auto first = singbox::compileConfig(options);
    const auto firstMs = time(compiledStart);
    const auto repeatedStart = Clock::now();
    const auto repeated = singbox::compileConfig(options);
    const auto repeatedMs = time(repeatedStart);
    check(projected.error.empty() && first.error.empty() && repeated.error.empty(), "R0 measurement input compiles");
    check(projected.rules.size() == std::size_t(count + 1) && first.json == repeated.json, "R0 repeat is byte-identical");
    check(capture ? (first.targetSource && first.targetSource->rules().size() == std::size_t(count)) : !first.targetSource,
        "target measurement capture matches requested scope");
    std::println("{}", json{{"rows", count}, {"bytes", options.profileYaml.size()}, {"projection_ms", projectionMs},
        {"compile_ms", firstMs}, {"repeat_compile_ms", repeatedMs}, {"json_bytes", first.json.size()},
        {"first_source_cache_hits", first.sourceParseCacheHits}, {"repeat_source_cache_hits", repeated.sourceParseCacheHits},
        {"projection_calls", 1}, {"compile_calls", 2}}.dump());
    return failures ? 1 : 0;
}
} // namespace target_rule_baseline
