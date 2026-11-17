// R2: target-only read projection and the production application artifact index.
namespace target_project_tests {
using Cache = clashflux::target_source::Cache<singbox::CompileOptions, singbox::TargetSourceModel>;

singbox::ProfileRules Project(const std::string& source, const singbox::ProfileRuleEdits& edits = {},
                             const singbox::RuleSetLibrary& library = {}) {
    const auto compiled = target_source_tests::Compile(source);
    check(compiled.targetSource && compiled.error.empty(), "R2 test source captures successfully");
    return compiled.targetSource ? singbox::ProjectTargetSource(*compiled.targetSource, edits, library) : singbox::ProfileRules{};
}

void CheckEditor() {
    singbox::CompileOptions options;
    options.mainConnectionId = "rules-r3";
    options.profileYaml = "rules: ['DOMAIN,unique.test,DIRECT','DOMAIN,repeat.test,DIRECT','DOMAIN,repeat.test,DIRECT',"
        "'IP-CIDR,192.0.2.0/24,DIRECT,no-resolve','DOMAIN,omitted.test,PASS','MATCH,DIRECT','DOMAIN,late.test,DIRECT']";
    const auto capture = [&] {
        auto input = options;
        input.captureTargetSource = true;
        input.profileRuleEdits = {};
        return singbox::compileConfig(input).targetSource;
    };
    auto model = capture();
    check(bool(model), "R3 captures editable and opaque Clash rule branches");
    if (!model) return;
    const auto editor = [&](const singbox::ProfileRuleEdits& edits = singbox::ProfileRuleEdits{}) {
        const auto projected = singbox::ProjectTargetSource(*model, edits);
        return singbox::InspectTargetSource(model.get(), projected, edits);
    };
    auto view = editor();
    check(view.error.empty() && view.rules.size() == 5 &&
        view.rules.front().editable && !view.rules[1].editable && !view.rules[2].editable &&
        !view.rules[3].editable && view.rules.back().terminal && view.rules.back().editable,
        "R3 only a unique, simple and emitted Clash rule and the default permit new edits");
    std::string error;
    singbox::ProfileRuleEdit replace{.id="replace", .operation="replace", .match="domain", .pattern="new.test", .target="DIRECT"};
    check(singbox::BindTargetSourceEdit(*model, "target:1", replace, error) &&
        replace.anchor == "clash:DOMAIN,unique.test,DIRECT", "R3 persists the legacy anchor rather than the target identity");
    check(!singbox::BindTargetSourceEdit(*model, "target:2", replace, error) &&
        !singbox::BindTargetSourceEdit(*model, "target:4", replace, error) &&
        !singbox::BindTargetSourceEdit(*model, "target:01", replace, error) &&
        !singbox::BindTargetSourceEdit(*model, "target:final", replace, error) &&
        replace.anchor == "clash:DOMAIN,unique.test,DIRECT",
        "R3 rejects ambiguous, read-only and invalid identities without changing the draft binding");
    singbox::ProfileRuleEdit final{.id="final", .operation="final", .target="REJECT"};
    check(singbox::BindTargetSourceEdit(*model, "target:final", final, error) && final.anchor.empty(),
        "R3 implicit and explicit defaults do not store a source index or anchor");
    auto disabled = replace;
    disabled.id = "parked";
    disabled.enabled = false;
    auto late = replace;
    late.id = "late";
    late.anchor = "clash:DOMAIN,late.test,DIRECT";
    options.profileRuleEdits.rules = {replace, final, disabled, late};
    view = editor(options.profileRuleEdits);
    check(view.error.empty() && view.rules.size() == 7 && view.rules.front().targetId == "target:1" &&
        view.rules[5].origin == "inactive" && !view.rules[5].enabled &&
        view.rules[6].origin == "unapplied" && view.rules[6].enabled,
        "R3 replacement retains the target ID and inactive/unreachable modifications remain manageable");
    options.profileRuleEdits.rules.back().anchor = "clash:DOMAIN,missing.test,DIRECT";
    view = editor(options.profileRuleEdits);
    check(!view.error.empty() && view.rules.size() == 4 && view.rules.back().editId == "late" && view.rules.back().enabled,
        "R3 recovery records survive a conflict that prevents calculating the active view");
    options.profileRuleEdits.rules = {final};
    const auto cold = singbox::compileConfig(options);
    const auto warm = singbox::compileConfig(options, {}, model.get());
    check(cold.error.empty() && warm.json == cold.json && warm.warnings == cold.warnings &&
        warm.rulePlan.origins == cold.rulePlan.origins && warm.targetRulesReused == 2,
        "R3 composed Clash JSON and diagnostics match cold compilation, including reused no-resolve");
    auto inserted = replace;
    inserted.operation = "prepend";
    inserted.anchor.clear();
    inserted.match = "ip_cidr";
    inserted.pattern = "198.51.100.0/24";
    options.profileRuleEdits.rules = {inserted, final};
    const auto insertedCold = singbox::compileConfig(options);
    const auto insertedWarm = singbox::compileConfig(options, {}, model.get());
    check(insertedCold.error.empty() && insertedWarm.json == insertedCold.json &&
        insertedWarm.warnings == insertedCold.warnings && insertedWarm.targetRulesReused == 2,
        "a preceding personal CIDR shifts the replayed no-resolve rule in the generated route");
    if (!insertedWarm.json.empty()) {
        const auto rules = json::parse(insertedWarm.json).at("route").at("rules");
        const auto resolve = std::find_if(rules.begin(), rules.end(), [](const auto& rule) {
            return rule.value("action", "") == "resolve";
        });
        check(resolve != rules.end() && resolve + 1 != rules.end() &&
            (resolve + 1)->value("ip_cidr", json::array()) == json::array({"198.51.100.0/24"}),
            "the personal CIDR triggers DNS ahead of the no-resolve source rule");
    }
    options.profileYaml += "\n# different source version\n";
    check(singbox::compileConfig(options, {}, model.get()).targetRulesReused == 0,
        "R3 source identity includes exact content");
    options.profileYaml = R"({"outbounds":[{"type":"direct","tag":"d"}],"route":{"rules":[{"domain":["only.test"],"outbound":"d"},{"domain":["one.test","two.test"],"outbound":"d"}],"final":"d"}})";
    model = capture();
    check(bool(model), "R3 captures native routes");
    if (!model) return;
    view = editor();
    check(view.rules.size() == 3 && view.rules.front().editable && !view.rules[1].editable &&
        singbox::BindTargetSourceEdit(*model, "target:1", replace, error) && replace.anchor.starts_with("json:{"),
        "R3 native bindings preserve canonical JSON anchors and reject multi-condition editing");
    options.profileRuleEdits.rules = {replace, final};
    const auto nativeCold = singbox::compileConfig(options);
    const auto nativeWarm = singbox::compileConfig(options, {}, model.get());
    check(nativeCold.error.empty() && nativeWarm.json == nativeCold.json &&
        nativeWarm.warnings == nativeCold.warnings && nativeWarm.targetRulesReused == 1,
        "R3 native edit composition preserves the opaque source branch");
}

void CheckCompatibilityDefaults() {
    singbox::CompileOptions input;
    input.mainConnectionId = "rules-r4";
    for (const auto* source : {
             "proxy-groups: [{name: 'Pick)', type: select, proxies: [DIRECT]}]\n"
             "rules: [[DOMAIN, invalid.test, DIRECT], 'DOMAIN,first.test,DIRECT','MATCH,Pick)','DOMAIN,after.test,DIRECT']\n",
             "rules: ['MATCH,Pick(a,b)','DOMAIN,after.test,DIRECT','MATCH,DIRECT']\n"
         }) {
        input.profileYaml = source;
        input.captureTargetSource = true;
        input.profileRuleEdits = {};
        const auto baseline = singbox::compileConfig(input);
        check(bool(baseline.targetSource), "R4 captures non-scalar rules and edge cases around MATCH");
        if (!baseline.targetSource) continue;
        input.captureTargetSource = false;
        input.profileRuleEdits.rules = {{.id="extra", .operation="prepend",
            .match="domain", .pattern="extra.test", .target="DIRECT"}};
        const auto cold = singbox::compileConfig(input);
        const auto warm = singbox::compileConfig(input, {}, baseline.targetSource.get());
        check(cold.error.empty() && warm.error == cold.error && warm.json == cold.json &&
            warm.warnings == cold.warnings && warm.rulePlan.origins == cold.rulePlan.origins,
            "R4 shared origins retain legacy MATCH/default compatibility and non-scalar diagnostics");
        const auto projected = singbox::ProjectTargetSource(*baseline.targetSource, input.profileRuleEdits);
        const bool reachesAfter = cold.json.find("after.test") != std::string::npos;
        check(projected.error.empty() && !projected.rules.empty() && projected.rules.back().terminal &&
            std::ranges::any_of(projected.rules, [](const auto& row) { return row.pattern == "after.test"; }) == reachesAfter,
            "R4 the target view terminates at the actual emitted MATCH");
    }
}

void Run() {
    const auto before = failures;
    CheckEditor();
    CheckCompatibilityDefaults();
    const std::string source = "rules: ['DOMAIN,original.test,DIRECT','UNKNOWN,omitted.test,DIRECT','MATCH,REJECT-DROP','DOMAIN,after.test,DIRECT']\n";
    auto projected = Project(source);
    check(projected.error.empty() && projected.rules.size() == 2 && projected.original.empty(),
        "R2 read view has only actual target branches and a separate default, no source-rule copy");
    check(projected.rules[0].anchor == "target:1" && projected.rules[0].type == "domain" &&
        projected.rules[0].pattern == "original.test" && !projected.rules[0].editable &&
        projected.rules.back().terminal && projected.rules.back().target == "REJECT-DROP",
        "R2 target IDs, native conditions and DROP default are visible without editable source anchors");
    singbox::ProfileRuleEdits edits;
    edits.rules.push_back({.id="disable", .operation="disable", .anchor="clash:DOMAIN,original.test,DIRECT"});
    projected = Project(source, edits);
    check(projected.error.empty() && projected.rules.size() == 2 && !projected.rules[0].enabled &&
        projected.rules[0].origin == "disabled" && projected.rules[0].editId == "disable",
        "R2 disabled target stays visible, with its saved operation identity");
    edits.rules[0] = {.id="replace", .operation="replace", .anchor="clash:DOMAIN,original.test,DIRECT",
        .match="domain_suffix", .pattern="changed.test", .target="REJECT-DROP"};
    edits.rules.push_back({.id="head", .operation="prepend", .match="domain", .pattern="first.test", .target="DIRECT"});
    edits.rules.push_back({.id="tail", .operation="before_final", .match="domain", .pattern="last.test", .target="DIRECT"});
    edits.rules.push_back({.id="final", .operation="final", .target="DIRECT"});
    projected = Project(source, edits);
    check(projected.error.empty() && projected.rules.size() == 4 && projected.rules[0].pattern == "first.test" &&
        projected.rules[1].pattern == "changed.test" && projected.rules[1].target == "REJECT-DROP" &&
        projected.rules[2].pattern == "last.test" && projected.rules[3].target == "DIRECT" &&
        projected.rules[3].origin == "replaced", "R2 existing prepend/replace/before-final/final intent retains native order and actions");
    edits.rules[0].anchor = "clash:DOMAIN,gone.test,DIRECT";
    check(Project(source, edits).error.find("冲突") != std::string::npos, "R2 missing anchors remain explicit conflicts");
    edits.rules.resize(1); edits.rules[0].anchor = "clash:DOMAIN,duplicate.test,DIRECT";
    check(Project("rules: ['DOMAIN,duplicate.test,DIRECT','DOMAIN,duplicate.test,DIRECT','MATCH,DIRECT']", edits).error.find("冲突") != std::string::npos,
        "R2 duplicate anchors are never guessed by position");
    edits.rules.clear();
    edits.rules.push_back({.id="fallback", .match="domain", .pattern="fallback.test", .target="gone", .unavailable="main"});
    edits.rules.push_back({.id="final", .operation="final", .target="DIRECT"});
    projected = Project(source, edits);
    check(projected.error.empty() && projected.rules[0].target == "REJECT-DROP" && projected.rules.back().target == "DIRECT",
        "R2 unavailable=main uses untouched source default, even when the user changes final");
    const auto duplicate = Project("rules: ['DOMAIN,duplicate.test,DIRECT','DOMAIN,duplicate.test,DIRECT','MATCH,DIRECT']");
    check(duplicate.rules.size() == 3 && duplicate.rules[0].anchor != duplicate.rules[1].anchor,
        "R2 unedited duplicates have distinct view keys");
    edits.rules = {{.id="after", .operation="replace", .anchor="clash:DOMAIN,after.test,DIRECT", .match="domain", .pattern="unreachable.test", .target="DIRECT"}};
    projected = Project(source, edits);
    check(projected.error.empty() && projected.rules.size() == 2 && projected.rules[0].pattern == "original.test",
        "R2 post-MATCH edits remain unreachable as in the existing compiler");
    edits.rules = {{.id="repair", .operation="replace", .anchor="clash:DOMAIN,https://invalid.test,DIRECT", .match="domain", .pattern="repaired.test", .target="DIRECT"}};
    projected = Project("rules: ['DOMAIN,https://invalid.test,DIRECT','MATCH,DIRECT']", edits);
    check(projected.error.empty() && projected.rules[0].pattern == "repaired.test", "R2 saved replacements can still repair omitted editable source rows");

    const std::string native = R"({"outbounds":[{"type":"direct","tag":"plain"}],"route":{"rules":[{"domain":["one.test"],"outbound":"plain"},{"outbound":"plain"},{"domain":["late.test"],"outbound":"plain"}],"final":"plain"}})";
    edits.rules = {{.id="insert", .operation="before_final", .match="domain", .pattern="insert.test", .target="DIRECT"}};
    projected = Project(native, edits);
    check(projected.error.empty() && projected.rules.size() == 5 && projected.rules[1].pattern == "insert.test" &&
        projected.rules[1].target == "plain" && projected.rules[2].type == "JSON" && projected.rules[3].pattern == "late.test",
        "R2 native before-final inserts before catch-all, preserves later native objects and chooses eligible direct");
    projected = Project(R"({"outbounds":[{"type":"socks","tag":"__clashflux_personal_direct","server":"127.0.0.1","server_port":1080},{"type":"direct","tag":"bound","bind_interface":"lo"}],"route":{"final":"bound"}})", edits);
    check(projected.error.empty() && projected.rules[0].target == "__clashflux_personal_direct-1",
        "R2 native DIRECT avoids bound exits and source tag collisions");
    edits.rules = {{.id="final", .operation="final", .target="REJECT"}};
    projected = Project("{}", edits);
    check(projected.error.empty() && projected.rules.size() == 1 && projected.rules[0].target == "REJECT",
        "R2 empty native input preserves its legacy implicit-final compatibility anchor");
    projected = Project(R"({"outbounds":[{"type":"direct","tag":"plain"}],"route":{"rules":[{"future":{"n":18446744073709551615},"outbound":"plain"},{"domain":["bad.test"],"action":false,"outbound":"plain"}],"final":"plain"}})");
    check(projected.error.empty() && projected.rules[0].type == "JSON" && projected.rules[0].raw.find("18446744073709551615") != std::string::npos &&
        projected.rules[1].type == "JSON" && json::parse(projected.rules[1].raw)["action"] == false,
        "R2 unknown native payload and invalid field types remain read-only JSON without narrowing or fabricated actions");
    singbox::RuleSetLibrary library;
    library.resources.push_back({.id="personal", .name="Personal", .type="inline", .format="source", .content=R"({"version":4,"rules":[{"domain":["set.test"]}]})"});
    edits.rules = {{.id="set", .match="rule_set", .pattern="personal", .target="DIRECT"}};
    projected = Project(source, edits, library);
    check(projected.error.empty() && projected.rules[0].type == "rule_set" && projected.rules[0].pattern == "personal",
        "R2 personal resource view retains the library ID used for its display label");
    check(!Project(source, edits).error.empty(), "R2 missing personal resources fail rather than turning into empty successful views");

    const auto fixture = target_rule_baseline::Read(std::filesystem::path(__FILE__).parent_path()/"fixtures"/"target-rule-baseline.json");
    for (const auto& sample : fixture.at("cases")) {
#if !defined(__linux__) || defined(__ANDROID__)
        if (sample.value("platform", "") == "linux") continue;
#endif
        auto options = target_rule_baseline::Options(sample);
        const auto saved = options.profileRuleEdits; options.profileRuleEdits = {}; options.captureTargetSource = true;
        const auto expected = sample.at("expected").at("projection").at("error").get<std::string>();
        if (!saved.rules.empty()) {
            std::string refreshError;
            const bool valid = singbox::ValidateProfileUpdate(options.profileYaml, saved, refreshError, false);
            check(valid == expected.empty() && refreshError == expected,
                "R4 production refresh preserves the frozen binding result: " + sample.at("name").get<std::string>());
        }
        const auto baseline = singbox::compileConfig(options);
        if (!baseline.targetSource) continue;
        const auto view = singbox::ProjectTargetSource(*baseline.targetSource, saved);
        check(view.error == expected, "R2 replay preserves frozen legacy conflict contracts: " + sample.at("name").get<std::string>());
        options.captureTargetSource = false;
        options.profileRuleEdits = saved;
        const auto cold = singbox::compileConfig(options);
        const auto warm = singbox::compileConfig(options, {}, baseline.targetSource.get());
        check(warm.json == cold.json && warm.error == cold.error && warm.warnings == cold.warnings &&
            warm.rulePlan.origins == cold.rulePlan.origins && warm.rulePlan.ruleCount == cold.rulePlan.ruleCount,
            "R3 cached composition matches cold output and trace: " + sample.at("name").get<std::string>());
        std::set<std::string> keys;
        for (const auto& row : view.rules) check(!row.anchor.empty() && keys.insert(row.anchor).second && !row.editable,
            "R2 actual target branches have unique read-only view identities");
    }

    singbox::CompileOptions options; options.captureTargetSource = true; options.mainConnectionId = "cache-r2";
    options.profileYaml = source;
    const auto originalOptions = options;
    Cache cache;
    unsigned loads = 0;
    const auto get = [&](std::uint64_t revision = 0) {
        return cache.Get(options, revision, [&] { ++loads; return singbox::compileConfig(options); });
    };
    auto first = get(), same = get();
    check(first.model && same.model == first.model && same.reused && loads == 1,
        "R2 real production artifact loader compiles once for repeated consumers");
    singbox::ProjectTargetSource(*first.model, {});
    edits.rules = {{.id="personal", .match="domain", .pattern="personal.test", .target="DIRECT"}};
    singbox::ProjectTargetSource(*first.model, edits);
    same = get();
    check(same.reused && loads == 1, "R2 view changes and personal replay never call the source compiler");
    const auto resources = get(1);
    check(resources.model && resources.model != first.model && !resources.reused && loads == 2,
        "R2 explicit resource revision invalidates the artifact identity");
    options.mainSourceName = "renamed"; auto renamed = get();
    options.profileYaml += "# changed bytes\n"; auto changed = get();
    options.mainConnectionId = "another-source"; auto another = get();
    check(loads == 5 && renamed.model && changed.model && another.model && another.model->sourceId() == "another-source",
        "R2 complete source bytes, display identity and source ID are checked independently");
    check(cache.retainedKeyBytes() <= 8*1024*1024, "R2 weak artifact key index stays inside its budget");
    const auto evicted = cache.Get(originalOptions, 0, [&] { ++loads; return singbox::compileConfig(originalOptions); });
    check(evicted.model && evicted.model != first.model && loads == 6,
        "R2 the fifth distinct live key evicts the oldest index entry without revoking its caller-held model");
    Cache tiny(1); unsigned tinyLoads = 0;
    const auto tinyLoad = [&] { return tiny.Get(options, 0, [&] { ++tinyLoads; return singbox::compileConfig(options); }); };
    auto tinyFirst = tinyLoad(), tinyNext = tinyLoad();
    check(tinyFirst.model && tinyNext.model && tinyLoads == 2 && tiny.retainedKeyBytes() == 0,
        "R2 oversized keys are not retained, even while both caller-owned snapshots live");
    Cache weak; unsigned weakLoads = 0;
    { auto held = weak.Get(options, 0, [&] { ++weakLoads; return singbox::compileConfig(options); }); }
    auto expired = weak.Get(options, 0, [&] { ++weakLoads; return singbox::compileConfig(options); });
    check(expired.model && weakLoads == 2, "R2 the index never keeps an otherwise unused target artifact alive");
    Cache failuresCache; unsigned attempts = 0;
    auto failed = failuresCache.Get(options, 0, [&] { ++attempts; singbox::CompileResult value; value.error = "injected"; return value; });
    auto recovered = failuresCache.Get(options, 0, [&] { ++attempts; return singbox::compileConfig(options); });
    check(!failed.model && failed.error == "injected" && recovered.model && attempts == 2,
        "R2 loader failure leaves no empty success or poisoned cache entry");
    Cache concurrent; std::atomic<unsigned> concurrentLoads{0};
    std::array<Cache::Result, 4> readers;
    { std::vector<std::jthread> threads;
      for (std::size_t i = 0; i < readers.size(); ++i) threads.emplace_back([&, i] {
          readers[i] = concurrent.Get(options, 0, [&] { ++concurrentLoads; return singbox::compileConfig(options); });
      }); }
    check(concurrentLoads == 1 && std::ranges::all_of(readers, [&](const auto& value) { return value.model == readers[0].model; }),
        "R2 concurrent production cache requests share exactly one capture");
    const auto ordinary = singbox::compileConfig(options);
    auto dependent = ordinary; dependent.hasFileProviders = true;
    check(!Cache::Reusable(options, dependent), "R2 file-provider artifacts require fresh dependency reads");
    dependent = ordinary; dependent.ruleSetResources.push_back({"geo", "https://example.test/geo.srs"});
    check(!Cache::Reusable(options, dependent), "R2 GEO disk-dependent artifacts do not reuse path-only identity");
    dependent = ordinary; dependent.httpRuleProviders.push_back({.cacheKey="http-body"});
    check(!Cache::Reusable(options, dependent), "R2 unpinned HTTP bodies never reuse a path-only artifact");
    options.ruleProviderContents["http-body"] = "payload: [pinned.test]";
    check(Cache::Reusable(options, dependent), "R2 explicit immutable HTTP bodies can participate in complete identity");
    {
        const RuleSetTestDirectory directory;
        singbox::CompileOptions fileOptions;
        fileOptions.captureTargetSource = true;
        fileOptions.ruleProviderDir = directory.path.string();
        fileOptions.profileYaml = "rule-providers:\n  local: {type: file, behavior: domain, path: rules.yaml}\nrules: ['RULE-SET,local,DIRECT','MATCH,DIRECT']\n";
        std::ofstream(directory.path/"rules.yaml") << "payload: [old-file.test]\n";
        Cache files; unsigned fileLoads = 0;
        const auto read = [&] { return files.Get(fileOptions, 0, [&] { ++fileLoads; return singbox::compileConfig(fileOptions); }); };
        const auto old = read();
        std::ofstream(directory.path/"rules.yaml") << "payload: [new-file.test]\n";
        const auto updated = read();
        check(old.model && updated.model && old.model != updated.model && fileLoads == 2 &&
            files.retainedKeyBytes() == 0 && old.model->ruleSetsJson().find("old-file.test") != std::string::npos &&
            updated.model->ruleSetsJson().find("new-file.test") != std::string::npos,
            "R2 actual same-path file-provider changes cannot reuse the earlier converted payload");
    }
    if (failures == before) std::println("Target R2: native view, saved-intent replay, real loader counts, singleflight and resource/budget checks passed");
}

int Benchmark(int count) {
    if (count < 1 || count > 100000) return 2;
    singbox::CompileOptions options; options.captureTargetSource = true; options.mainConnectionId = "target-r2-benchmark";
    options.profileYaml = "rules:\n";
    for (int i = 0; i < count; ++i) options.profileYaml += std::format("  - DOMAIN-SUFFIX,rule{}.test,DIRECT\n", i);
    options.profileYaml += "  - MATCH,DIRECT\n";
    using Clock = std::chrono::steady_clock;
    const auto ms = [](auto start) { return std::chrono::duration<double, std::milli>(Clock::now()-start).count(); };
    Cache cache; unsigned loads = 0;
    const auto get = [&] { return cache.Get(options, 0, [&] { ++loads; return singbox::compileConfig(options); }); };
    auto start = Clock::now(); const auto cold = get(); const auto coldMs = ms(start);
    if (!cold.model) return 1;
    start = Clock::now(); const auto view = singbox::ProjectTargetSource(*cold.model); const auto viewMs = ms(start);
    start = Clock::now(); const auto warm = get(); const auto warmMs = ms(start);
    singbox::ProfileRuleEdits edits;
    edits.rules.push_back({.id="personal", .match="domain", .pattern="personal.test", .target="DIRECT"});
    start = Clock::now(); const auto edited = singbox::ProjectTargetSource(*warm.model, edits); const auto editMs = ms(start);
    check(view.error.empty() && edited.error.empty() && view.rules.size() == std::size_t(count+1) &&
        edited.rules.size() == std::size_t(count+2) && loads == 1 && warm.reused, "R2 benchmark measures shared real capture and projections");
    std::println("{}", json{{"rows",count},{"source_bytes",options.profileYaml.size()},{"cold_ms",coldMs},
        {"view_ms",viewMs},{"warm_ms",warmMs},{"edit_ms",editMs},{"source_compiles",loads},
        {"model_billed_bytes",cold.model->retainedBytes()},{"weak_key_bytes",cache.retainedKeyBytes()}}.dump());
    return failures ? 1 : 0;
}
} // namespace target_project_tests
