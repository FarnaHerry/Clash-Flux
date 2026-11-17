// Main source capture runs before namespacing and runtime policy injection.
// No source is re-parsed, and failed candidates never publish the pending model.
// Reuse simple branches without resource side effects. Top-level no-resolve
// also carries its note and position into the new candidate.
bool emitCapturedClashRule(Context& ctx, std::size_t sourceIndex, std::string_view raw) {
    if (!ctx.ruleSource) return false;
    const auto& data = TargetProjection::Read(*ctx.ruleSource);
    if (sourceIndex >= data.origins.size()) return false;
    const auto& origin = data.origins[sourceIndex];
    if (origin.rules.size() != 1 ||
        !origin.anchor.starts_with("clash:") || std::string_view(origin.anchor).substr(6) != trimCopy(raw))
        return false;
    const auto id = origin.rules.front();
    const bool noResolve = std::ranges::binary_search(data.noResolveRuleIds, id);
    if (!noResolve && (!origin.editable || !origin.diagnostics.empty())) return false;
    const auto& rule = data.rules[id-1];
    if (!rule.outbound.empty() && !ctx.tagKnown(rule.outbound)) return false;
    auto& rules = ctx.config["route"]["rules"];
    if (noResolve) {
        ctx.noResolveSourceRules.push_back(rules.size());
        for (const auto index : origin.diagnostics) {
            const auto& note = data.diagnostics[index];
            ctx.note(note.scope, note.level, note.subject, note.detail, note.action);
        }
    }
    rules.push_back(data.payload[id-1]);
    ++ctx.result.targetRulesReused;
    return true;
}

bool convertSourceClashRule(Context& ctx, std::size_t sourceIndex, std::string_view raw) {
    if (emitCapturedClashRule(ctx, sourceIndex, raw)) return true;
    if (!ctx.targetCapture) return convertRule(ctx, raw);
    const auto begin = ctx.config["route"]["rules"].size();
    const auto firstNote = ctx.result.fidelity.size();
    const auto more = convertRule(ctx, raw);
    ctx.targetCapture->Record("clash:" + trimCopy(raw),
        more ? TargetRuleState::Omitted : TargetRuleState::Default,
        ctx.config["route"]["rules"], begin, firstNote, ctx.result.fidelity.size());
    if (!ctx.noResolveSourceRules.empty() && ctx.noResolveSourceRules.back() == begin &&
        ctx.config["route"]["rules"].size() > begin)
        ctx.targetCapture->data->noResolveRuleIds.push_back(ctx.targetCapture->data->rules.back().id);
    return more;
}

void recordAfterFinal(Context& ctx, const YAML::Node& rules, std::size_t begin) {
    if (!ctx.targetCapture) return;
    for (auto index = begin; index < rules.size(); ++index) {
        if (ctx.cancelled()) return;
        const bool scalar = rules[index].IsScalar();
        const auto raw = scalar ? trimCopy(rules[index].Scalar()) : YAML::Dump(rules[index]);
        ctx.targetCapture->Record("clash:" + raw, TargetRuleState::AfterFinal,
            nlohmann::json::array(), 0, 0, 0, !scalar);
    }
}

void captureNativeRules(Context& ctx) {
    if (!ctx.targetCapture) return;
    const auto& rules = ctx.config["route"]["rules"];
    for (std::size_t index = 0; index < rules.size(); ++index) {
        if (ctx.cancelled()) return;
        // Record exactly one original native object; no round-trip through a
        // narrowed model, and no assumptions about unknown field types.
        auto& capture = *ctx.targetCapture;
        capture.Record("json:" + rules[index].dump(), TargetRuleState::Emitted,
            nlohmann::json::array({rules[index]}), 0, 0, 0);
    }
}

void prepareTargetEditBindings(TargetCapture& capture);
std::shared_ptr<const TargetSourceModel> finishTargetSource(Context& ctx, ProfileFormat format) {
    if (!ctx.targetCapture) return {};
    auto& capture = *ctx.targetCapture;
    auto& data = *capture.data;
    data.sourceId = ctx.sourceId;
    data.sourceText = ctx.opt.profileYaml;
    data.format = format;
    const auto final = ctx.config["route"].value("final", "DIRECT");
    const bool reject = ctx.finalTarget == "REJECT" || ctx.finalTarget == "REJECT-DROP";
    data.defaultAction = {reject ? "reject" : "route", reject ? "" : final,
        ctx.finalTarget == "REJECT-DROP" ? "drop" : "", !capture.explicitDefault};
    if (format == ProfileFormat::SingBoxJson || !capture.explicitDefault) {
        capture.Record(format == ProfileFormat::SingBoxJson ? capture.nativeDefaultAnchor.value_or("json:final:" + final) : "implicit:final",
            TargetRuleState::Default, nlohmann::json::array(), 0, 0, 0);
    }
    for (const auto& native : ctx.outbounds) {
        const auto tag = native.value("tag", "");
        const auto type = native.value("type", "");
        if (data.nativeDirect.empty() && type == "direct" && !native.contains("bind_interface") && !native.contains("detour"))
            data.nativeDirect = tag;
        data.objects.push_back({ctx.sourceId, ctx.opt.mainSourceName,
            type == "selector" || type == "urltest" ? vpn::TargetKind::Group : vpn::TargetKind::Node, tag, tag});
    }
    for (const auto& native : ctx.config.value("endpoints", nlohmann::json::array())) {
        const auto tag = native.value("tag", "");
        data.objects.push_back({ctx.sourceId, ctx.opt.mainSourceName, vpn::TargetKind::Node, tag, tag});
    }
    data.ruleSets = format == ProfileFormat::SingBoxJson
        ? ctx.config["route"].value("rule_set", nlohmann::json::array()) : ctx.ruleSets;
    data.diagnostics = ctx.result.fidelity;
    prepareTargetEditBindings(capture);
    return capture.Finish();
}
