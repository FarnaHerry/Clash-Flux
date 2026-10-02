// singbox_rules.inc — 规则集、规则匹配与原生连接规则
// 由 src/singbox.cpp 在同一匿名命名空间中包含。

bool ensureGeoipRuleSet(Context& ctx, std::string country) {
    country = lowerCopy(country);
    if (ctx.geoipTags.contains(country)) {
        const std::string tag = "geoip-" + country;
        for (const auto& ruleSet : ctx.ruleSets) {
            if (ruleSet.is_object() && ruleSet.value("tag", "") == tag) {
                return true;
            }
        }
        return false;
    }
    const std::string tag = "geoip-" + country;
    ctx.geoipTags.emplace(country, tag);
    // MetaCubeX 的 geoip 目录只提供国家/地区代码。某些 Clash 订阅会把
    // TELEGRAM 等类别误写成 GEOIP；继续拼出不存在的 URL 会让 sing-box
    // 在启动阶段直接 FATAL，而不是把规则温和地降级。
    if (country.size() != 2 ||
        !std::all_of(country.begin(), country.end(), [](unsigned char c) {
            return c >= 'a' && c <= 'z';
        })) {
        ctx.note(FidelityScope::Rule, Fidelity::Unsupported,
                 std::format("GEOIP,{}", country),
                 std::format("GEOIP 代码「{}」不是国家/地区代码，已跳过", country),
                 "GEOIP 只接受两位国家/地区代码（如 CN、US）；该条规则已丢弃");
        return false;
    }
    ctx.result.ruleSetResources.push_back({tag, BuiltinRuleSetUrl(tag)});
    // 本地缓存命中 → local rule_set（首启不依赖代理/网络；core_store 负责
    // 预取与按周刷新）；未命中 → remote，由内核在启动时经默认出站拉取。
    // 命中前必须校验：0 字节/被写入错误内容的缓存会让内核启动期 FATAL
    // （parse rule-set: read rule: unexpected EOF）。
    if (!ctx.opt.ruleSetDir.empty()) {
        const std::filesystem::path local =
            std::filesystem::path(ctx.opt.ruleSetDir) / (tag + ".srs");
        std::error_code ec;
        if (std::filesystem::exists(local, ec) && !ec) {
            if (RuleSetCacheValid(local)) {
                ctx.ruleSets.push_back({
                    {"type", "local"},
                    {"tag", tag},
                    {"format", "binary"},
                    {"path", local.string()},
                });
                return true;
            }
            ctx.warn(std::format("本地 GEOIP 规则集缓存「{}」不可用，忽略该缓存", tag));
        }
    }
#if defined(__ANDROID__)
    // Android 的远程 GEOIP 规则集会在 VpnService 建立 TUN 的同一阶段下载。
    // 此时 libbox 还没有可用的物理网络接口，下载失败会使整个数据面启动
    // 失败。节点/策略组仍可正常使用，GEOIP 规则只在本地缓存存在时启用。
    ctx.note(FidelityScope::Rule, Fidelity::Unsupported,
             std::format("GEOIP,{}", country),
             std::format("Android 暂无本地 GEOIP 规则集「{}」，已跳过在线下载", country),
             "Android 在建立 TUN 的阶段没有可用物理网络，无法下载规则集；"
             "该条规则已丢弃（节点与策略组不受影响）");
    return false;
#else
    ctx.ruleSets.push_back({
        {"type", "remote"},
        {"tag", tag},
        {"format", "binary"},
        // meta-rules-dat（mihomo geodata 同源）：国家码与 telegram/netflix/
        // cloudflare 等类别全覆盖；SagerNet/sing-geoip 只有国家码，GEOIP,
        // TELEGRAM 这类别会 404 并在启动期 FATAL。
        {"url", BuiltinRuleSetUrl(tag)},
        {"update_interval", "24h"},
    });
    return true;
#endif
}
bool ensureGeositeRuleSet(Context& ctx, std::string category) {
    category = lowerCopy(category);
    if (ctx.geositeTags.contains(category)) {
        const std::string tag = "geosite-" + category;
        return std::ranges::any_of(ctx.ruleSets, [&](const auto& ruleSet) {
            return ruleSet.is_object() && ruleSet.value("tag", "") == tag;
        });
    }
    const std::string tag = "geosite-" + category;
    ctx.geositeTags.emplace(category, tag);
    if (category.empty() ||
        !std::all_of(category.begin(), category.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                   c == '-' || c == '_';
        })) {
        ctx.note(FidelityScope::Rule, Fidelity::Unsupported,
                 std::format("GEOSITE,{}", category),
                 std::format("GEOSITE 类别「{}」无效，已跳过", category),
                 "类别名只能包含 a-z0-9-_（如 cn、geolocation-!cn）；该条规则已丢弃");
        return false;
    }
    ctx.result.ruleSetResources.push_back({tag, BuiltinRuleSetUrl(tag)});
    if (!ctx.opt.ruleSetDir.empty()) {
        const std::filesystem::path local =
            std::filesystem::path(ctx.opt.ruleSetDir) / (tag + ".srs");
        std::error_code ec;
        if (std::filesystem::exists(local, ec) && !ec) {
            if (RuleSetCacheValid(local)) {
                ctx.ruleSets.push_back({
                    {"type", "local"},
                    {"tag", tag},
                    {"format", "binary"},
                    {"path", local.string()},
                });
                return true;
            }
            ctx.warn(std::format("本地 GEOSITE 规则集缓存「{}」不可用，忽略该缓存", tag));
        }
    }
#if defined(__ANDROID__)
    ctx.note(FidelityScope::Rule, Fidelity::Unsupported,
             std::format("GEOSITE,{}", category),
             std::format("Android 暂无本地 GEOSITE 规则集「{}」，已跳过在线下载", category),
             "Android 在建立 TUN 的阶段没有可用物理网络，无法下载规则集；"
             "该条规则已丢弃（节点与策略组不受影响）");
    return false;
#else
    ctx.ruleSets.push_back({
        {"type", "remote"},
        {"tag", tag},
        {"format", "binary"},
        {"url", BuiltinRuleSetUrl(tag)},
        {"update_interval", "24h"},
    });
    return true;
#endif
}

void ensureChinaDnsRule(Context& ctx, const std::string& geositeTag) {
    if (!ctx.config["dns"].contains("rules") ||
        !ctx.config["dns"]["rules"].is_array()) {
        ctx.config["dns"]["rules"] = nlohmann::json::array();
    }
    const bool exists = std::ranges::any_of(ctx.config["dns"]["rules"],
        [&](const auto& rule) {
            return rule.is_object() && rule.value("server", "") == "local" &&
                   rule.value("rule_set", nlohmann::json::array()) ==
                       nlohmann::json::array({geositeTag});
        });
    if (!exists) {
        ctx.config["dns"]["rules"].push_back({
            {"rule_set", {geositeTag}},
            {"action", "route"},
            {"server", "local"},
        });
    }
}

std::optional<std::string> addressCidr(std::string_view raw, bool ipv4Only,
                                     bool requirePrefix);

bool appendRulePorts(nlohmann::json& rule, std::string_view value, bool source) {
    nlohmann::json ports = nlohmann::json::array();
    nlohmann::json ranges = nlohmann::json::array();
    const auto parsePort = [](std::string_view text) -> std::optional<unsigned int> {
        unsigned int port = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), port);
        if (text.empty() || error != std::errc{} || end != text.data() + text.size() ||
            port > 65535) return std::nullopt;
        return port;
    };
    // Clash 规则使用 '/' 分隔多个端口；范围的 '-' 转为 sing-box 的 ':'。
    // 先完整校验，不能保留半条列表（那会静默缩小用户的匹配范围）。
    while (true) {
        const auto separator = value.find('/');
        const std::string piece = trimCopy(value.substr(0, separator));
        const auto dash = piece.find('-');
        if (dash == std::string::npos) {
            const auto port = parsePort(piece);
            if (!port) return false;
            ports.push_back(*port);
        } else {
            const auto first = parsePort(std::string_view(piece).substr(0, dash));
            const auto last = parsePort(std::string_view(piece).substr(dash + 1));
            if (!first || !last || *first > *last) return false;
            ranges.push_back(std::format("{}:{}", *first, *last));
        }
        if (separator == std::string_view::npos) break;
        value.remove_prefix(separator + 1);
    }
    if (!ports.empty()) rule[source ? "source_port" : "port"] = std::move(ports);
    if (!ranges.empty()) rule[source ? "source_port_range" : "port_range"] = std::move(ranges);
    return true;
}

struct RuleCondition {
    nlohmann::json rule = nlohmann::json::object();
    std::string error;
    explicit operator bool() const { return error.empty(); }
};

// Shared match-only lowering. A provider is a HeadlessRule, not a route rule:
// notably user_id and ip_version do not exist in its fixed-version schema.
RuleCondition compileLeafCondition(const std::string& kind, const std::string& value,
                                   bool headless) {
    RuleCondition result;
    auto& rule = result.rule;
    if (value.empty()) { result.error = "匹配值为空"; return result; }
    if (kind == "domain") rule["domain"] = {value};
    else if (kind == "domain-suffix") rule["domain_suffix"] = {value};
    else if (kind == "domain-keyword") rule["domain_keyword"] = {value};
    else if (kind == "domain-regex") rule["domain_regex"] = {value};
    else if (kind == "network") {
        const std::string network = lowerCopy(value);
        if (network != "tcp" && network != "udp") result.error = "NETWORK 只接受 TCP/UDP";
        else rule["network"] = {network};
    } else if (kind == "src-port" || kind == "dst-port") {
        if (!appendRulePorts(rule, value, kind == "src-port")) result.error = "端口列表或范围无效";
    } else if (kind == "ip-cidr" || kind == "ip-cidr6" || kind == "src-ip-cidr") {
        const auto cidr = addressCidr(value, false, true);
        if (!cidr) result.error = "必须为有效的 IPv4/IPv6 CIDR";
        else rule[kind == "src-ip-cidr" ? "source_ip_cidr" : "ip_cidr"] = {*cidr};
    } else if (kind == "ip-version") {
        if (headless) result.error = "inline rule_set 没有 ip_version 字段";
        else if (value != "4" && value != "6") result.error = "IP-VERSION 只接受 4/6";
        else rule["ip_version"] = value == "4" ? 4 : 6;
    } else if (kind == "uid") {
#if defined(__linux__) && !defined(__ANDROID__)
        int uid = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), uid);
        if (headless) result.error = "inline rule_set 没有 user_id 字段";
        else if (error != std::errc{} || end != value.data() + value.size() || uid < 0)
            result.error = "UID 必须为 0–2147483647";
        else rule["user_id"] = {uid};
#else
        result.error = "UID 仅支持 Linux 桌面；Android 应用分流使用包名";
#endif
    } else if (kind == "process-name" || kind == "process-name-regex" ||
               kind == "process-path" || kind == "process-path-regex") {
#if defined(__ANDROID__)
        if (kind == "process-name") rule["package_name"] = {value};
        else if (kind == "process-name-regex") rule["package_name_regex"] = {value};
        else result.error = "Android 不支持桌面进程路径规则";
#elif defined(__linux__) || defined(_WIN32) || (defined(__APPLE__) && !defined(CLASHFLUX_IOS))
        if (kind == "process-name") rule["process_name"] = {value};
        else if (kind == "process-path") rule["process_path"] = {value};
        else if (kind == "process-path-regex") rule["process_path_regex"] = {value};
        else result.error = "桌面内核没有 process_name_regex 字段";
#else
        result.error = "此平台不支持进程规则";
#endif
    } else result.error = "此匹配类型尚未接入";
    return result;
}

nlohmann::json rejectRule(const std::string& target) {
    nlohmann::json rule = {{"action", "reject"}};
    if (target == "REJECT-DROP") rule["method"] = "drop";
    return rule;
}

// Parentheses belong to the Clash logical-rule grammar here. Leaves use a
// deliberately bounded subset; an unsupported child rejects the entire tree.
std::optional<std::vector<std::string>> splitLogicalParts(std::string_view text) {
    std::vector<std::string> parts;
    unsigned depth = 0;
    std::size_t begin = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '(') {
            if (++depth > 32) return std::nullopt;
        } else if (text[i] == ')') {
            if (depth == 0) return std::nullopt;
            --depth;
        } else if (text[i] == ',' && depth == 0) {
            parts.push_back(trimCopy(text.substr(begin, i - begin)));
            begin = i + 1;
        }
    }
    if (depth != 0) return std::nullopt;
    parts.push_back(trimCopy(text.substr(begin)));
    return parts;
}

std::optional<nlohmann::json> logicalCondition(std::string_view text, unsigned depth,
                                               unsigned& count, bool headless,
                                               std::string& error) {
    if (depth > 24 || ++count > 256) return std::nullopt;
    const auto parts = splitLogicalParts(text);
    if (!parts || parts->size() != 2 || (*parts)[1].empty()) return std::nullopt;
    const std::string kind = lowerCopy((*parts)[0]);
    const std::string& value = (*parts)[1];
    if (kind == "and" || kind == "or" || kind == "not") {
        if (value.size() < 3 || value.front() != '(' || value.back() != ')') return std::nullopt;
        const auto children = splitLogicalParts(std::string_view(value).substr(1, value.size() - 2));
        if (!children || children->empty() || (kind == "not" && children->size() != 1))
            return std::nullopt;
        nlohmann::json rules = nlohmann::json::array();
        for (const std::string& child : *children) {
            if (child.size() < 3 || child.front() != '(' || child.back() != ')') return std::nullopt;
            auto compiled = logicalCondition(std::string_view(child).substr(1, child.size() - 2),
                                             depth + 1, count, headless, error);
            if (!compiled) return std::nullopt;
            rules.push_back(std::move(*compiled));
        }
        nlohmann::json rule = {{"type", "logical"}, {"mode", kind == "or" ? "or" : "and"},
                               {"rules", std::move(rules)}};
        if (kind == "not") rule["invert"] = true;
        return rule;
    }
    auto condition = compileLeafCondition(kind, value, headless);
    if (!condition) { error = kind + ": " + condition.error; return std::nullopt; }
    return std::move(condition.rule);
}

void compileRuleProviders(Context& ctx, const YAML::Node& providers) {
    if (!providers) return;
    if (!providers.IsMap()) {
        ctx.note(FidelityScope::Field, Fidelity::Unsupported, "rule-providers",
                 "rule-providers 必须为映射，已忽略");
        return;
    }
    std::map<std::string, unsigned> nameCounts;
    for (const auto& entry : providers) {
        if (entry.first.IsScalar()) {
            ++nameCounts[entry.first.Scalar()];
            ctx.declaredProviders.insert(entry.first.Scalar());
        }
    }
    std::set<std::string> reportedDuplicates;
    for (const auto& entry : providers) {
        const std::string name = entry.first.IsScalar() ? entry.first.Scalar() : "<非标量>";
        const auto reject = [&](std::string_view reason) {
            ctx.note(FidelityScope::Field, Fidelity::Unsupported, "rule-providers." + name,
                     std::format("规则集「{}」{}，整份规则集未加载", name, reason),
                     "当前支持 inline domain/ipcidr/classical 的明确子集；外部 YAML/text/MRS 下载尚未接入");
        };
        if (!entry.first.IsScalar() || name.empty()) {
            reject("名称无效");
            continue;
        }
        if (nameCounts[name] != 1) {
            if (reportedDuplicates.insert(name).second) reject("名称重复，所有同名声明均拒绝");
            continue;
        }
        const YAML::Node provider = entry.second;
        if (!provider.IsMap() || lowerCopy(ytext(provider, "type")) != "inline") {
            reject("尚未接入此来源类型");
            continue;
        }
        const std::string behavior = lowerCopy(ytext(provider, "behavior"));
        if (behavior != "domain" && behavior != "ipcidr" && behavior != "classical") {
            reject("behavior 无效");
            continue;
        }
        bool fieldsValid = true;
        for (const auto& field : provider) {
            const auto key = field.first.IsScalar() ? field.first.Scalar() : "<非标量>";
            if (key != "type" && key != "behavior" && key != "payload" && key != "format") {
                fieldsValid = false;
                break;
            }
        }
        const std::string format = lowerCopy(ytext(provider, "format"));
        if (!fieldsValid || (!format.empty() && format != "yaml" && format != "text")) {
            reject("含未映射字段或格式");
            continue;
        }
        const YAML::Node payload = provider["payload"];
        if (!payload || !payload.IsSequence()) {
            reject("payload 必须为数组");
            continue;
        }
        nlohmann::json rules = nlohmann::json::array();
        bool valid = true;
        std::string failure;
        std::size_t index = 0;
        for (const auto& item : payload) {
            if (!item.IsScalar()) {
                valid = false; failure = "payload[" + std::to_string(index) + "] 不是字符串"; break;
            }
            const std::string value = trimCopy(item.Scalar());
            std::optional<nlohmann::json> condition;
            if (behavior == "domain") {
                const bool suffix = value.starts_with("+.");
                const auto domain = dnsPolicyDomain(suffix ? value.substr(2) : value);
                if (domain) condition = nlohmann::json{{suffix ? "domain_suffix" : "domain", {*domain}}};
            } else if (behavior == "ipcidr") {
                if (const auto cidr = addressCidr(value, false, true))
                    condition = nlohmann::json{{"ip_cidr", {*cidr}}};
            } else {
                unsigned count = 0;
                condition = logicalCondition(value, 0, count, true, failure);
            }
            if (!condition) {
                valid = false;
                failure = "payload[" + std::to_string(index) + "]「" + value + "」无效：" +
                          (failure.empty() ? "匹配格式或类型未接入" : failure);
                break;
            }
            rules.push_back(std::move(*condition));
            ++index;
        }
        if (!valid) { reject(failure); continue; }
        const std::string tag = "clash-provider-" + std::to_string(ctx.providerTags.size());
        ctx.providerTags.emplace(name, tag);
        ctx.ruleSets.push_back({{"type", "inline"}, {"tag", tag}, {"rules", std::move(rules)}});
    }
}

// 单条 Clash 规则 → sing-box route rule；MATCH 返回 false 表示已写入 final。
bool convertRule(Context& ctx, std::string_view rawLine) {
    // 先拷贝到稳定存储：trimCopy 返回的临时 string 一旦赋回 string_view 就
    // 悬垂（这里真实踩过的坑）。
    const std::string line = trimCopy(rawLine);
    if (line.empty() || line.front() == '#') return true;
    std::vector<std::string> parts;
    const std::string prefix = lowerCopy(line.substr(0, line.find(',')));
    const bool logical = prefix == "and" || prefix == "or" || prefix == "not";
    if (logical) {
        auto split = splitLogicalParts(line);
        if (!split) {
            ctx.note(FidelityScope::Rule, Fidelity::Unsupported, line,
                     std::format("逻辑规则 {} 的括号无效或嵌套过深，整条跳过", line));
            return true;
        }
        parts = std::move(*split);
    }
    std::string_view rest{line};
    for (; !logical;) {
        const auto comma = rest.find(',');
        parts.push_back(trimCopy(rest.substr(0, comma)));
        if (comma == std::string_view::npos) break;
        rest = rest.substr(comma + 1);
    }
    if (parts.empty()) return true;
    const std::string kind = lowerCopy(parts[0]);
    if (kind == "match") {
        if (parts.size() != 2 || parts[1].empty()) {
            ctx.note(FidelityScope::Rule, Fidelity::Unsupported, line,
                     std::format("规则 {} 需要 MATCH,目标，已跳过", line));
            return true;
        }
        ctx.finalTarget = parts[1];
        return false;
    }
    if (parts.size() < 3) {
        ctx.note(FidelityScope::Rule, Fidelity::Unsupported, line,
                 std::format("规则 {} 缺少匹配值或目标，已跳过", line));
        return true;
    }
    bool ignoredNoResolve = false;
    for (std::size_t index = 3; index < parts.size(); ++index) {
        if (lowerCopy(parts[index]) == "no-resolve") {
            ignoredNoResolve = true;
        } else {
            ctx.note(FidelityScope::Rule, Fidelity::Unsupported, line,
                     std::format("规则 {} 的修饰符「{}」尚未转换，已跳过", line, parts[index]),
                     "未知修饰符可能改变匹配语义，不能删除后继续执行规则");
            return true;
        }
    }
    const std::string value = parts[1];
    const std::string target = parts[2];
    if (value.empty() || target.empty()) {
        ctx.note(FidelityScope::Rule, Fidelity::Unsupported, line,
                 std::format("规则 {} 的匹配值或目标为空，已跳过", line));
        return true;
    }

    nlohmann::json rule = nlohmann::json::object();
    if (logical) {
        unsigned count = 0;
        std::string error;
        auto condition = logicalCondition(parts[0] + "," + value, 0, count, false, error);
        if (!condition) {
            ctx.note(FidelityScope::Rule, Fidelity::Unsupported, line,
                     std::format("逻辑规则 {} 含无效或未接入的子条件：{}，整条跳过", line,
                                 error.empty() ? "括号、层数或条件数无效" : error),
                     "逻辑条件按路由 schema 转换，不删除子条件后扩大匹配");
            return true;
        }
        rule = std::move(*condition);
    }
    if (target == "REJECT" || target == "REJECT-DROP") {
        rule.update(rejectRule(target));
    } else if (target == "PASS") {
        ctx.note(FidelityScope::Rule, Fidelity::Unsupported, parts[0],
                 std::format("规则 {} 的目标 PASS 暂不支持，已跳过", line),
                 "sing-box 没有 PASS 语义；删掉这条规则或改用 DIRECT");
        return true;
    } else if (target != "DIRECT" && !ctx.tagKnown(target)) {
        ctx.note(FidelityScope::Rule, Fidelity::Unsupported, parts[0],
                 std::format("规则 {} 引用了未知目标「{}」，已跳过", line, target),
                 "检查目标名是否写错，或它引用的节点/组是否因不支持被跳过");
        return true;
    } else {
        rule["outbound"] = target;
    }

    if (logical) {
        // Conditions are already compiled; only the outer rule carries action.
    } else if (kind == "geoip") {
        const std::string code = lowerCopy(value);
        if (code == "private" || code == "lan") {
            rule["ip_is_private"] = true;
        } else {
            if (!ensureGeoipRuleSet(ctx, code)) return true;
            rule["rule_set"] = {ctx.geoipTags.at(code)};
        }
    } else if (kind == "geosite") {
        const std::string category = lowerCopy(value);
        if (!ensureGeositeRuleSet(ctx, category)) return true;
        const std::string& tag = ctx.geositeTags.at(category);
        rule["rule_set"] = {tag};
        if (category == "cn" && target == "DIRECT") {
            ensureChinaDnsRule(ctx, tag);
        }
    } else if (kind == "rule-set") {
        // Explicit providers own their names, including failed declarations.
        // Undeclared historical China aliases remain approximate: bundled GEO
        // content is not proof that it equals an external provider's payload.
        const std::string provider = lowerCopy(value);
        if (const auto found = ctx.providerTags.find(value); found != ctx.providerTags.end()) {
            rule["rule_set"] = {found->second};
        } else if (ctx.declaredProviders.contains(value)) {
            ctx.note(FidelityScope::Rule, Fidelity::Unsupported, line,
                     std::format("规则 {} 的 provider 未成功转换，已跳过", line),
                     "声明的规则集不会被同名国内别名替换");
            return true;
        } else if (provider == "cn" || provider == "china" ||
            provider == "geosite-cn") {
            ctx.note(FidelityScope::Rule, Fidelity::Approx, line,
                     "未声明的国内 RULE-SET 别名使用内置 GEOSITE CN，不能证明与原 provider 内容相同");
            if (!ensureGeositeRuleSet(ctx, "cn")) return true;
            const std::string& tag = ctx.geositeTags.at("cn");
            rule["rule_set"] = {tag};
            if (target == "DIRECT") ensureChinaDnsRule(ctx, tag);
        } else if (provider == "cn-ip" || provider == "china-ip" ||
                   provider == "geoip-cn") {
            ctx.note(FidelityScope::Rule, Fidelity::Approx, line,
                     "未声明的国内 RULE-SET 别名使用内置 GEOIP CN，不能证明与原 provider 内容相同");
            if (!ensureGeoipRuleSet(ctx, "cn")) return true;
            rule["rule_set"] = {ctx.geoipTags.at("cn")};
        } else {
            ctx.note(FidelityScope::Rule, Fidelity::Unsupported, value,
                     std::format("规则集 RULE-SET {} 的格式暂不支持，已跳过", value),
                     "支持已转换的 inline provider 及 GEOIP/GEOSITE；该条规则已丢弃");
            return true;
        }
    } else {
        auto condition = compileLeafCondition(kind, value, false);
        if (!condition) {
            ctx.note(FidelityScope::Rule, Fidelity::Unsupported, line,
                     "规则 " + line + " 无法转换：" + condition.error + "，已跳过");
            return true;
        }
        rule.update(condition.rule);
    }
    if (ignoredNoResolve) {
        ctx.note(FidelityScope::Rule, Fidelity::Approx, line,
                 std::format("规则 {} 的 no-resolve 尚未保真，托管路由仍会执行前置解析", line),
                 "当前保留原匹配规则；no-resolve 不会写成内核不存在的字段");
    }
    ctx.config["route"]["rules"].push_back(std::move(rule));
    return true;
}

// ---- 骨架与收尾 ----------------------------------------------------------------

bool validIpv4(std::string_view value) {
    for (int index = 0; index < 4; ++index) {
        const auto separator = value.find('.');
        const auto part = value.substr(0, separator);
        unsigned int octet = 0;
        const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), octet);
        if (part.empty() || (part.size() > 1 && part.front() == '0') ||
            error != std::errc{} || end != part.data() + part.size() || octet > 255 ||
            (index == 3) != (separator == std::string_view::npos)) return false;
        if (index != 3) value.remove_prefix(separator + 1);
    }
    return true;
}

bool validIpv6(std::string_view value) {
    // Portable parser for literal addresses only (no scoped interface or DNS).
    // A dotted IPv4 tail occupies two 16-bit groups.
    const auto compressed = value.find("::");
    if (compressed != std::string_view::npos &&
        value.find("::", compressed + 2) != std::string_view::npos) return false;
    const auto countGroups = [](std::string_view part, bool allowIpv4) {
        int count = 0;
        if (part.empty()) return count;
        while (!part.empty()) {
            const auto separator = part.find(':');
            const auto group = part.substr(0, separator);
            if (group.empty()) return -1;
            if (group.find('.') != std::string_view::npos) {
                if (!allowIpv4 || separator != std::string_view::npos || !validIpv4(group)) return -1;
                return count + 2;
            }
            if (group.size() > 4 || !std::ranges::all_of(group, [](unsigned char ch) {
                    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
                           (ch >= 'A' && ch <= 'F');
                })) return -1;
            ++count;
            if (separator == std::string_view::npos) return count;
            part.remove_prefix(separator + 1);
            if (part.empty()) return -1;
        }
        return count;
    };
    if (compressed == std::string_view::npos) return countGroups(value, true) == 8;
    const int left = countGroups(value.substr(0, compressed), false);
    const int right = countGroups(value.substr(compressed + 2), true);
    return left >= 0 && right >= 0 && left + right < 8;
}

std::optional<std::string> addressCidr(std::string_view raw, bool ipv4Only,
                                     bool requirePrefix = false) {
    const std::string value = trimCopy(raw);
    const auto slash = value.find('/');
    const std::string_view address = std::string_view(value).substr(0, slash);
    // Keep this validation independent from platform name resolution. A host
    // name must never enter sing-box route_exclude_address as if it were an IP.
    const bool ipv4 = validIpv4(address) &&
        std::ranges::count(address, '.') == 3;
    const int bits = ipv4 ? 32 : (!ipv4Only && validIpv6(address) ? 128 : 0);
    if (bits == 0) return std::nullopt;
    if (slash == std::string::npos) {
        return requirePrefix ? std::nullopt : std::optional{value + "/" + std::to_string(bits)};
    }
    unsigned int prefix = 0;
    const auto [end, error] = std::from_chars(value.data() + slash + 1,
                                             value.data() + value.size(), prefix);
    if (error != std::errc{} || end != value.data() + value.size() ||
        prefix > static_cast<unsigned int>(bits)) return std::nullopt;
    return value;
}

int ruleSpecificity(vpn::MatchKind kind) {
    switch (kind) {
    case vpn::MatchKind::ExactDomain:
    case vpn::MatchKind::ExactIp: return 4;
    case vpn::MatchKind::DomainSuffix: return 3;
    case vpn::MatchKind::Ipv4Cidr: return 2;
    case vpn::MatchKind::Any: return 1;
    }
    return 0;
}

std::optional<nlohmann::json> globalRuleMatch(Context& ctx, const vpn::RouteRule& rule) {
    nlohmann::json match = nlohmann::json::object();
    std::string pattern = trimCopy(rule.pattern);
    switch (rule.match) {
    case vpn::MatchKind::Any:
        if (pattern.empty()) return match;
        break;
    case vpn::MatchKind::ExactDomain:
    case vpn::MatchKind::DomainSuffix:
        if (const auto host = vpn::NormalizeRuleDomain(pattern)) {
            match[rule.match == vpn::MatchKind::ExactDomain ? "domain" : "domain_suffix"] = {*host};
            return match;
        }
        break;
    case vpn::MatchKind::ExactIp:
    case vpn::MatchKind::Ipv4Cidr: {
        const bool cidr = rule.match == vpn::MatchKind::Ipv4Cidr;
        if (!cidr && pattern.find('/') != std::string::npos) break;
        if (const auto address = addressCidr(pattern, cidr, cidr)) {
            match["ip_cidr"] = {*address};
            return match;
        }
        break;
    }
    }
    ctx.result.error = std::format("全局规则「{}」的匹配地址无效，未启动内核", rule.pattern);
    return std::nullopt;
}

std::string unusedOutboundTag(const nlohmann::json& config, std::string preferred) {
    const auto used = [&](const std::string& tag) {
        for (const char* section : {"outbounds", "endpoints"}) {
            if (!config.contains(section) || !config[section].is_array()) continue;
            for (const auto& item : config[section]) {
                if (item.is_object() && item.value("tag", "") == tag) return true;
            }
        }
        return false;
    };
    const std::string base = preferred;
    for (int index = 1; used(preferred); ++index) preferred = base + "-" + std::to_string(index);
    return preferred;
}

bool applyConnectionRules(Context& ctx) {
#if (!defined(__linux__) && !defined(_WIN32)) || defined(__ANDROID__)
    if (ctx.opt.tunInbound && std::ranges::any_of(ctx.opt.nativeConnections,
            [](const auto& native) {
                return native.connected && native.kind != vpn::ConnectionKind::OpenVpn;
            })) {
        ctx.result.error = "当前平台尚未实现原生 VPN 与主 TUN 的补偿路由；请先关闭 TUN";
        return false;
    }
#endif
#ifdef _WIN32
    if (ctx.opt.tunInbound && std::ranges::any_of(ctx.opt.nativeConnections,
            [](const auto& native) {
                return native.connected && native.kind != vpn::ConnectionKind::Pptp &&
                    native.kind != vpn::ConnectionKind::OpenVpn;
            })) {
        ctx.result.error = "Windows 主 TUN 共存目前支持 PPTP 和 sing-box OpenVPN；其他原生 VPN 请先关闭 TUN";
        return false;
    }
#endif
    auto& config = ctx.config;
    if (!config.contains("outbounds")) config["outbounds"] = nlohmann::json::array();
    if (!config["outbounds"].is_array()) {
        ctx.result.error = "sing-box outbounds 必须为数组";
        return false;
    }
    std::string directTag;
    for (const auto& outbound : config["outbounds"]) {
        if (outbound.is_object() && outbound.value("type", "") == "direct" &&
            !outbound.contains("bind_interface") && !outbound.contains("detour")) {
            directTag = outbound.value("tag", "");
            if (!directTag.empty()) break;
        }
    }
    if (directTag.empty()) {
        directTag = unusedOutboundTag(config, "DIRECT");
        config["outbounds"].push_back({{"type", "direct"}, {"tag", directTag}});
    }
    if (!config.contains("endpoints")) config["endpoints"] = nlohmann::json::array();
    if (!config["endpoints"].is_array()) {
        ctx.result.error = "sing-box endpoints 必须为数组";
        return false;
    }
    const std::string finalOutbound = config["route"].value("final", directTag);
    std::map<std::string, std::string> nativeTags;
    std::set<std::string> unavailableWarnings;
    for (const auto& native : ctx.opt.nativeConnections) {
        if (native.id.empty() || nativeTags.contains(native.id)) {
            ctx.result.error = "原生 VPN 连接 ID 为空或重复";
            return false;
        }
        std::string tag;
        const bool referenced = !native.internalRoutes.empty() ||
            std::ranges::any_of(ctx.opt.globalRules, [&](const auto& rule) {
                return rule.enabled && rule.connectionId == native.id;
            });
        if (!referenced) { nativeTags.emplace(native.id, std::string{}); continue; }
        if (native.connected && native.kind == vpn::ConnectionKind::OpenVpn) {
            tag = unusedOutboundTag(config, "clash-flux-openvpn-" +
                                             std::to_string(nativeTags.size()));
            std::string endpointError;
            auto endpoint = compileOpenVpnEndpoint(native, tag, endpointError);
            if (!endpoint) {
                ctx.result.error = std::format("OpenVPN「{}」配置转换失败：{}",
                                               native.id, endpointError);
                ctx.note(FidelityScope::Node, Fidelity::Unsupported, native.id,
                         ctx.result.error, "修正原生连接配置；不会省略失败字段后继续建立连接");
                ctx.result.fidelity.back().sourceId = native.id;
                return false;
            }
            config["endpoints"].push_back(std::move(*endpoint));
        } else if (native.connected && !native.interfaceName.empty()) {
            tag = unusedOutboundTag(config, "clash-flux-native-" + std::to_string(nativeTags.size()));
            config["outbounds"].push_back({{"type", "direct"}, {"tag", tag},
                                          {"bind_interface", native.interfaceName}});
        }
        if (!tag.empty()) ctx.result.participatingSources.push_back(native.id);
        nativeTags.emplace(native.id, std::move(tag));
    }
    const auto applyMainDefault = [&](nlohmann::json& rule) {
        const auto main = ctx.sourceDefaults.find(ctx.opt.mainConnectionId);
        if ((main != ctx.sourceDefaults.end() && main->second.reject) ||
            ctx.finalTarget == "REJECT" || ctx.finalTarget == "REJECT-DROP") {
            rule["action"] = "reject";
            if ((main != ctx.sourceDefaults.end() && main->second.drop) || ctx.finalTarget == "REJECT-DROP") rule["method"] = "drop";
        } else rule["outbound"] = finalOutbound;
    };
    const auto applyTarget = [&](nlohmann::json& rule, const vpn::RouteRule& target) {
        const auto native = nativeTags.find(target.connectionId);
        if (native != nativeTags.end() && target.targetKind == vpn::TargetKind::Default) {
            if (!native->second.empty()) { rule["outbound"] = native->second; return; }
        } else if (target.targetKind == vpn::TargetKind::Default) {
            const auto source = ctx.sourceDefaults.find(target.connectionId);
            if (source != ctx.sourceDefaults.end()) {
                if (source->second.reject) {
                    rule["action"] = "reject";
                    if (source->second.drop) rule["method"] = "drop";
                    return;
                }
                if (!source->second.tag.empty()) { rule["outbound"] = source->second.tag; return; }
            }
            // 兼容没有来源对象的旧单配置调用方。
            if (target.connectionId == ctx.opt.mainConnectionId && !target.connectionId.empty()) {
                rule["outbound"] = finalOutbound; return;
            }
        } else {
            for (const auto& object : ctx.result.sourceObjects) {
                if (object.sourceId == target.connectionId && object.kind == target.targetKind &&
                    object.objectId == target.targetObject) { rule["outbound"] = object.tag; return; }
            }
        }
        if (unavailableWarnings.insert(target.connectionId + "/" + target.targetObject).second)
            ctx.warn("目标「" + target.connectionId + "/" + target.targetObject +
                     "」不可用，执行 " + std::string(vpn::UnavailablePolicyKey(target.unavailable)));
        switch (target.unavailable) {
        case vpn::UnavailablePolicy::Reject: rule["action"] = "reject"; break;
        case vpn::UnavailablePolicy::UseMain: applyMainDefault(rule); break;
        case vpn::UnavailablePolicy::Direct: rule["outbound"] = directTag; break;
        }
    };

    nlohmann::json precedence = nlohmann::json::array({
        {{"action", "sniff"}},
        {{"protocol", "dns"}, {"action", "hijack-dns"}},
        {{"action", "resolve"}},
    });
    auto rules = ctx.opt.globalRules;
    // 原生内网规则进入同一 SourcePolicy 层；显式 UserOverride 始终在其前面。
    struct InternalRule { std::string cidr; std::string connectionId; int prefix; };
    std::vector<InternalRule> internalRules;
    for (const auto& native : ctx.opt.nativeConnections) {
        for (const auto& route : native.internalRoutes) {
            const auto cidr = addressCidr(route, true);
            if (!cidr) {
                ctx.result.error = "原生来源「" + native.id + "」内网地址无效；当前仅支持 IPv4/CIDR";
                return false;
            }
            internalRules.push_back({*cidr, native.id, std::stoi(cidr->substr(cidr->find('/') + 1))});
        }
    }
    std::stable_sort(internalRules.begin(), internalRules.end(), [](const auto& a, const auto& b) {
        return a.prefix > b.prefix;
    });
    std::int64_t order = 0;
    for (const auto& rule : rules) order = std::max(order, rule.order);
    for (const auto& internal : internalRules) {
        if (order == std::numeric_limits<std::int64_t>::max()) {
            ctx.result.error = "规则顺序超出范围"; return false;
        }
        rules.push_back({.match = vpn::MatchKind::Ipv4Cidr, .pattern = internal.cidr,
            .connectionId = internal.connectionId, .priority = 0,
            .id = "internal/" + internal.connectionId + "/" + internal.cidr,
            .tier = vpn::RuleTier::SourcePolicy, .order = ++order});
    }
    std::stable_sort(rules.begin(), rules.end(), vpn::RulePrecedes);
    for (const auto& item : rules) {
        if (!item.enabled) continue;
        if (item.match == vpn::MatchKind::ExactIp && item.pattern.find(':') != std::string::npos &&
            nativeTags.contains(item.connectionId)) {
            ctx.result.error = "原生 VPN 补偿路由当前仅支持 IPv4，不能使用 IPv6 目标规则";
            return false;
        }
        auto rule = globalRuleMatch(ctx, item);
        if (!rule) return false;
        applyTarget(*rule, item);
        precedence.push_back(std::move(*rule));
    }
    precedence.push_back({{"clash_mode", "direct"}, {"outbound", directTag}});
    nlohmann::json globalMode{{"clash_mode", "global"}};
    applyMainDefault(globalMode);
    precedence.push_back(std::move(globalMode));
    for (auto& rule : config["route"]["rules"]) precedence.push_back(std::move(rule));
    config["route"]["rules"] = std::move(precedence);
    return true;
}
