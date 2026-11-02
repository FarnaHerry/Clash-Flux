struct ProxyProviderSet {
    std::vector<std::string> names;
    std::map<std::string, std::vector<std::string>> members;
    std::map<std::string, std::size_t> resources;
};

std::optional<HttpProxyProviderResource> httpProxyProviderResource(
        Context& ctx, const YAML::Node& provider, const std::string& name,
        std::string& failure) {
    HttpProxyProviderResource resource;
    resource.name = name;
    resource.sourceId = ctx.sourceId;
    const auto url = provider["url"];
    if (!url || !url.IsScalar() || url.Scalar().size() > 8192 ||
        !clashflux::profile_link::IsHttpUrl(url.Scalar())) {
        failure = "需要有效 HTTP(S) url";
        return std::nullopt;
    }
    resource.url = url.Scalar();
    const auto authorityStart = resource.url.find("://") + 3;
    const auto authorityEnd = resource.url.find_first_of("/?#", authorityStart);
    if (resource.url.substr(authorityStart, authorityEnd - authorityStart).find('@') != std::string::npos) {
        failure = "URL userinfo 鉴权尚未跨平台接入";
        return std::nullopt;
    }
    for (const auto* field : {"interval", "size-limit"}) {
        const auto value = provider[field];
        if (!value) continue;
        std::uint64_t number = 0;
        if (!value.IsScalar()) {
            failure = std::string(field) + " 必须为非负整数";
            return std::nullopt;
        }
        const auto text = value.Scalar();
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
        if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
            failure = std::string(field) + " 必须为非负整数";
            return std::nullopt;
        }
        if (std::string_view(field) == "interval") {
            if (number > 2147483647ULL) {
                failure = "interval 超过支持范围";
                return std::nullopt;
            }
            resource.intervalSeconds = number;
        } else if (number > 0) {
            if (number > resource.maxBytes) {
                failure = "size-limit 超过 8 MiB 支持上限";
                return std::nullopt;
            }
            resource.maxBytes = static_cast<std::size_t>(number);
        }
    }
    if (const auto proxy = provider["proxy"];
        proxy && (!proxy.IsScalar() || proxy.Scalar() != "DIRECT")) {
        failure = "proxy 仅支持显式 DIRECT，不能改写指定代理下载语义";
        return std::nullopt;
    }
    if (const auto path = provider["path"]) {
        if (!path.IsScalar() || path.Scalar().empty() || path.Scalar().size() > 4096 ||
            path.Scalar().find('\0') != std::string::npos) {
            failure = "path 必须是有效的缓存位置提示";
            return std::nullopt;
        }
        resource.cachePathHint = path.Scalar();
    }
    if (const auto headers = provider["header"]; headers) {
        if (!headers.IsMap()) {
            failure = "header 必须为单值字符串数组的映射";
            return std::nullopt;
        }
        for (const auto& entry : headers) {
            if (!entry.first.IsScalar() || !entry.second.IsSequence() ||
                entry.second.size() != 1 || !entry.second[0].IsScalar()) {
                failure = "header 仅支持每项一个字符串";
                return std::nullopt;
            }
            const auto key = clashflux::http_request::LowerHeaderName(entry.first.Scalar());
            if (!resource.headers.emplace(key, entry.second[0].Scalar()).second ||
                !clashflux::http_request::ValidHeaders(resource.headers)) {
                failure = "header 含重复、非法、超限或不支持的传输字段";
                return std::nullopt;
            }
        }
#if defined(CLASHFLUX_IOS)
        if (!resource.headers.empty()) {
            failure = "iOS 请求头下载尚未接入";
            return std::nullopt;
        }
#endif
    }
    auto identity = nlohmann::json::array({1, resource.sourceId, name, resource.url,
        resource.cachePathHint, resource.headers, resource.maxBytes, resource.intervalSeconds});
    resource.identity = identity.dump();
    if (resource.identity.size() > 32 * 1024) {
        failure = "来源身份过长";
        return std::nullopt;
    }
    resource.cacheKey = httpProviderKey(resource.identity);
    return resource;
}

bool appendProxyProviderNodes(Context& ctx, const std::string& content,
        const std::string& providerName, std::vector<std::string>& members,
        std::string& failure) {
    try {
        const auto root = loadClashProfileDocument(content);
        if (!root.IsMap() || !root["proxies"] || !root["proxies"].IsSequence() ||
            root["proxies"].size() == 0 || root["proxies"].size() > 4096) {
            failure = "代理集合必须包含 1–4096 个 proxies 节点";
            return false;
        }
        for (const auto& proxy : root["proxies"]) {
            if (ctx.cancelled()) return false;
            if (!proxy.IsMap()) {
                failure = "代理集合包含非对象节点";
                return false;
            }
            auto converted = convertProxy(ctx, proxy);
            if (!converted) {
                failure = "代理集合包含无法完整转换或重名的节点";
                return false;
            }
            members.push_back(converted->at("tag").get<std::string>());
            ctx.knownTags.push_back(converted->at("tag").get<std::string>());
            ctx.outbounds.push_back(std::move(*converted));
        }
        return true;
    } catch (const std::exception&) {
        failure = "代理集合「" + providerName + "」内容解析失败";
        return false;
    }
}

std::optional<ProxyProviderSet> compileProxyProviders(Context& ctx, const YAML::Node& providers) {
    ProxyProviderSet compiled;
    if (!providers) return compiled;
    if (!providers.IsMap() || providers.size() > 64) {
        ctx.result.error = "proxy-providers 必须为不超过 64 项的映射";
        return std::nullopt;
    }
    std::map<std::string, unsigned> nameCounts;
    for (const auto& entry : providers)
        if (entry.first.IsScalar()) ++nameCounts[entry.first.Scalar()];
    for (const auto& entry : providers) {
        const std::string name = entry.first.IsScalar() ? entry.first.Scalar() : "";
        const auto reject = [&](const std::string& detail) {
            ctx.note(FidelityScope::Field, Fidelity::Unsupported,
                     "proxy-providers." + name, detail,
                     "当前仅支持 HTTP 代理集合、select 组的 use/include-all-providers 与 DIRECT 下载");
            ctx.result.error = detail;
            return std::optional<ProxyProviderSet>{};
        };
        if (name.empty() || nameCounts[name] != 1 || !entry.second.IsMap())
            return reject("代理集合名称无效、重复或声明不是映射：" + name);
        const auto provider = entry.second;
        if (lowerCopy(ytext(provider, "type")) != "http")
            return reject("代理集合「" + name + "」当前仅支持 type: http");
        bool fieldsValid = true;
        std::set<std::string> fields;
        for (const auto& field : provider) {
            if (!field.first.IsScalar()) { fieldsValid = false; break; }
            const auto key = field.first.Scalar();
            if (!fields.insert(key).second) { fieldsValid = false; break; }
            if (key == "type" || key == "url" || key == "path" || key == "interval" ||
                key == "proxy" || key == "size-limit" || key == "header") continue;
            const auto value = field.second;
            if ((key == "filter" || key == "exclude-filter" || key == "exclude-type") &&
                value.IsScalar() && value.Scalar().empty()) continue;
            if (key == "override" && value.IsMap() && value.size() == 0) continue;
            if (key == "payload" && value.IsSequence() && value.size() == 0) continue;
            if (key == "health-check" && value.IsMap() && value["enable"] &&
                value["enable"].IsScalar()) {
                const auto enabled = lowerCopy(value["enable"].Scalar());
                if (enabled == "false" || enabled == "no" || enabled == "0") continue;
            }
            fieldsValid = false;
            break;
        }
        if (!fieldsValid)
            return reject("代理集合「" + name + "」含重复或尚未支持的筛选、健康检查、覆盖、加密或回退字段");
        std::string failure;
        auto resource = httpProxyProviderResource(ctx, provider, name, failure);
        if (!resource) return reject("代理集合「" + name + "」：" + failure);
        compiled.names.push_back(name);
        compiled.resources[name] = ctx.result.httpProxyProviders.size();
        ctx.result.httpProxyProviders.push_back(*resource);
        const auto supplied = ctx.opt.proxyProviderContents.find(resource->cacheKey);
        auto content = supplied != ctx.opt.proxyProviderContents.end()
            ? std::optional<std::string>{supplied->second}
            : ReadHttpProxyProviderCache(*resource, ctx.opt.proxyProviderCacheDir, failure);
        if (!content) continue;
        if (content->size() > resource->maxBytes)
            return reject("代理集合「" + name + "」内容超过 size-limit/8 MiB 上限");
        std::vector<std::string> members;
        if (!appendProxyProviderNodes(ctx, *content, name, members, failure))
            return reject("代理集合「" + name + "」：" + failure);
        compiled.members[name] = std::move(members);
        if (supplied == ctx.opt.proxyProviderContents.end())
            ctx.result.proxyProviderContents.try_emplace(resource->cacheKey, *content);
        ctx.note(FidelityScope::Field, Fidelity::Approx, "proxy-providers." + name,
                 "HTTP 代理集合使用 DIRECT 下载和应用自有缓存，节点编译为静态快照；interval 仅在应用配置时检查，path 不读写，默认上限 8 MiB",
                 "手动刷新主订阅会重新下载引用中的代理集合；启动时到期刷新失败可沿用已验证缓存");
    }
    return compiled;
}

bool expandProxyProviderGroup(Context& ctx, const YAML::Node& group,
        const std::string& name, const ProxyProviderSet& providers,
        std::vector<std::string>& members) {
    const bool includeAll = ybool(group, "include-all-providers");
    const auto use = group["use"];
    const bool uses = use && (!use.IsSequence() || use.size() != 0);
    if (ybool(group, "include-all")) {
        ctx.result.error = "代理组「" + name + "」的 include-all 尚未支持";
        return false;
    }
    if (!includeAll && !uses) return true;
    if (lowerCopy(ytext(group, "type")) != "select") {
        ctx.result.error = "代理组「" + name + "」使用 provider 时当前仅支持 select";
        return false;
    }
    for (const auto* field : {"filter", "exclude-filter", "exclude-type"}) {
        const auto value = group[field];
        if (value && (!value.IsScalar() || !value.Scalar().empty())) {
            ctx.result.error = "代理组「" + name + "」的 provider 筛选尚未支持";
            return false;
        }
    }
    std::vector<std::string> selected;
    if (includeAll) {
        selected = providers.names;
        std::ranges::sort(selected);
    }
    if (uses) {
        if (!use.IsSequence()) {
            ctx.result.error = "代理组「" + name + "」的 use 必须为数组";
            return false;
        }
        for (const auto& item : use) {
            if (!item.IsScalar() || item.Scalar().empty()) {
                ctx.result.error = "代理组「" + name + "」的 use 含无效 provider 名称";
                return false;
            }
            selected.push_back(item.Scalar());
        }
    }
    std::set<std::string> seen;
    for (const auto& provider : selected) {
        if (!seen.insert(provider).second) continue;
        const auto resource = providers.resources.find(provider);
        if (resource == providers.resources.end()) {
            ctx.result.error = "代理组「" + name + "」引用未知代理集合「" + provider + "」";
            return false;
        }
        ctx.result.httpProxyProviders[resource->second].required = true;
        const auto found = providers.members.find(provider);
        if (found == providers.members.end()) {
            ctx.result.error = "HTTP 代理集合「" + provider + "」尚未准备好";
            return false;
        }
        members.insert(members.end(), found->second.begin(), found->second.end());
    }
    if (seen.empty()) {
        ctx.result.error = "代理组「" + name + "」没有可展开的代理集合";
        return false;
    }
    return true;
}
