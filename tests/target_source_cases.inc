// R1: observations are independent from the R0 golden file, which stays frozen.
namespace target_source_tests {

singbox::CompileResult Compile(std::string source, bool capture = true) {
    singbox::CompileOptions options;
    options.profileYaml = std::move(source);
    options.mainConnectionId = "source-r1";
    options.mainSourceName = "R1 source";
    options.captureTargetSource = capture;
    return singbox::compileConfig(options);
}

void Invariants(const singbox::TargetSourceModel& model) {
    std::set<std::uint64_t> ids;
    for (const auto& rule : model.rules()) {
        check(rule.id && ids.insert(rule.id).second, "R1 IDs are unique within the model");
        check(rule.origin < model.origins().size(), "R1 rule references its own origin");
        if (rule.origin < model.origins().size()) {
            const auto& origin = model.origins()[rule.origin];
            check(origin.state == singbox::TargetRuleState::Emitted &&
                std::ranges::find(origin.rules, rule.id) != origin.rules.end(), "R1 origin owns the actual emitted branch");
        }
        check(model.ruleJson(rule.id).has_value(), "R1 native payload available by local identity");
    }
    for (const auto& origin : model.origins()) {
        for (const auto diagnostic : origin.diagnostics)
            check(diagnostic < model.diagnostics().size(), "R1 diagnostic indices belong to this immutable snapshot");
        if (origin.state != singbox::TargetRuleState::Emitted)
            check(origin.rules.empty(), "R1 default/omitted/post-final mapping has no phantom target");
    }
    check(!model.ruleJson(0) && !model.ruleJson(std::numeric_limits<std::uint64_t>::max()),
        "R1 invalid local IDs are rejected");
}

void Run() {
    const auto before = failures;
    const auto fixture = target_rule_baseline::Read(std::filesystem::path(__FILE__).parent_path() / "fixtures" / "target-rule-baseline.json");
    unsigned compared = 0, edited = 0;
    for (const auto& sample : fixture.at("cases")) {
#if !defined(__linux__) || defined(__ANDROID__)
        if (sample.value("platform", "") == "linux") continue;
#endif
        auto options = target_rule_baseline::Options(sample);
        const auto ordinary = singbox::compileConfig(options);
        check(!ordinary.targetSource, "R1 ordinary compile does not allocate/publish a target snapshot");
        options.captureTargetSource = true;
        const auto captured = singbox::compileConfig(options);
        if (!options.profileRuleEdits.rules.empty()) {
            ++edited;
            check(!captured.error.empty() && captured.json.empty() && !captured.targetSource,
                "R1 baseline capture cannot silently discard pre-existing personal edits");
            continue;
        }
        ++compared;
        check(captured.json == ordinary.json && protocolObservation(captured).dump() == protocolObservation(ordinary).dump() &&
            captured.sourceObjects == ordinary.sourceObjects && captured.participatingSources == ordinary.participatingSources &&
            captured.rulePlan.origins == ordinary.rulePlan.origins && captured.rulePlan.ruleCount == ordinary.rulePlan.ruleCount &&
            captured.rulePlan.outboundLabels == ordinary.rulePlan.outboundLabels,
            "R1 capture does not alter configuration or ordered diagnostics: " + sample.at("name").get<std::string>());
        if (!captured.error.empty()) {
            check(!captured.targetSource, "R1 failed candidate never exposes a partial source snapshot");
            continue;
        }
        check(bool(captured.targetSource), "R1 successful unedited source publishes a snapshot");
        if (captured.targetSource) Invariants(*captured.targetSource);
    }
    check(compared >= 25 && edited >= 10, "R1 covers unedited baselines and explicit edited-source refusal");

    const auto result = Compile("rules: ['DOMAIN,kept.test,DIRECT','UNKNOWN,omitted.test,DIRECT','MATCH,REJECT-DROP','DOMAIN,after.test,DIRECT']\n");
    check(result.error.empty() && result.targetSource && result.targetSource->rules().size() == 1,
        "R1 only actual generated source rules enter the target table");
    if (result.targetSource) {
        const auto& model = *result.targetSource;
        check(model.origins().size() == 4 && model.origins()[0].state == singbox::TargetRuleState::Emitted &&
            model.origins()[1].state == singbox::TargetRuleState::Omitted &&
            model.origins()[2].state == singbox::TargetRuleState::Default &&
            model.origins()[3].state == singbox::TargetRuleState::AfterFinal, "R1 emission/default/omission/post-MATCH are distinguished");
        check(!model.origins()[1].diagnostics.empty(), "R1 skipped rule points to its existing diagnostic");
        check(model.defaultAction().action == "reject" && model.defaultAction().rejectMethod == "drop" &&
            !model.defaultAction().implicit, "R1 DROP default is an action, not an invented outbound");
        const auto native = json::parse(*model.ruleJson(model.rules()[0].id));
        check(native == json{{"domain", {"kept.test"}}, {"outbound", "DIRECT"}}, "R1 source payload is captured before runtime namespace rewriting");
        check(model.rules()[0].matches.size() == 1 && model.rules()[0].matches[0].field == "domain" &&
            model.rules()[0].matches[0].values == std::vector<std::string>{"kept.test"}, "R1 list descriptors expose native fields without reparsing text");
        check(!result.json.empty() && result.json.find("sourceIndex") == std::string::npos && result.json.find("opaque") == std::string::npos,
            "R1 management information never enters kernel JSON");
    }
    const auto implicit = Compile("proxy-groups: [{name: Pick, type: select, proxies: [DIRECT]}]\nrules: []\n");
    check(implicit.targetSource && implicit.targetSource->defaultAction().implicit &&
        implicit.targetSource->defaultAction().outbound == "Pick" && implicit.targetSource->rules().empty(),
        "R1 implicit selector default is captured before source namespacing");
    if (implicit.targetSource) {
        check(implicit.targetSource->objects().size() == 2 && implicit.targetSource->objects()[1].objectId == "Pick" &&
            implicit.targetSource->objects()[1].tag == "Pick", "R1 directory belongs to the local source, not a runtime namespace");
    }
    const auto nodePolicy = Compile("ss://aes-128-gcm:synthetic-secret@example.test:8388#Synthetic\n");
    check(nodePolicy.targetSource && nodePolicy.targetSource->format() == singbox::ProfileFormat::NodeSubscription &&
        !nodePolicy.targetSource->origins().empty() && nodePolicy.targetSource->origins()[0].synthetic,
        "R1 generated node policy does not pretend to be a literal URI rule");
    const auto futureArray = Compile(R"({"outbounds":[{"type":"direct","tag":"plain"}],"route":{"rules":[{"future_array":["preserved"],"outbound":"plain"}],"final":"plain"}})");
    check(futureArray.targetSource && futureArray.targetSource->rules()[0].opaque &&
        futureArray.targetSource->rules()[0].matches.empty(), "R1 unrecognized arrays stay opaque instead of claiming editable semantics");
    const auto invalidAction = Compile(R"({"outbounds":[{"type":"direct","tag":"plain"}],"route":{"rules":[{"action":false,"domain":["invalid.test"],"outbound":"plain"}],"final":"plain"}})");
    check(invalidAction.targetSource && invalidAction.targetSource->rules()[0].opaque &&
        invalidAction.targetSource->rules()[0].action.empty() && json::parse(*invalidAction.targetSource->ruleJson(1))["action"] == false,
        "R1 invalid native action is retained without inventing route semantics");
    const auto duplicate = Compile("rules: ['DOMAIN,duplicate.test,DIRECT','DOMAIN,duplicate.test,DIRECT','MATCH,DIRECT']\n");
    check(duplicate.targetSource && duplicate.targetSource->rules().size() == 2 &&
        duplicate.targetSource->origins()[0].anchor == duplicate.targetSource->origins()[1].anchor &&
        duplicate.targetSource->rules()[0].id != duplicate.targetSource->rules()[1].id,
        "R1 duplicates retain distinct local IDs and ambiguous legacy anchors");

    const auto native = Compile(R"({"outbounds":[{"type":"direct","tag":"plain"}],"route":{"rules":[{"future":{"unsigned":18446744073709551615,"signed":-9223372036854775808,"float":1.25},"outbound":"plain"},{"type":"logical","mode":"or","rules":[{"domain":["nested.test"]}],"outbound":"plain"}],"final":"plain"}})");
    check(native.targetSource && native.targetSource->rules().size() == 2 &&
        native.targetSource->rules()[0].opaque && native.targetSource->rules()[1].opaque,
        "R1 unknown and nested native objects are retained as opaque payloads");
    if (native.targetSource) {
        const auto value = json::parse(*native.targetSource->ruleJson(1));
        check(value["future"]["unsigned"].is_number_unsigned() && value["future"]["unsigned"].get<std::uint64_t>() == std::numeric_limits<std::uint64_t>::max() &&
            value["future"]["signed"].get<std::int64_t>() == std::numeric_limits<std::int64_t>::min() && value["future"]["float"].is_number_float(),
            "R1 private native storage preserves integer bounds and floating point types");
        check(!native.targetSource->defaultAction().implicit && native.targetSource->defaultAction().outbound == "plain" &&
            native.targetSource->origins().back().state == singbox::TargetRuleState::Default,
            "R1 explicit native final has a mapping independent of rule-array objects");
    }
    const auto nativeImplicit = Compile("{\"outbounds\":[{\"type\":\"direct\",\"tag\":\"first\"}]}");
    check(nativeImplicit.targetSource && nativeImplicit.targetSource->defaultAction().implicit &&
        nativeImplicit.targetSource->defaultAction().outbound == "first", "R1 native missing final uses the existing first-outbound default");
    const auto nonscalar = Compile("rules: [{unknown: value}, 'MATCH,DIRECT']\n");
    check(nonscalar.targetSource && nonscalar.targetSource->origins()[0].state == singbox::TargetRuleState::Omitted &&
        !nonscalar.targetSource->origins()[0].diagnostics.empty(), "R1 malformed source entry preserves its rejection mapping");
    const auto provider = Compile("rule-providers:\n  domains: {type: inline, behavior: domain, payload: [provider.test]}\nrules: ['RULE-SET,domains,DIRECT','MATCH,DIRECT']\n");
    check(provider.targetSource && json::parse(provider.targetSource->ruleSetsJson()).size() == 1 &&
        provider.targetSource->rules().size() == 1, "R1 captures the native rule-set declarations referenced by target rules");

    auto options = target_rule_baseline::Options(fixture.at("cases")[0]);
    options.captureTargetSource = true;
    options.globalRules = {{.match = vpn::MatchKind::DomainSuffix, .pattern = "global.test", .connectionId = std::string(vpn::DirectConnectionId()), .id = "global"}};
    const auto global = singbox::compileConfig(options);
    check(global.targetSource && global.targetSource->rules().size() == 1 &&
        global.targetSource->ruleJson(1)->find("global.test") == std::string::npos && global.json.find("global.test") != std::string::npos,
        "R1 global/managed/mode rules are never misidentified as source target rules");
    options.auxiliarySources = {{"broken", "broken", "rules: ['MATCH,missing']\n"}};
    options.globalRules = {{.match = vpn::MatchKind::DomainSuffix, .pattern = "broken.test", .connectionId = "broken", .id = "broken"}};
    const auto failed = singbox::compileConfig(options);
    check(!failed.error.empty() && !failed.targetSource, "R1 failure after source capture cannot publish its pending model");
    std::stop_source cancel; cancel.request_stop();
    const auto cancelled = singbox::compileConfig(options, cancel.get_token());
    check(!cancelled.error.empty() && !cancelled.targetSource && cancelled.json.empty(), "R1 cancellation exposes no source model");

    // Keep an earlier snapshot alive across subsequent conversions and failures.
    if (result.targetSource) check(result.targetSource->rules().size() == 1 && result.targetSource->ruleJson(1)->find("kept.test") != std::string::npos,
        "R1 immutable source payload survives compiler context destruction and later compiles");
    if (failures == before) std::println("Target R1: source emission, mapping, native preservation and failure-publication checks passed");
}
} // namespace target_source_tests
