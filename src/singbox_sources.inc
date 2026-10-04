// L3 来源导出与隔离。仅改写本项目从 Clash 生成的已知字段；原生 JSON 不猜引用。
std::string sourceTag(std::string_view source, char kind, std::string_view name) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string result = "cf_";
    const auto append = [&](std::string_view value) {
        for (unsigned char c : value) { result += hex[c >> 4]; result += hex[c & 15]; }
    };
    append(source);
    result += '_'; result += kind; result += '_'; append(name);
    return result;
}

void collectSourceObjects(Context& ctx, const std::string& source, const std::string& name) {
    if (source.empty()) return;
    const auto& outputs = ctx.outbounds;
    for (const auto& item : outputs) {
        const auto tag = item.value("tag", "");
        const auto type = item.value("type", "");
        ctx.result.sourceObjects.push_back({source, name,
            type == "selector" || type == "urltest" ? vpn::TargetKind::Group : vpn::TargetKind::Node,
            tag, tag});
    }
}

void namespaceClashSource(Context& ctx, const std::string& source, const std::string& name) {
    collectSourceObjects(ctx, source, name);
    std::map<std::string, std::string> outputs, servers, sets;
    for (const auto& item : ctx.outbounds) {
        const auto tag = item.value("tag", "");
        outputs.emplace(tag, sourceTag(source, 'o', tag));
    }
    for (const auto& item : ctx.config["dns"]["servers"]) {
        const auto tag = item.value("tag", "");
        servers.emplace(tag, sourceTag(source, 'd', tag));
    }
    for (const auto& item : ctx.ruleSets) {
        const auto tag = item.value("tag", "");
        sets.emplace(tag, sourceTag(source, 'r', tag));
    }
    const auto rewrite = [](nlohmann::json& value, const auto& map) {
        if (value.is_string()) {
            const auto found = map.find(value.get<std::string>());
            if (found != map.end()) value = found->second;
        } else if (value.is_array()) {
            for (auto& item : value) {
                const auto found = map.find(item.get<std::string>());
                if (found != map.end()) item = found->second;
            }
        }
    };
    if (!ctx.hostsDnsTag.empty()) ctx.hostsDnsTag = servers.at(ctx.hostsDnsTag);
    for (auto& item : ctx.outbounds) {
        rewrite(item["tag"], outputs);
        for (const auto* key : {"detour", "outbounds", "default"})
            if (item.contains(key)) rewrite(item[key], outputs);
        if (item.contains("domain_resolver")) rewrite(item["domain_resolver"]["server"], servers);
    }
    for (auto& item : ctx.config["dns"]["servers"]) {
        rewrite(item["tag"], servers);
        if (item.contains("detour")) rewrite(item["detour"], outputs);
        if (item.contains("domain_resolver")) rewrite(item["domain_resolver"]["server"], servers);
    }
    std::function<void(nlohmann::json&)> rule = [&](auto& item) {
        if (!item.is_object()) return;
        if (item.contains("outbound")) rewrite(item["outbound"], outputs);
        if (item.contains("server")) rewrite(item["server"], servers);
        if (item.contains("rule_set")) rewrite(item["rule_set"], sets);
        if (item.contains("rules")) for (auto& nested : item["rules"]) rule(nested);
    };
    for (auto& item : ctx.config["route"]["rules"]) rule(item);
    if (ctx.config["dns"].contains("rules"))
        for (auto& item : ctx.config["dns"]["rules"]) rule(item);
    if (ctx.config["dns"].contains("final")) rewrite(ctx.config["dns"]["final"], servers);
    if (ctx.config["route"].contains("default_domain_resolver"))
        rewrite(ctx.config["route"]["default_domain_resolver"]["server"], servers);
    if (ctx.config["route"].contains("final")) rewrite(ctx.config["route"]["final"], outputs);
    // ruleSetResources.tag 是自有 GEO 缓存身份，保持原名/文件路径；不是运行 tag。
    for (auto& item : ctx.ruleSets) rewrite(item["tag"], sets);
    for (auto& item : ctx.result.sourceObjects) item.tag = outputs.at(item.objectId);
    ctx.knownTags.clear();
    for (const auto& item : ctx.outbounds) ctx.knownTags.push_back(item.value("tag", ""));
}

bool compileAuxiliarySources(Context& ctx) {
#if defined(__ANDROID__) || defined(CLASHFLUX_IOS)
    if (!ctx.opt.auxiliarySources.empty()) {
        ctx.result.error = "手机平台不支持普通代理多订阅编排";
        return false;
    }
#endif
    std::set<std::string> sourceIds;
    if (!ctx.opt.mainConnectionId.empty()) sourceIds.insert(ctx.opt.mainConnectionId);
    for (const auto& source : ctx.opt.auxiliarySources) {
        if (source.id.empty() || !sourceIds.insert(source.id).second) {
            ctx.result.error = "次来源身份为空、重复或与 main 相同";
            return false;
        }
        const bool referenced = std::ranges::any_of(ctx.opt.globalRules, [&](const auto& rule) {
            return rule.enabled && rule.connectionId == source.id;
        });
        if (!referenced) continue;
        if (!source.available) {
            ctx.warn("次来源「" + source.name + "」文件不可用；保留分流规则并执行其不可用策略");
            continue;
        }
        auto options = ctx.opt;
        options.profileYaml = source.content;
        options.auxiliarySources.clear(); options.globalRules.clear(); options.nativeConnections.clear();
        Context part{options};
        part.sourceId = source.id;
        part.config = nlohmann::json::object();
        applyManagedSkeleton(part, options);
        part.outbounds.push_back({{"type", "direct"}, {"tag", "DIRECT"}});
        part.knownTags.push_back("DIRECT");
        const auto text = trimCopy(source.content);
        if (text.empty() || text.front() == '{') {
            ctx.result.error = "次来源「" + source.name + "」需要可解析的 Clash YAML；原生 JSON 编排尚未开放";
            ctx.sourceId = source.id;
            ctx.note(FidelityScope::Field, Fidelity::Unsupported, "source", ctx.result.error);
            return false;
        }
        try { compileClashDocument(part, text, true); }
        catch (const std::exception& e) { part.result.error = e.what(); }
        ctx.result.fidelity.insert(ctx.result.fidelity.end(), part.result.fidelity.begin(), part.result.fidelity.end());
        ctx.result.warnings.insert(ctx.result.warnings.end(), part.result.warnings.begin(), part.result.warnings.end());
        if (!part.result.error.empty()) {
            ctx.result.error = "次来源「" + source.name + "」编译失败：" + part.result.error;
            return false;
        }
        std::string defaultTag = part.finalTarget;
        const bool reject = defaultTag == "REJECT" || defaultTag == "REJECT-DROP";
        const bool drop = defaultTag == "REJECT-DROP";
        if (defaultTag.empty()) {
            defaultTag = "DIRECT";
            for (const auto& output : part.outbounds) {
                if (output.value("type", "") == "selector") { defaultTag = output.value("tag", ""); break; }
            }
        }
        if (!reject && !std::ranges::any_of(part.outbounds, [&](const auto& item) { return item.value("tag", "") == defaultTag; })) {
            ctx.result.error = "次来源「" + source.name + "」默认目标不存在：" + defaultTag;
            return false;
        }
        namespaceClashSource(part, source.id, source.name);
        defaultTag = reject ? std::string{} : sourceTag(source.id, 'o', defaultTag);
        ctx.sourceDefaults[source.id] = {defaultTag, reject, drop};
        // 仅保留规则实际引用的对象及 detour/组成员/DNS 的依赖闭包。
        std::map<std::string, const nlohmann::json*> outputs, servers;
        for (const auto& item : part.outbounds) outputs[item.value("tag", "")] = &item;
        for (const auto& item : part.config["dns"]["servers"]) servers[item.value("tag", "")] = &item;
        std::set<std::string> neededOutputs, neededServers;
        std::vector<std::pair<bool, std::string>> pending;
        for (const auto& rule : ctx.opt.globalRules) {
            if (!rule.enabled || rule.connectionId != source.id) continue;
            if (rule.targetKind == vpn::TargetKind::Default) {
                if (!reject) pending.emplace_back(false, defaultTag);
            } else {
                for (const auto& object : part.result.sourceObjects)
                    if (object.kind == rule.targetKind && object.objectId == rule.targetObject)
                        pending.emplace_back(false, object.tag);
            }
        }
        while (!pending.empty()) {
            auto [dns, tag] = std::move(pending.back()); pending.pop_back();
            auto& needed = dns ? neededServers : neededOutputs;
            const auto& entries = dns ? servers : outputs;
            if (!entries.contains(tag) || !needed.insert(tag).second) continue;
            const auto& item = *entries.at(tag);
            if (item.contains("detour")) pending.emplace_back(false, item["detour"].get<std::string>());
            if (item.contains("outbounds"))
                for (const auto& member : item["outbounds"]) pending.emplace_back(false, member.get<std::string>());
            if (item.contains("domain_resolver"))
                pending.emplace_back(true, item["domain_resolver"].value("server", ""));
        }
        for (const auto& item : part.outbounds) {
            const auto tag = item.value("tag", "");
            if (!neededOutputs.contains(tag)) continue;
            if (ctx.tagKnown(tag)) { ctx.result.error = "来源命名空间与主配置 tag 冲突：" + tag; return false; }
            ctx.outbounds.push_back(item); ctx.knownTags.push_back(tag);
        }
        for (const auto& item : part.config["dns"]["servers"]) {
            if (!neededServers.contains(item.value("tag", ""))) continue;
            const auto tag = item.value("tag", "");
            for (const auto& existing : ctx.config["dns"]["servers"])
                if (existing.value("tag", "") == tag) { ctx.result.error = "DNS 来源 tag 冲突：" + tag; return false; }
            ctx.config["dns"]["servers"].push_back(item);
        }
        for (const auto& object : part.result.sourceObjects)
            if (neededOutputs.contains(object.tag)) ctx.result.sourceObjects.push_back(object);
        ctx.result.participatingSources.push_back(source.id);
    }
    return true;
}
