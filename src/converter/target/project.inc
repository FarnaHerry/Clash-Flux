// A compatibility projection over the captured target, never a YAML/URI parse.
// Legacy anchors validate persisted intent. Editing exposes only unique
// legacy-supported bindings; native target identities never enter persistence.
ProfileRules projectTargetSource(const TargetSourceModel& model, const ProfileRuleEdits& edits, const RuleSetLibrary& library) {
    ProfileRules output;
    try {
        if (!ValidateProfileRuleEdits(edits, output.error) || !ValidateRuleSetLibrary(library, output.error) || !ValidateRuleSetReferences(library, edits, output.error)) return output;
        const auto& data = TargetProjection::Read(model);
        const auto defaultTarget = targetDefaultTarget(data);
        const auto view = [](const nlohmann::json& native, std::string identity) {
            // Reuse native projection semantics for complex/non-editable fields;
            // no source text decoder is involved. Fast simple facets avoid dumps.
            ProfileRule row{.anchor = std::move(identity), .type = "JSON"};
            if (!native.is_object()) { row.raw = native.dump(); return row; }
            if (const auto target = native.find("outbound"); target != native.end() && target->is_string()) row.target = target->get<std::string>();
            const bool knownAction = !native.contains("action") ||
                (native["action"].is_string() && (native["action"] == "route" || native["action"] == "reject"));
            const bool reject = native.contains("action") && native["action"].is_string() && native["action"] == "reject";
            const bool knownMethod = !native.contains("method") ||
                (reject && native["method"].is_string() && native["method"] == "drop");
            if (reject && knownMethod) row.target = native.value("method", "") == "drop" ? "REJECT-DROP" : "REJECT";
            std::size_t fields = 0;
            bool simple = !row.target.empty() && knownAction && knownMethod;
            for (auto it = native.begin(); it != native.end(); ++it) {
                if (it.key() == "outbound" || it.key() == "action" || it.key() == "method") continue;
                ++fields;
                if (validPersonalMatch(it.key()) && it->is_array() && it->size() == 1 && (*it)[0].is_string()) {
                    row.type = it.key(); row.pattern = (*it)[0].get<std::string>();
                } else simple = false;
            }
            if (!simple || fields != 1) { row.type = "JSON"; row.pattern.clear(); row.raw = native.dump(); }
            return row;
        };
        const auto appendOrigin = [&](std::size_t index, const ProfileRule* legacy = nullptr) {
            const auto& origin = data.origins[index];
            for (const auto id : origin.rules) {
                auto row = view(data.payload[std::size_t(id-1)], "target:" + std::to_string(id));
                if (legacy) {
                    row.enabled = legacy->enabled; row.editId = legacy->editId; row.origin = legacy->origin;
                }
                output.rules.push_back(std::move(row));
            }
        };
        const auto appendDefault = [&](std::string target, std::string edit = {}) {
            const bool modified = !edit.empty();
            output.rules.push_back({.anchor = "target:final",
                .type = "MATCH", .target = std::move(target), .editId = std::move(edit), .origin = modified ? "replaced" : "source",
                .terminal = true, .implicit = data.defaultAction.implicit});
        };
        if (edits.rules.empty()) {
            for (std::size_t index = 0; index < data.origins.size(); ++index) appendOrigin(index);
            appendDefault(defaultTarget);
            return output;
        }
        auto originals = targetSourceOriginals(data);
        auto plan = personalRulePlan(std::move(originals), edits);
        if (!plan.error.empty()) { output.error = std::move(plan.error); return output; }
        CompileOptions options;
        options.profileYaml = data.format == ProfileFormat::SingBoxJson ? "{}" : "";
        options.mainConnectionId = data.sourceId; options.profileRuleEdits = edits; options.ruleSetLibrary = library;
        Context ctx{options}; ctx.sourceId = data.sourceId; ctx.finalTarget = defaultTarget;
        ctx.config = {{"route", {{"rules", nlohmann::json::array()}, {"final", defaultTarget}}}, {"outbounds", nlohmann::json::array()}};
        for (const auto& object : data.objects) {
            ctx.knownTags.push_back(object.tag);
            nlohmann::json native{{"tag", object.tag}, {"type", object.tag == data.nativeDirect ? "direct" : "socks"}};
            ctx.outbounds.push_back(native); ctx.config["outbounds"].push_back(std::move(native));
        }
        // The edit plan preserves the relative order of original rows. New
        // rules carry no anchor; the compatibility default is independent.
        std::size_t nextSource = 0;
        for (const auto& row : plan.rules) {
            const bool hasOrigin = !row.anchor.empty() && !row.implicit;
            const auto index = nextSource;
            if (hasOrigin) {
                if (index >= data.origins.size() || data.origins[index].anchor != row.anchor) {
                    output.error = "目标规则来源映射失效"; output.rules.clear(); return output;
                }
                ++nextSource;
            }
            const bool sourceDefault = hasOrigin && data.origins[index].state == TargetRuleState::Default;
            if (row.terminal) {
                if (!row.enabled) continue;
                if (row.origin == "source" && !row.implicit && !sourceDefault) continue;
                appendDefault(row.origin == "source" ? defaultTarget : personalTarget(ctx, row), row.editId);
                break;
            }
            if (row.origin == "source" || row.origin == "disabled") {
                if (sourceDefault && row.enabled) { appendDefault(defaultTarget); break; }
                if (hasOrigin) appendOrigin(index, &row);
                continue;
            }
            if (!row.enabled) continue;
            const auto begin = ctx.config["route"]["rules"].size();
            if (!emitPersonalRule(ctx, row)) { output.rules.clear(); output.error = ctx.result.error; return output; }
            for (auto ruleIndex = begin; ruleIndex < ctx.config["route"]["rules"].size(); ++ruleIndex) {
                auto projected = view(ctx.config["route"]["rules"][ruleIndex],
                    "edit:" + row.editId + ":" + std::to_string(ruleIndex-begin));
                if (hasOrigin && data.origins[index].rules.size() == 1)
                    projected.targetId = "target:" + std::to_string(data.origins[index].rules.front());
                projected.editId = row.editId; projected.origin = row.origin; projected.position = row.position;
                if (row.type == "rule_set") projected.pattern = row.pattern; // Keep the user's resource label identity.
                output.rules.push_back(std::move(projected));
            }
        }
        if (output.rules.empty() || !output.rules.back().terminal) appendDefault(defaultTarget);
    } catch (const std::exception& error) { output.rules.clear(); output.error = error.what(); }
    return output;
}

ProfileRules inspectTargetSource(const TargetSourceModel* model, const ProfileRules& effective,
                                 const ProfileRuleEdits& edits) {
    auto output = effective;
    const bool hasFinalEdit = std::ranges::any_of(edits.rules, [](const auto& edit) {
        return edit.operation == "final";
    });
    std::set<std::string> visible;
    for (auto& row : output.rules) {
        if (!row.editId.empty()) visible.insert(row.editId);
        if (row.targetId.empty()) row.targetId = row.anchor;
        if (row.terminal) row.editable = model && (!hasFinalEdit || !row.editId.empty());
        else if (model && row.type != "JSON")
            row.editable = row.origin == "added" || row.origin == "replaced" ||
                targetEditOrigin(TargetProjection::Read(*model), row.targetId);
    }
    for (const auto& edit : edits.rules) {
        if (visible.contains(edit.id)) continue;
        const bool terminal = edit.operation == "final";
        const bool disable = edit.operation == "disable";
        output.rules.push_back({.raw = edit.anchor,
            .type = terminal ? "MATCH" : disable ? "JSON" : edit.match,
            .pattern = disable ? edit.anchor : edit.pattern,
            .target = disable ? "" : edit.target, .editId = edit.id,
            .origin = edit.enabled ? "unapplied" : "inactive", .position = edit.operation,
            .editable = model && !disable, .enabled = edit.enabled, .terminal = terminal,
            .targetId = "edit:" + edit.id + ":record"});
    }
    return output;
}
