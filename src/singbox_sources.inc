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

// A portable .ovpn exports one stable endpoint. The original document stays
// in the source file, so refresh/edit/export use the ordinary profile path.
bool compileOpenVpnSource(Context& ctx, std::string_view text,
                          const std::string& source, const std::string& name) {
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    const auto tag = source.empty() ? "OpenVPN" : sourceTag(source, 'e', "OpenVPN");
    const auto fail = [&](std::string error) {
        ctx.result.error = std::move(error);
        ctx.note(FidelityScope::Node, Fidelity::Unsupported, "OpenVPN", ctx.result.error,
                 "修正 .ovpn 配置；不支持的字段不会被静默省略");
        return false;
    };
    if (text.size() > 20000 || text.find('\0') != std::string_view::npos)
        return fail("OpenVPN 配置包含 NUL 或超过 20 KiB");
    std::string error;
    const auto input = parseOpenVpnEndpointInput(text, error);
    if (!input) return fail(std::move(error));
    if (input->connectionBlocks) return fail("OpenVPN connection 分块内的独立选项尚未转换");
    if (input->unsupportedRemoteOptions) return fail("OpenVPN remote 的协议约束或附加参数尚未转换");
    static const std::set<std::string> mapped{
        "remote", "proto", "remote-random", "ca", "cert", "key", "secret",
        "tls-auth", "tls-crypt", "tls-crypt-v2", "key-direction", "peer-fingerprint",
        "remote-cert-tls", "verify-x509-name", "tls-version-min", "tls-version-max",
        "tls-cipher", "auth-user-pass", "data-ciphers", "data-ciphers-fallback",
        "auth", "cipher", "comp-lzo", "compress", "allow-compression", "topology",
        "mssfix", "fragment", "ping", "ping-restart", "reneg-sec", "explicit-exit-notify"};
    static const std::set<std::string> managed{
        "client", "tls-client", "dev", "nobind", "persist-key", "persist-tun",
        "resolv-retry", "verb", "mute", "connect-retry", "connect-retry-max", "connect-timeout"};
    static const std::set<std::string> blocks{
        "ca", "cert", "key", "secret", "tls-auth", "tls-crypt", "tls-crypt-v2", "auth-user-pass"};
    for (const auto* key : {"ca", "cert", "key", "secret", "tls-auth", "tls-crypt", "tls-crypt-v2", "auth-user-pass"}) {
        const auto it = input->options.find(key);
        if (it != input->options.end() && !it->second.empty() && it->second != "[inline]")
            return fail(std::string("OpenVPN 外部 ") + key + " 文件或附加参数尚未转换；请使用 inline 配置");
    }
    for (const auto* key : {"mssfix", "fragment", "ping", "ping-restart", "reneg-sec", "explicit-exit-notify"}) {
        const auto it = input->options.find(key);
        if (it == input->options.end()) continue;
        int value = 0;
        const auto [end, ec] = std::from_chars(it->second.data(), it->second.data() + it->second.size(), value);
        if (ec != std::errc() || end != it->second.data() + it->second.size() || value < 0)
            return fail(std::string("OpenVPN ") + key + " 参数无效或尚未转换");
    }
    if (input->options.contains("remote-cert-tls") && input->options.at("remote-cert-tls") != "server" &&
        input->options.at("remote-cert-tls") != "client") return fail("OpenVPN remote-cert-tls 参数无效");
    if (input->options.contains("proto")) {
        const auto& proto = input->options.at("proto");
        if (proto != "udp" && proto != "tcp" && proto != "tcp-client")
            return fail("OpenVPN proto 参数无效或尚未转换");
    }
    for (const auto& [key, value] : input->options) {
        if (mapped.contains(key)) {
            if (value.empty() && key != "remote-random" && !blocks.contains(key))
                return fail("OpenVPN 指令缺少参数：" + key);
            if (key == "remote-random" && !value.empty()) return fail("OpenVPN remote-random 参数无效");
            continue;
        }
        if (!managed.contains(key)) return fail("OpenVPN 指令尚未转换：" + key);
        if (key == "dev" && value != "tun") return fail("OpenVPN 托管 endpoint 只支持 dev tun");
        ctx.note(FidelityScope::Field, Fidelity::Approx, key,
                 "OpenVPN 的 " + key + " 由 sing-box 用户态 endpoint 生命周期管理，原 CLI 行为不逐项保留");
    }
    for (const auto& [key, value] : input->inlineBlocks)
        if (!blocks.contains(key))
            return fail("OpenVPN inline 配置块尚未转换：" + key);
    if (input->options.contains("auth-user-pass") && !input->inlineBlocks.contains("auth-user-pass"))
        return fail("OpenVPN auth-user-pass 需要 inline 用户名和密码；不支持交互输入或外部文件");
    NativeConnection native{.id = source, .kind = vpn::ConnectionKind::OpenVpn, .nativeConfig = std::string(text)};
    std::optional<nlohmann::json> endpoint;
    try { endpoint = compileOpenVpnEndpoint(native, tag, error); }
    catch (...) { return fail("OpenVPN 指令参数无效"); }
    if (!endpoint) return fail(std::move(error));
    if (ctx.tagKnown(tag)) return fail("OpenVPN endpoint tag 与主配置冲突");
    if (!ctx.config.contains("endpoints")) ctx.config["endpoints"] = nlohmann::json::array();
    for (const auto& item : ctx.config["endpoints"])
        if (item.value("tag", "") == tag) return fail("OpenVPN endpoint tag 重复");
    ctx.config["endpoints"].push_back(std::move(*endpoint));
    ctx.knownTags.push_back(tag);
    ctx.finalTarget = tag;
    if (!source.empty()) ctx.result.sourceObjects.push_back({source, name, vpn::TargetKind::Node, "OpenVPN", tag});
    return true;
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
        if (DetectProfileFormat(source.content) == ProfileFormat::OpenVpn) {
            const auto previousSource = ctx.sourceId;
            const auto previousFinal = ctx.finalTarget;
            ctx.sourceId = source.id;
            const bool ok = compileOpenVpnSource(ctx, source.content, source.id, source.name);
            const auto tag = ctx.finalTarget;
            ctx.sourceId = previousSource;
            ctx.finalTarget = previousFinal;
            if (!ok) return false;
            ctx.sourceDefaults[source.id] = {tag};
            ctx.result.participatingSources.push_back(source.id);
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
