// singbox_proxy.inc — 代理节点、传输层与代理组转换
// 由 src/singbox.cpp 在同一匿名命名空间中包含。

bool applyClashFingerprint(Context& ctx, const YAML::Node& root) {
    const YAML::Node value = root["global-client-fingerprint"];
    if (!value) return true;
    if (!value.IsScalar() || !protocol::FingerprintSupported(value.Scalar())) {
        ctx.result.error = "global-client-fingerprint 必须为固定内核支持的非空指纹字符串";
        ctx.note(FidelityScope::Field, Fidelity::Unsupported, "global-client-fingerprint",
                 ctx.result.error, "在节点中指定支持的 client-fingerprint；不会自动换成 chrome");
        return false;
    }
    ctx.defaultFingerprint = value.Scalar();
    return true;
}

// ---- 节点转换 ---------------------------------------------------------------

protocol::TargetCapabilities protocolTargetCapabilities() {
    protocol::TargetCapabilities target;
#if !defined(__ANDROID__) && !defined(CLASHFLUX_IOS)
    target.bindInterface = true;
#endif
#if defined(__linux__) && !defined(__ANDROID__)
    target.routingMark = true;
#endif
    return target;
}

void publishProtocolDiagnostics(Context& ctx, protocol::Diagnostics& diagnostics) {
    for (auto& diagnostic : diagnostics.entries) {
        auto& note = diagnostic.note;
        ctx.note(note.scope, note.level, std::move(note.subject), std::move(note.detail), std::move(note.action));
    }
}

bool protocolNameAvailable(Context& ctx, const std::string& name) {
    if (!ctx.tagKnown(name)) return true;
    ctx.note(FidelityScope::Node, Fidelity::Unsupported, name,
        std::format("订阅包含重名节点「{}」，仅保留首个", name),
        "同名节点只保留第一个；改名后两份才会都生效");
    return false;
}

void noteNodeSubscriptionTlsAlias(Context& ctx, const std::string& name) {
    ctx.note(FidelityScope::Node, Fidelity::Approx, name,
        "非标准 net=tls 兼容映射为 TCP，并保留显式 TLS 设置",
        "建议服务商使用标准 net=tcp、tls=tls 字段");
}

void noteNodeSubscriptionTcpPath(Context& ctx, const std::string& name) {
    ctx.note(FidelityScope::Node, Fidelity::Approx, name,
        "普通 TCP 传输（无伪装）不发送传输路径，已忽略未生效的 path 字段",
        "与 V2Ray/Mihomo 行为一致；若需要伪装或路径传输请检查服务商的 net/type 配置");
}

void noteNodeSubscriptionLegacySs(Context& ctx) {
    // Format recognition is lossless; do not label unchanged protocol semantics approximate.
    ctx.warn("已读取 SS 历史整串 Base64 格式；密码按原文字节保留，不进行百分号解码");
}

// Every format and source completes semantic validation and native emission here.
std::optional<nlohmann::json> emitProtocolNode(Context& ctx, std::optional<protocol::NodeSpec> node,
        protocol::Diagnostics& diagnostics) {
    std::optional<protocol::ValidatedNode> validated;
    if (node) validated = protocol::ValidateNode(std::move(*node), protocolTargetCapabilities(),
        diagnostics, {ctx.defaultFingerprint});
    publishProtocolDiagnostics(ctx, diagnostics);
    if (!validated) return std::nullopt;
    return protocol::EmitSingBox(*validated);
}

std::optional<nlohmann::json> convertProtocolNode(Context& ctx, protocol::NodeSpec node) {
    if (!protocolNameAvailable(ctx, node.origin.name)) return std::nullopt;
    node.origin.sourceId = ctx.sourceId;
    protocol::Diagnostics diagnostics{node.origin, {}};
    return emitProtocolNode(ctx, std::move(node), diagnostics);
}

std::optional<nlohmann::json> convertProxy(Context& ctx, const YAML::Node& item) {
    const std::string name = ytext(item, "name");
    if (name.empty() || ytext(item, "type").empty()) {
        ctx.note(FidelityScope::Node, Fidelity::Unsupported, "(未命名节点)",
            "订阅包含无名称或类型的节点，已跳过",
            "订阅里有一个没有 name/type 的条目，它不会出现在节点列表里");
        return std::nullopt;
    }
    if (!protocolNameAvailable(ctx, name)) return std::nullopt;
    protocol::NodeOrigin origin{ctx.sourceId, name};
    protocol::Diagnostics diagnostics{origin, {}};
    std::optional<protocol::NodeSpec> parsed;
    try {
        parsed = protocol::ParseClashProxy(item, std::move(origin), {ctx.defaultFingerprint},
            protocolTargetCapabilities(), diagnostics);
    } catch (...) {
        // YAML access can throw after earlier compatibility notes were produced.
        publishProtocolDiagnostics(ctx, diagnostics);
        throw;
    }
    return emitProtocolNode(ctx, std::move(parsed), diagnostics);
}

// ---- 代理组转换 --------------------------------------------------------------

std::optional<nlohmann::json> convertGroup(Context& ctx, const YAML::Node& item,
                                           const std::string& name,
                                           const std::vector<std::string>& members,
                                           const std::string& providerTestUrl = {}) {
    const std::string clashType = lowerCopy(ytext(item, "type"));
    for (const auto& field : item) {
        const std::string key = field.first.IsScalar() ? field.first.Scalar() : "(无效字段名)";
        if (key == "name" || key == "type" || key == "proxies" || key == "default-selected" ||
            key == "use" || key == "include-all" || key == "include-all-proxies" || key == "include-all-providers") continue;
        if ((ybool(item, "include-all-proxies") || ybool(item, "include-all-providers") ||
             (item["use"] && item["use"].IsSequence() && item["use"].size() > 0)) &&
            (key == "filter" || key == "exclude-filter" || key == "exclude-type")) continue;
        if ((clashType == "url-test" || clashType == "fallback" || clashType == "load-balance") &&
            (key == "url" || key == "interval" || key == "tolerance")) continue;
        ctx.note(FidelityScope::Group, Fidelity::Approx, name,
                 std::format("代理组「{}」的 {} 未映射，仍使用内核组行为", name, key),
                 "该字段不会写入内核；lazy/timeout/expected-status 等不能假装有等价设置");
    }
    nlohmann::json out = {{"tag", name}};
    if (clashType == "select") {
        out["type"] = "selector";
    } else if (clashType == "url-test" || clashType == "fallback" ||
               clashType == "load-balance") {
        if (clashType != "url-test") {
            ctx.note(FidelityScope::Group, Fidelity::Approx, name,
                     std::format("代理组「{}」类型 {} 在 sing-box 无对应语义，降级为 urltest",
                                 name, clashType),
                     "sing-box 没有 fallback / load-balance：该组现在是「最低延迟」，"
                     "不再是顺序兜底 / 轮询分发");
        }
        // Keep the imported URLTest group (and its URL/interval/tolerance)
        // even for the Android speed-only runtime. CommandClient urlTest on
        // the group then uses the subscription's test URL instead of the
        // per-outbound default URL, matching sing-box and SFA semantics.
        out["type"] = "urltest";
        std::string url = providerTestUrl.empty() ? ytext(item, "url") : providerTestUrl;
        if (url.empty()) url = "https://www.gstatic.com/generate_204";
        out["url"] = url;
        if (!providerTestUrl.empty() && yint(item, "interval").value_or(0) == 0)
            out["interval"] = "300s";
        if (const auto interval = yint(item, "interval"); interval && *interval > 0) {
            out["interval"] = std::format("{}s", *interval);
            if (*interval > 1800) {
                out["idle_timeout"] = std::format("{}s", *interval);
                ctx.note(FidelityScope::Group, Fidelity::Approx, name,
                         std::format("代理组「{}」保留 {}s 测速间隔，并把 idle_timeout 从默认 30m 延长到同一值", name, *interval),
                         "sing-box 要求 interval ≤ idle_timeout；空闲时停止巡检的时间因此延长");
            }
        } else if (item["interval"]) {
            ctx.note(FidelityScope::Group, Fidelity::Approx, name,
                     std::format("代理组「{}」的 interval 无效，使用 sing-box 默认 3m", name),
                     "interval 必须为正整数秒数");
        }
        if (item["tolerance"]) {
            const auto tolerance = yint(item, "tolerance");
            if (tolerance && *tolerance > 0 && *tolerance <= 65535) {
                out["tolerance"] = *tolerance;
            } else {
                ctx.note(FidelityScope::Group, Fidelity::Approx, name,
                         std::format("代理组「{}」的 tolerance 无法原样表达，使用 sing-box 默认 50ms", name),
                         "sing-box 接受 1–65535ms；0 会被内核替换成 50ms，不能表示零容差");
            }
        }
    } else {
        ctx.note(FidelityScope::Group, Fidelity::Unsupported, name,
                 std::format("代理组「{}」类型 {} 暂不支持，已跳过", name, clashType),
                 "该组已从订阅里移除；把 type 改成 select / url-test 才会被编译");
        return std::nullopt;
    }
    // 成员必须是已知的节点/组/DIRECT；REJECT 等特殊目标在 1.14 无对应 outbound，
    // 过滤并提示（sing-box 对未知 outbound tag 直接拒绝启动）。
    nlohmann::json filtered = nlohmann::json::array();
    std::set<std::string> included;
    for (const std::string& member : members) {
        if (member == "REJECT" || member == "REJECT-DROP" || member == "PASS") {
            ctx.note(FidelityScope::Group, Fidelity::Approx, name,
                     std::format("代理组「{}」的成员 {} 在 sing-box 无对应 outbound，已移除",
                                 name, member),
                     "sing-box 1.14 的 REJECT 是规则动作而不是出站，因此该成员不会"
                     "出现在卡片列表里");
            continue;
        }
        if (member != "DIRECT" && !ctx.tagKnown(member)) {
            ctx.note(FidelityScope::Group, Fidelity::Approx, name,
                     std::format("代理组「{}」引用了未知的成员「{}」，已移除", name, member),
                     "检查订阅里被引用的成员名是否写错，或该节点是否因协议不支持被跳过");
            continue;
        }
        if (!included.insert(member).second) {
            ctx.note(FidelityScope::Group, Fidelity::Approx, name,
                     std::format("代理组「{}」的重复成员「{}」只保留首次出现位置", name, member),
                     "sing-box 1.14.4 拒绝 selector/urltest 的重复成员；保留不同成员的原顺序和默认选择，原订阅不改写");
            continue;
        }
        filtered.push_back(member);
    }
    if (filtered.empty()) {
        if (ybool(item, "include-all-proxies")) {
            ctx.note(FidelityScope::Group, Fidelity::Unsupported, name,
                     "自动展开组没有编译后可用成员，空组回退尚未转换，拒绝候选而不改成 DIRECT");
            ctx.result.error = std::format("代理组「{}」没有可用成员", name);
            return std::nullopt;
        }
        filtered.push_back("DIRECT");
    }
    if (const YAML::Node initial = item["default-selected"]; initial) {
        const std::string selected = ytext(item, "default-selected");
        if (out.value("type", "") == "selector" && initial.IsScalar() && !selected.empty() &&
            std::find(filtered.begin(), filtered.end(), nlohmann::json(selected)) != filtered.end()) {
            out["default"] = selected;
        } else {
            ctx.note(FidelityScope::Group, Fidelity::Approx, name,
                     std::format("代理组「{}」的 default-selected 未生效", name),
                     "仅 selector 支持默认选择，目标必须是该组编译后的成员；其它情况沿用内核默认行为");
        }
    }
    out["outbounds"] = std::move(filtered);
    return out;
}

// A detour may point forward to a node or group. Validate the completed graph,
// including every selectable member, so switching groups cannot create a loop.
// Use an explicit stack: untrusted subscriptions can contain long chains.
bool validateOutboundDependencies(Context& ctx) {
    const std::size_t count = ctx.outbounds.size();
    std::map<std::string, std::size_t> indices;
    std::vector<std::string> tags;
    tags.reserve(count);
    auto fail = [&](std::size_t index, const std::string& detail) {
        const bool group = ctx.outbounds[index].value("type", "") == "selector" ||
                           ctx.outbounds[index].value("type", "") == "urltest";
        ctx.note(group ? FidelityScope::Group : FidelityScope::Node, Fidelity::Unsupported,
                 tags[index], detail, "修正引用或代理链；编译器不会移除 detour 后改成直连");
        ctx.result.error = detail;
        return false;
    };
    for (std::size_t index = 0; index < count; ++index) {
        tags.push_back(ctx.outbounds[index].value("tag", ""));
        if (!indices.emplace(tags.back(), index).second) {
            return fail(index, std::format("出站「{}」重名，无法唯一解析依赖", tags.back()));
        }
    }
    std::vector<std::vector<std::size_t>> edges(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto& outbound = ctx.outbounds[index];
        std::vector<std::string> dependencies;
        if (outbound.contains("detour")) dependencies.push_back(outbound["detour"].get<std::string>());
        if (outbound.contains("outbounds")) {
            for (const auto& member : outbound["outbounds"]) dependencies.push_back(member.get<std::string>());
        }
        for (const auto& target : dependencies) {
            const auto found = indices.find(target);
            if (found == indices.end()) {
                return fail(index, std::format("出站「{}」依赖的「{}」未成功编译", tags[index], target));
            }
            edges[index].push_back(found->second);
        }
    }
    struct Frame { std::size_t index; std::size_t next = 0; };
    std::vector<unsigned char> colors(count, 0);
    std::vector<Frame> stack;
    for (std::size_t start = 0; start < count; ++start) {
        if (colors[start] != 0) continue;
        stack.push_back({start});
        colors[start] = 1;
        while (!stack.empty()) {
            auto& frame = stack.back();
            if (frame.next == edges[frame.index].size()) {
                colors[frame.index] = 2;
                stack.pop_back();
                continue;
            }
            const std::size_t target = edges[frame.index][frame.next++];
            if (colors[target] == 1) {
                std::string path;
                const auto beginning = std::find_if(stack.begin(), stack.end(),
                    [&](const Frame& entry) { return entry.index == target; });
                for (auto cursor = beginning; cursor != stack.end(); ++cursor) {
                    if (!path.empty()) path += " → ";
                    path += tags[cursor->index];
                }
                path += " → " + tags[target];
                return fail(frame.index, "出站依赖形成循环：" + path);
            }
            if (colors[target] == 0) {
                colors[target] = 1;
                stack.push_back({target});
            }
        }
    }
    return true;
}

// ---- 规则转换 -----------------------------------------------------------------
