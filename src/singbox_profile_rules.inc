// Source rule projection and personal edits share one plan. No YAML is emitted.
bool validPersonalMatch(const std::string& kind) {
    return kind == "domain" || kind == "domain_suffix" || kind == "ip_cidr" ||
        kind == "domain_keyword" || kind == "domain_regex" || kind == "process_name" || kind == "rule_set";
}

std::string personalRuleSetTag(const RuleSetLibrary& library, const std::string& id) {
    std::string tag = "cf_personal_set_" + id;
    const auto resource = std::ranges::find(library.resources, id, &PersonalRuleSet::id);
    if (resource != library.resources.end() && resource->type == "remote") {
        // The native remote cache is keyed by tag. Encode the complete download
        // identity so changing URL/format cannot reuse a previous URL's cache.
        static constexpr char hex[] = "0123456789abcdef";
        tag += '_';
        for (const unsigned char c : resource->format + "\n" + resource->url) {
            tag += hex[c >> 4]; tag += hex[c & 15];
        }
        if (resource->refresh_generation != 0) tag += "_fetch_" + std::to_string(resource->refresh_generation);
    }
    return tag;
}

ProfileRule clashProfileRule(std::string raw) {
    ProfileRule row;
    row.raw = trimCopy(raw);
    row.anchor = "clash:" + row.raw;
    const auto parts = splitLogicalParts(row.raw);
    if (!parts || parts->empty()) { row.type = row.raw; return row; }
    row.type = (*parts)[0];
    if (lowerCopy(row.type) == "match" && parts->size() == 2 && !(*parts)[1].empty()) {
        row.terminal = row.editable = true; row.target = (*parts)[1];
    } else if (parts->size() >= 3) {
        row.pattern = (*parts)[1]; row.target = (*parts)[2];
        const std::map<std::string, std::string> kinds{{"domain", "domain"},
            {"domain-suffix", "domain_suffix"}, {"ip-cidr", "ip_cidr"},
            {"ip-cidr6", "ip_cidr"}, {"domain-keyword", "domain_keyword"},
            {"domain-regex", "domain_regex"}, {"process-name", "process_name"}};
        const auto kind = kinds.find(lowerCopy(row.type));
        if (kind != kinds.end() && parts->size() == 3) {
            row.type = kind->second; row.editable = true;
        }
    }
    return row;
}

std::vector<ProfileRule> clashProfileRules(const YAML::Node& root) {
    std::vector<ProfileRule> rows;
    if (const auto rules = root["rules"]; rules && rules.IsSequence()) {
        for (const auto& rule : rules) {
            if (rule.IsScalar()) rows.push_back(clashProfileRule(rule.Scalar()));
            else rows.push_back({.anchor = "clash:" + YAML::Dump(rule), .raw = YAML::Dump(rule), .type = "YAML"});
        }
    }
    // This row represents the existing implicit default, not a synthetic MATCH.
    if (!std::ranges::any_of(rows, [](const auto& row) { return row.terminal; })) {
        std::string target = "DIRECT";
        if (const auto groups = root["proxy-groups"]; groups && groups.IsSequence()) {
            for (const auto& group : groups) {
                if (ytext(group, "type") == "select") { target = ytext(group, "name"); break; }
            }
        }
        rows.push_back({.anchor = "implicit:final", .type = "MATCH", .target = target,
            .editable = true, .terminal = true, .implicit = true});
    }
    return rows;
}

std::vector<ProfileRule> nativeProfileRules(const nlohmann::json& config) {
    std::vector<ProfileRule> rows;
    if (config.contains("route") && config["route"].contains("rules")) {
        const auto& rules = config["route"]["rules"];
        if (!rules.is_array()) throw std::runtime_error("route.rules 必须为数组");
        for (const auto& rule : rules) {
            ProfileRule row{.anchor = "json:" + rule.dump(), .raw = rule.dump(), .type = "JSON"};
            if (rule.is_object()) {
                row.target = rule.value("outbound", "");
                if (rule.value("action", "") == "reject")
                    row.target = rule.value("method", "") == "drop" ? "REJECT-DROP" : "REJECT";
                std::size_t conditions = 0;
                bool simple = !row.target.empty();
                for (auto it = rule.begin(); it != rule.end(); ++it) {
                    const auto& key = it.key();
                    const auto& value = it.value();
                    if (key == "outbound" || key == "action" || key == "method") continue;
                    ++conditions;
                    if (key != "rule_set" && validPersonalMatch(key) && value.is_array() && value.size() == 1 && value[0].is_string()) {
                        row.type = key; row.pattern = value[0].get<std::string>();
                    } else simple = false;
                }
                const auto action = rule.value("action", "route");
                if (simple && conditions == 0 && (action == "route" || action == "reject")) row.position = "catch_all";
                row.editable = simple && conditions == 1 && (action == "route" || action == "reject") &&
                    (!rule.contains("method") || (action == "reject" && rule["method"] == "drop"));
            }
            rows.push_back(std::move(row));
        }
    }
    std::string target;
    if (config.contains("route")) target = config["route"].value("final", "");
    if (target.empty() && config.contains("outbounds") && config["outbounds"].is_array() && !config["outbounds"].empty())
        target = config["outbounds"][0].value("tag", "");
    rows.push_back({.anchor = "json:final:" + target, .type = "MATCH", .target = target,
        .editable = true, .terminal = true, .implicit = true});
    return rows;
}

ProfileRules personalRulePlan(std::vector<ProfileRule> rows, const ProfileRuleEdits& edits) {
    ProfileRules result;
    if (!ValidateProfileRuleEdits(edits, result.error)) return result;
    std::set<std::string> anchors;
    for (const auto& edit : edits.rules) {
        if (edit.operation != "replace" && edit.operation != "disable") continue;
        if (!edit.enabled) continue;
        const auto count = std::ranges::count(rows, edit.anchor, &ProfileRule::anchor);
        if (count != 1 || !anchors.insert(edit.anchor).second) {
            result.error = "订阅规则修改冲突，原规则已变化、重复或消失：" + edit.id; return result;
        }
        auto& row = *std::ranges::find(rows, edit.anchor, &ProfileRule::anchor);
        if (!row.editable || row.terminal) {
            result.error = "此规则仅支持查看；兜底请使用默认出口修改"; return result;
        }
        row.editId = edit.id;
        if (edit.operation == "disable") { row.enabled = false; row.origin = "disabled"; }
        else {
            row.type = edit.match; row.pattern = edit.pattern; row.target = edit.target;
            row.origin = "replaced";
        }
    }
    std::vector<ProfileRule> head, tail;
    for (const auto& edit : edits.rules) {
        if (edit.operation == "replace" || edit.operation == "disable") continue;
        ProfileRule row{.type = edit.match, .pattern = edit.pattern, .target = edit.target,
            .editId = edit.id, .origin = "added", .position = edit.operation,
            .editable = true, .enabled = edit.enabled};
        if (edit.operation == "final") {
            if (!edit.enabled) continue;
            auto found = std::ranges::find_if(rows, [](const auto& item) { return item.terminal; });
            if (found == rows.end()) { result.error = "订阅缺少默认出口"; return result; }
            found->target = edit.target; found->origin = "replaced"; found->editId = edit.id;
        } else if (edit.operation == "prepend") head.push_back(std::move(row));
        else tail.push_back(std::move(row));
    }
    result.rules = std::move(head);
    bool inserted = false;
    for (auto& row : rows) {
        if ((row.terminal || row.position == "catch_all") && !inserted) {
            for (auto& extra : tail) result.rules.push_back(std::move(extra));
            inserted = true;
        }
        result.rules.push_back(std::move(row));
    }
    return result;
}

std::string personalTarget(Context& ctx, const ProfileRule& row) {
    if (row.target == "DIRECT" && DetectProfileFormat(ctx.opt.profileYaml) == ProfileFormat::SingBoxJson) {
        for (const auto& outbound : ctx.outbounds)
            if (outbound.value("type", "") == "direct" && !outbound.contains("bind_interface") && !outbound.contains("detour"))
                return outbound.value("tag", "");
        const auto tag = unusedOutboundTag(ctx.config, "__clashflux_personal_direct");
        ctx.outbounds.push_back({{"type", "direct"}, {"tag", tag}});
        ctx.knownTags.push_back(tag);
        return tag;
    }
    if (row.target == "DIRECT" || row.target == "REJECT" || row.target == "REJECT-DROP" || ctx.tagKnown(row.target))
        return row.target;
    const auto edit = std::ranges::find(ctx.opt.profileRuleEdits.rules, row.editId, &ProfileRuleEdit::id);
    const auto fallback = edit == ctx.opt.profileRuleEdits.rules.end() ? "reject" : edit->unavailable;
    ctx.note(FidelityScope::Rule, Fidelity::Approx, row.editId,
        "个人规则目标不可用：" + row.target + "；执行 " + fallback,
        "重新选择订阅中的目标对象，或恢复订阅原规则");
    // main uses the source's unmodified default, never re-runs its rule chain.
    if (fallback == "main" && !row.terminal && !ctx.finalTarget.empty()) return ctx.finalTarget;
    if (fallback == "direct") { auto direct = row; direct.target = "DIRECT"; return personalTarget(ctx, direct); }
    return "REJECT";
}

bool emitPersonalRule(Context& ctx, const ProfileRule& row) {
    const auto target = personalTarget(ctx, row);
    if (row.terminal) { ctx.finalTarget = target; return true; }
    nlohmann::json rule;
    if (row.type == "rule_set") {
        rule["rule_set"] = {personalRuleSetTag(ctx.opt.ruleSetLibrary, row.pattern)};
    } else if (row.type == "domain" || row.type == "domain_suffix") {
        const auto host = vpn::NormalizeRuleDomain(row.pattern);
        if (!host) { ctx.result.error = "个人规则域名无效：" + row.pattern; return false; }
        rule[row.type] = {*host};
    } else if (row.type == "ip_cidr") {
        const auto cidr = addressCidr(row.pattern, row.pattern.find('/') != std::string::npos);
        if (!cidr) { ctx.result.error = "个人规则 IP/CIDR 无效：" + row.pattern; return false; }
        rule["ip_cidr"] = {*cidr};
    } else rule[row.type] = {row.pattern};
    if (target == "REJECT" || target == "REJECT-DROP") rule.update(rejectRule(target));
    else rule["outbound"] = target;
    ctx.config["route"]["rules"].push_back(std::move(rule));
    return true;
}

bool compilePersonalClashRules(Context& ctx, const YAML::Node& root) {
    const auto originals = clashProfileRules(root);
    auto plan = personalRulePlan(originals, ctx.opt.profileRuleEdits);
    if (!plan.error.empty()) { ctx.result.error = std::move(plan.error); return false; }
    // Resolve fallback from the untouched source even when a new rule precedes it.
    for (const auto& original : originals) if (original.terminal) { ctx.finalTarget = original.target; break; }
    for (const auto& row : plan.rules) {
        if (!row.enabled) continue;
        if (row.origin == "source") {
            if (row.implicit) break;
            if (!convertRule(ctx, row.raw)) break;
        } else {
            if (!emitPersonalRule(ctx, row)) return false;
            if (row.terminal) break;
        }
    }
    return true;
}

bool compilePersonalNativeRules(Context& ctx) {
    auto plan = personalRulePlan(nativeProfileRules(ctx.config), ctx.opt.profileRuleEdits);
    if (!plan.error.empty()) { ctx.result.error = std::move(plan.error); return false; }
    ctx.finalTarget = ctx.config["route"].value("final", "");
    ctx.config["route"]["rules"] = nlohmann::json::array();
    for (const auto& row : plan.rules) {
        if (!row.enabled) continue;
        if (row.terminal) {
            if (row.origin != "source") {
                const auto target = personalTarget(ctx, row);
                ctx.finalTarget = target;
                if (target == "REJECT" || target == "REJECT-DROP") {
                    ctx.config["route"]["rules"].push_back(rejectRule(target));
                } else ctx.config["route"]["final"] = target;
            }
        } else if (row.origin == "source") ctx.config["route"]["rules"].push_back(nlohmann::json::parse(row.raw));
        else if (!emitPersonalRule(ctx, row)) return false;
    }
    return true;
}

// Inject after source namespacing. These IDs belong to the shared application
// library, never to a subscription's provider namespace.
bool compilePersonalRuleSets(Context& ctx) {
    std::set<std::string> wanted;
    std::string resourceResolver;
    for (const auto& edit : ctx.opt.profileRuleEdits.rules)
        if (edit.enabled && edit.operation != "disable" && edit.operation != "final" && edit.match == "rule_set")
            wanted.insert(edit.pattern);
    for (const auto& rule : ctx.opt.globalRules)
        if (rule.enabled && rule.match == vpn::MatchKind::RuleSet) wanted.insert(rule.pattern);
    if (ctx.opt.validateAllPersonalRuleSets)
        for (const auto& resource : ctx.opt.ruleSetLibrary.resources) wanted.insert(resource.id);
    for (const auto& id : wanted) {
        const auto found = std::ranges::find(ctx.opt.ruleSetLibrary.resources, id, &PersonalRuleSet::id);
        if (found == ctx.opt.ruleSetLibrary.resources.end()) {
            ctx.result.error = "个人规则引用的规则集已失效：" + id;
            ctx.note(FidelityScope::Rule, Fidelity::Unsupported, id, ctx.result.error, "重新选择规则集；不会删除匹配条件");
            return false;
        }
        auto& sets = ctx.config["route"]["rule_set"];
        if (sets.is_null()) sets = nlohmann::json::array();
        if (!sets.is_array()) { ctx.result.error = "route.rule_set 必须为数组"; return false; }
        const auto tag = personalRuleSetTag(ctx.opt.ruleSetLibrary, id);
        for (const auto& set : sets) {
            if (!set.contains("tag")) continue;
            const auto& existing = set["tag"];
            if (existing == tag || (existing.is_array() && std::ranges::find(existing, nlohmann::json(tag)) != existing.end())) {
                ctx.result.error = "个人规则集与订阅资源标识冲突：" + found->name; return false;
            }
        }
        if (found->type == "inline") {
            const auto document = nlohmann::json::parse(found->content);
            sets.push_back({{"type", "inline"}, {"tag", tag}, {"rules", document["rules"]}});
        } else {
            // Explicit direct HTTP client avoids using the very proxy whose
            // startup is waiting for this resource. TLS uses system trust.
            if (resourceResolver.empty()) {
                auto& servers = ctx.config["dns"]["servers"];
                if (!servers.is_array()) { ctx.result.error = "dns.servers 必须为数组"; return false; }
                for (const auto& server : servers)
                    if (server.value("type", "") == "local" && !server.contains("detour")) {
                        resourceResolver = server.value("tag", "");
                        if (!resourceResolver.empty()) break;
                    }
                if (resourceResolver.empty()) {
                    // Preserve the old single-server implicit resolver when
                    // adding a dedicated bootstrap resolver to native JSON.
                    if (servers.size() == 1 && !ctx.config["route"].contains("default_domain_resolver"))
                        ctx.config["route"]["default_domain_resolver"] = {{"server", servers[0].value("tag", "")}};
                    resourceResolver = "cf_personal_resource_dns";
                    while (std::ranges::any_of(servers, [&](const auto& server) { return server.value("tag", "") == resourceResolver; }))
                        resourceResolver += "_";
                    servers.push_back({{"type", "local"}, {"tag", resourceResolver}});
                }
            }
            sets.push_back({{"type", "remote"}, {"tag", tag}, {"format", found->format},
                {"url", found->url}, {"update_interval", std::to_string(found->update_hours) + "h"},
                {"http_client", {{"engine", "go"}, {"domain_resolver", resourceResolver}}}});
            // applyManagedSkeleton already enables the existing native cache.
        }
    }
    return true;
}
