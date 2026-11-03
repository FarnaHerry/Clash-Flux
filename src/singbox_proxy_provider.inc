struct ProxyProviderSet {
    std::vector<std::string> names;
    std::map<std::string, std::vector<std::string>> members;
    std::map<std::string, std::size_t> resources;
    std::map<std::string, std::string> healthCheckUrls;
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
    resource.filter = ytext(provider, "filter");
    resource.excludeFilter = ytext(provider, "exclude-filter");
    resource.excludeType = ytext(provider, "exclude-type");
    auto identity = nlohmann::json::array({1, resource.sourceId, name, resource.url,
        resource.cachePathHint, resource.headers, resource.maxBytes, resource.intervalSeconds,
        resource.filter, resource.excludeFilter, resource.excludeType});
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
        std::string& failure, const HttpProxyProviderResource* declaration = nullptr) {
    try {
        ProxyTextFilters filters;
        if (declaration && !compileProxyTextFilters(declaration->filter,
                declaration->excludeFilter, declaration->excludeType, filters, failure)) return false;
        YAML::Node root;
        if (nodeSubscriptionText(content)) {
            root = loadClashProfileDocument(content);
        } else {
            const auto documents = YAML::LoadAll(content);
            if (documents.size() != 1) {
                failure = "代理集合必须为单个文档";
                return false;
            }
            root = documents.front();
        }
        if (!root.IsMap() || !root["proxies"] || !root["proxies"].IsSequence() ||
            root["proxies"].size() == 0 || root["proxies"].size() > 4096) {
            failure = "代理集合必须包含 1–4096 个 proxies 节点";
            return false;
        }
        unsigned declarations = 0;
        for (const auto& field : root)
            if (field.first.IsScalar() && field.first.Scalar() == "proxies" && ++declarations > 1) {
                failure = "代理集合含重复 proxies 字段";
                return false;
            }
        std::vector<YAML::Node> eligible;
        std::vector<std::string> names;
        for (const auto& proxy : root["proxies"]) {
            if (!proxy.IsMap()) {
                failure = "代理集合包含非对象节点";
                return false;
            }
            const auto type = lowerCopy(ytext(proxy, "type"));
            const auto name = ytext(proxy, "name");
            if (filters.excludeTypes.contains(type) || proxyFilterExcluded(filters, name)) continue;
            if (name.size() > 1024 && (!filters.include.empty() || !filters.exclude.empty())) {
                failure = "筛选节点名称超过 1024 字节";
                return false;
            }
            eligible.push_back(proxy);
            names.push_back(name);
        }
        std::vector<std::size_t> selected;
        if (filters.include.empty()) {
            for (std::size_t index = 0; index < eligible.size(); ++index) selected.push_back(index);
        } else {
            selected = proxyFilterUnion(filters.include, names, [](std::size_t) { return true; });
            if (selected.empty()) {
                failure = "筛选没有匹配任何节点";
                return false;
            }
        }
        for (const auto index : selected) {
            if (ctx.cancelled()) return false;
            auto converted = convertProxy(ctx, eligible[index]);
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
                     "支持 HTTP/file/inline 代理集合及 select/url-test 组；HTTP 下载仅支持 DIRECT，启用的 provider 健康检查仍不支持");
            ctx.result.error = detail;
            return std::optional<ProxyProviderSet>{};
        };
        if (name.empty() || nameCounts[name] != 1 || !entry.second.IsMap())
            return reject("代理集合名称无效、重复或声明不是映射：" + name);
        const auto provider = entry.second;
        const auto type = lowerCopy(ytext(provider, "type"));
        if (type != "http" && type != "file" && type != "inline")
            return reject("代理集合「" + name + "」仅支持 type: http/file/inline");
        bool fieldsValid = true;
        std::set<std::string> fields;
        for (const auto& field : provider) {
            if (!field.first.IsScalar()) { fieldsValid = false; break; }
            const auto key = field.first.Scalar();
            if (!fields.insert(key).second) { fieldsValid = false; break; }
            if (key == "type" || (type == "file" && key == "path") ||
                (type == "http" && (key == "url" || key == "path" || key == "interval" ||
                    key == "proxy" || key == "size-limit" || key == "header"))) continue;
            const auto value = field.second;
            if ((key == "filter" || key == "exclude-filter" || key == "exclude-type") &&
                value.IsScalar()) continue;
            if (key == "override" && value.IsMap() && value.size() == 0) continue;
            if (key == "payload" && value.IsSequence() &&
                (type == "inline" || value.size() == 0)) continue;
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
        if (const auto health = provider["health-check"]; health) {
            std::set<std::string> keys;
            for (const auto& field : health) {
                if (!field.first.IsScalar() || !keys.insert(field.first.Scalar()).second)
                    return reject("代理集合「" + name + "」health-check 含重复或无效字段");
                const auto key = field.first.Scalar();
                if (key != "enable" && key != "url" && key != "interval" && key != "timeout" &&
                    key != "lazy" && key != "expected-status")
                    return reject("代理集合「" + name + "」health-check 含未知字段");
            }
            if (const auto url = health["url"]; url) {
                if (!url.IsScalar() || (!url.Scalar().empty() &&
                    !clashflux::profile_link::IsHttpUrl(url.Scalar())))
                    return reject("代理集合「" + name + "」health-check.url 必须为 HTTP(S) 地址");
                compiled.healthCheckUrls[name] = url.Scalar();
            }
        }
        compiled.names.push_back(name);
        if (type != "http") {
            std::optional<std::string> content;
            if (type == "file") {
                content = readRuleProviderFile(ctx, provider, failure);
            } else {
                const auto payload = provider["payload"];
                if (!payload || !payload.IsSequence() || payload.size() == 0 || payload.size() > 4096)
                    return reject("代理集合「" + name + "」payload 必须含 1–4096 个节点");
                YAML::Node document;
                document["proxies"] = payload;
                content = YAML::Dump(document);
            }
            if (!content) return reject("代理集合「" + name + "」：" + failure);
            if (content->size() > 8 * 1024 * 1024)
                return reject("代理集合「" + name + "」超过 8 MiB 上限");
            HttpProxyProviderResource filters;
            filters.filter = ytext(provider, "filter");
            filters.excludeFilter = ytext(provider, "exclude-filter");
            filters.excludeType = ytext(provider, "exclude-type");
            std::vector<std::string> members;
            if (!appendProxyProviderNodes(ctx, *content, name, members, failure, &filters) || members.empty())
                return reject("代理集合「" + name + "」：" + (failure.empty() ? "筛选后没有节点" : failure));
            compiled.members[name] = std::move(members);
            if (type == "file")
                ctx.note(FidelityScope::Field, Fidelity::Approx, "proxy-providers." + name,
                         "本地代理集合按显式文件根只读为静态快照，不监听或写回文件",
                         "文件变化后重新应用配置；读取失败拒绝整份候选并保留旧运行计划");
            continue;
        }
        auto resource = httpProxyProviderResource(ctx, provider, name, failure);
        if (!resource) return reject("代理集合「" + name + "」：" + failure);
        resource->required = resource->cacheKey == ctx.opt.refreshProviderKey;
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
        if (!appendProxyProviderNodes(ctx, *content, name, members, failure, &*resource))
            return reject("代理集合「" + name + "」：" + failure);
        compiled.members[name] = std::move(members);
        if (supplied == ctx.opt.proxyProviderContents.end())
            ctx.result.proxyProviderContents.try_emplace(resource->cacheKey, *content);
        ctx.note(FidelityScope::Field, Fidelity::Approx, "proxy-providers." + name,
                 "HTTP 代理集合使用 DIRECT 下载和应用自有缓存，节点编译为静态快照；interval 由应用资源调度器独立检查，配置交接可能重启内核；path 不读写，默认上限 8 MiB",
                 "手动刷新主订阅会重新下载引用中的代理集合；启动时到期刷新失败可沿用已验证缓存");
    }
    return compiled;
}

bool filterProxyGroupMembers(Context& ctx, const ProxyTextFilters& filters,
        const std::vector<std::string>& selected, const ProxyProviderSet& providers,
        std::vector<std::string>& output) {
    for (const auto& provider : selected) {
        const auto& names = providers.members.at(provider);
        if (filters.include.empty()) {
            output.insert(output.end(), names.begin(), names.end());
            continue;
        }
        std::set<std::string> seen;
        for (const auto& pattern : filters.include) {
            for (const auto& name : names) {
                if (seen.contains(name) || !proxyFilterMatch(pattern, name)) continue;
                seen.insert(name);
                output.push_back(name);
            }
        }
    }
    if (selected.size() > 1 && filters.include.size() > 1) {
        std::vector<std::string> ordered;
        std::set<std::string> seen;
        for (const auto& pattern : filters.include) {
            for (const auto& name : output) {
                if (seen.contains(name) || !proxyFilterMatch(pattern, name)) continue;
                seen.insert(name);
                ordered.push_back(name);
            }
        }
        for (const auto& name : output) {
            if (seen.insert(name).second) ordered.push_back(name);
        }
        output = std::move(ordered);
    }
    std::erase_if(output, [&](const std::string& name) {
        if (proxyFilterExcluded(filters, name)) return true;
        const auto outbound = std::ranges::find_if(ctx.outbounds, [&](const auto& item) {
            return item.value("tag", "") == name;
        });
        return outbound != ctx.outbounds.end() &&
            filters.excludeTypes.contains(proxyGroupType(outbound->value("type", "")));
    });
    return true;
}

bool expandProxyProviderGroup(Context& ctx, const YAML::Node& group,
        const std::string& name, const ProxyProviderSet& providers,
        std::vector<std::string>& members, std::string& testUrl) {
    const bool includeAll = ybool(group, "include-all-providers");
    const auto use = group["use"];
    const bool uses = use && (!use.IsSequence() || use.size() != 0);
    if (ybool(group, "include-all")) {
        ctx.result.error = "代理组「" + name + "」的 include-all 尚未支持";
        return false;
    }
    if (!includeAll && !uses) return true;
    const auto type = lowerCopy(ytext(group, "type"));
    if (type != "select" && type != "url-test") {
        ctx.result.error = "代理组「" + name + "」使用 provider 时仅支持 select/url-test";
        return false;
    }
    for (const auto* field : {"filter", "exclude-filter", "exclude-type"}) {
        const auto value = group[field];
        if (value && !value.IsScalar()) {
            ctx.result.error = "代理组「" + name + "」的筛选必须为字符串";
            return false;
        }
    }
    ProxyTextFilters filters;
    if (!compileProxyTextFilters(ytext(group, "filter"), ytext(group, "exclude-filter"),
            ytext(group, "exclude-type"), filters, ctx.result.error)) return false;
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
    std::vector<std::string> expandedProviders;
    std::set<std::string> seen;
    for (const auto& provider : selected) {
        if (!seen.insert(provider).second) continue;
        if (std::ranges::find(providers.names, provider) == providers.names.end()) {
            ctx.result.error = "代理组「" + name + "」引用未知代理集合「" + provider + "」";
            return false;
        }
        if (const auto resource = providers.resources.find(provider); resource != providers.resources.end())
            ctx.result.httpProxyProviders[resource->second].required = true;
        if (!providers.members.contains(provider)) {
            ctx.result.error = "HTTP 代理集合「" + provider + "」尚未准备好";
            return false;
        }
        expandedProviders.push_back(provider);
    }
    if (expandedProviders.empty()) {
        ctx.result.error = "代理组「" + name + "」没有可展开的代理集合";
        return false;
    }
    if (type == "url-test") {
        const auto reject = [&](std::string failure) {
            ctx.result.error = std::move(failure);
            ctx.note(FidelityScope::Group, Fidelity::Unsupported, name, ctx.result.error,
                     "修正测速参数；保留旧订阅和运行配置，不提交部分成员");
            return false;
        };
        if (const auto url = group["url"]; url && !url.IsScalar()) {
            return reject("provider 测速组 url 必须为字符串");
        }
        testUrl = ytext(group, "url");
        if (testUrl.empty()) {
            for (const auto& provider : expandedProviders) {
                const auto url = providers.healthCheckUrls.find(provider);
                if (url != providers.healthCheckUrls.end() && !url->second.empty()) {
                    testUrl = url->second;
                    break;
                }
            }
        }
        if (testUrl.empty()) testUrl = "https://www.gstatic.com/generate_204";
        if (!clashflux::profile_link::IsHttpUrl(testUrl)) {
            return reject("provider 测速组 url 必须为 HTTP(S) 地址");
        }
        if (const auto interval = group["interval"]; interval) {
            unsigned seconds = 0;
            const auto text = interval.IsScalar() ? interval.Scalar() : "";
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), seconds);
            if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
                seconds > 2147483647U) {
                return reject("provider 测速组 interval 必须为支持范围内的非负整数");
            }
        }
        ctx.note(FidelityScope::Group, Fidelity::Approx, name,
                 "代理集合静态展开为原生 urltest，测速调度和成功判定遵循 sing-box；未设或为 0 的 interval 使用 300 秒，不提供独立 provider 健康检查",
                 "组 URL 优先，否则继承首个 provider 检查 URL；provider 更新需重新应用配置");
    }
    std::vector<std::string> providerMembers;
    filterProxyGroupMembers(ctx, filters, expandedProviders, providers, providerMembers);
    if (providerMembers.empty()) {
        ctx.result.error = "代理组「" + name + "」筛选后没有可用 provider 节点";
        return false;
    }
    members.insert(members.end(), providerMembers.begin(), providerMembers.end());
    return true;
}
