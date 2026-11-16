// A compatibility projection over the captured target, never a YAML/URI parse.
// Legacy source anchors only validate existing intent; the visible objects are
// actual target branches. The interactive editor still uses its old contract.
ProfileRules projectTargetSource(const TargetSourceModel& model, const ProfileRuleEdits& edits, const RuleSetLibrary& library) {
    ProfileRules output;
    try {
        if (!ValidateProfileRuleEdits(edits, output.error) || !ValidateRuleSetLibrary(library, output.error) || !ValidateRuleSetReferences(library, edits, output.error)) return output;
        const auto& data = TargetProjection::Read(model);
        const auto defaultTarget = data.defaultAction.action == "reject"
            ? (data.defaultAction.rejectMethod == "drop" ? std::string("REJECT-DROP") : std::string("REJECT")) : data.defaultAction.outbound;
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
                if (legacy) { row.enabled = legacy->enabled; row.editId = legacy->editId; row.origin = legacy->origin; }
                output.rules.push_back(std::move(row));
            }
        };
        const auto appendDefault = [&](std::string target, std::string edit = {}) {
            const bool modified = !edit.empty();
            output.rules.push_back({.anchor = "target:final", .type = "MATCH", .target = std::move(target),
                .editId = std::move(edit), .origin = modified ? "replaced" : "source", .terminal = true, .implicit = data.defaultAction.implicit});
        };
        if (edits.rules.empty()) {
            for (std::size_t index = 0; index < data.origins.size(); ++index) appendOrigin(index);
            appendDefault(defaultTarget);
            return output;
        }
        std::vector<ProfileRule> originals;
        if (data.format == ProfileFormat::SingBoxJson) {
            auto final = data.origins.back().anchor;
            final.erase(0, std::string("json:final:").size());
            originals = nativeProfileRules({{"route", {{"rules", data.payload}, {"final", final}}}});
        } else {
            for (const auto& origin : data.origins) {
                if (origin.anchor.starts_with("clash:")) {
                    auto row = clashProfileRule(origin.anchor.substr(6)); row.anchor = origin.anchor;
                    originals.push_back(std::move(row));
                } else originals.push_back({.anchor = origin.anchor, .type = "MATCH", .target = defaultTarget,
                    .editable = true, .terminal = true, .implicit = true});
            }
        }
        auto plan = personalRulePlan(std::move(originals), edits);
        if (!plan.error.empty()) { output.error = std::move(plan.error); return output; }
        std::map<std::string, std::vector<std::size_t>> indices;
        std::map<std::string, std::size_t> cursors;
        for (std::size_t index = 0; index < data.origins.size(); ++index) indices[data.origins[index].anchor].push_back(index);
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
        for (const auto& row : plan.rules) {
            std::optional<std::size_t> origin;
            if (!row.anchor.empty()) {
                const auto found = indices.find(row.anchor);
                auto& next = cursors[row.anchor];
                if (found == indices.end() || next >= found->second.size()) { output.error = "目标规则来源映射失效"; output.rules.clear(); return output; }
                origin = found->second[next++];
            }
            if (row.terminal) {
                if (!row.enabled) continue;
                appendDefault(row.origin == "source" ? defaultTarget : personalTarget(ctx, row), row.editId);
                break;
            }
            if (row.origin == "source" || row.origin == "disabled") {
                if (origin) appendOrigin(*origin, &row);
                continue;
            }
            if (!row.enabled) continue;
            const auto begin = ctx.config["route"]["rules"].size();
            if (!emitPersonalRule(ctx, row)) { output.rules.clear(); output.error = ctx.result.error; return output; }
            for (auto index = begin; index < ctx.config["route"]["rules"].size(); ++index) {
                auto projected = view(ctx.config["route"]["rules"][index], "edit:" + row.editId + ":" + std::to_string(index-begin));
                projected.editId = row.editId; projected.origin = row.origin; projected.position = row.position;
                if (row.type == "rule_set") projected.pattern = row.pattern; // Keep the user's resource label identity.
                output.rules.push_back(std::move(projected));
            }
        }
    } catch (const std::exception& error) { output.rules.clear(); output.error = error.what(); }
    return output;
}
