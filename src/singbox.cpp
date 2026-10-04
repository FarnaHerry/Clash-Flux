// singbox.cpp — clashflux.singbox 实现单元（yaml-cpp 解析订阅 + nlohmann 合成 JSON）。
//
// 产物形态对齐 sing-box 1.14：special outbounds（block/dns）已移除，REJECT
// 走 route rule action；sniff 走首条 rule action（不再写 inbound.sniff）；
// DNS 用 1.12+ 的 typed server。字段集保持严格最小——sing-box 对未知字段
// 直接拒绝启动。
module;

#include <yaml-cpp/yaml.h>
#include <zstd.h>
#include "profile_link.h"
#include "http_request_headers.h"

module clashflux.singbox;

import std;
import nlohmann.json;

namespace singbox {
namespace {

#include "singbox_yaml_openvpn.inc"
#include "singbox_context_dns.inc"
#include "singbox_proxy.inc"
#include "singbox_mrs.inc"
#include "singbox_rules.inc"
#include "singbox_compile.inc"
#include "singbox_sources.inc"
} // namespace

std::string FidelitySummary(const std::vector<FidelityNote>& notes) {
    std::size_t nodeSkip = 0;
    std::size_t nodeApprox = 0;
    std::size_t groupSkip = 0;
    std::size_t groupApprox = 0;
    std::size_t ruleSkip = 0;
    std::size_t ruleApprox = 0;
    std::size_t dnsMiss = 0;
    std::size_t dnsApprox = 0;
    std::size_t fieldMiss = 0;
    std::size_t fieldApprox = 0;
    for (const FidelityNote& note : notes) {
        if (note.level == Fidelity::Exact) continue;
        const bool approx = note.level == Fidelity::Approx;
        switch (note.scope) {
        case FidelityScope::Node:
            if (approx) ++nodeApprox; else ++nodeSkip;
            break;
        case FidelityScope::Group:
            if (approx) ++groupApprox; else ++groupSkip;
            break;
        case FidelityScope::Rule:
            if (approx) ++ruleApprox; else ++ruleSkip;
            break;
        case FidelityScope::Dns:
            if (approx) ++dnsApprox; else ++dnsMiss;
            break;
        case FidelityScope::Field:
            if (approx) ++fieldApprox; else ++fieldMiss;
            break;
        }
    }
    // 先报「丢了什么」，再报「变成什么样」；固定顺序便于用户和测试比对。
    std::vector<std::string> parts;
    if (nodeSkip > 0) parts.push_back(std::format("跳过 {} 个节点", nodeSkip));
    if (groupSkip > 0) parts.push_back(std::format("跳过 {} 个组", groupSkip));
    if (ruleSkip > 0) parts.push_back(std::format("忽略 {} 条规则", ruleSkip));
    if (dnsMiss > 0) parts.push_back(std::format("忽略 {} 项 DNS", dnsMiss));
    if (fieldMiss > 0) parts.push_back(std::format("忽略 {} 个字段", fieldMiss));
    if (nodeApprox > 0) parts.push_back(std::format("近似 {} 个节点", nodeApprox));
    if (groupApprox > 0) parts.push_back(std::format("降级 {} 个组", groupApprox));
    if (ruleApprox > 0) parts.push_back(std::format("近似 {} 条规则", ruleApprox));
    if (dnsApprox > 0) parts.push_back(std::format("近似 {} 项 DNS", dnsApprox));
    if (fieldApprox > 0) parts.push_back(std::format("近似 {} 个字段", fieldApprox));
    std::string summary;
    for (const std::string& part : parts) {
        if (!summary.empty()) summary += " · ";
        summary += part;
    }
    return summary;
}

CompileResult compileConfig(const CompileOptions& options) {
    Context ctx{options};
    ctx.sourceId = options.mainConnectionId;
    ctx.result.planRevision = options.planRevision;
    if (!vpn::ValidatePolicyRules(options.globalRules, ctx.result.error)) return std::move(ctx.result);
    const std::string trimmed = trimCopy(options.profileYaml);

    if (!trimmed.empty() && trimmed.front() == '{') {
        // 原生 sing-box JSON：直通，只合并托管设置。
        nlohmann::json parsed = nlohmann::json::parse(trimmed, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            ctx.result.error = "原生 sing-box 配置不是合法的 JSON 对象";
            return std::move(ctx.result);
        }
        ctx.config = std::move(parsed);
        try {
            applyManagedSkeleton(ctx, options);
            if (!ctx.result.error.empty()) return std::move(ctx.result);
            ctx.outbounds = ctx.config.value("outbounds", nlohmann::json::array());
            for (const auto& item : ctx.outbounds) ctx.knownTags.push_back(item.value("tag", ""));
            collectSourceObjects(ctx, options.mainConnectionId, options.mainSourceName);
            for (const auto& item : ctx.config.value("endpoints", nlohmann::json::array())) {
                const auto tag = item.value("tag", "");
                ctx.knownTags.push_back(tag);
                ctx.result.sourceObjects.push_back({options.mainConnectionId, options.mainSourceName, vpn::TargetKind::Node, tag, tag});
            }
            if (!options.mainConnectionId.empty()) {
                ctx.sourceDefaults[options.mainConnectionId] = {ctx.config["route"].value("final", "")};
                ctx.result.participatingSources.push_back(options.mainConnectionId);
            }
            if (!compileAuxiliarySources(ctx)) return std::move(ctx.result);
            ctx.config["outbounds"] = ctx.outbounds;
            if (!applyConnectionRules(ctx)) return std::move(ctx.result);
        } catch (const std::exception& error) {
            ctx.result.error = std::format("原生 sing-box 配置合并失败：{}", error.what());
            return std::move(ctx.result);
        }
        ctx.result.json = ctx.config.dump();
        return std::move(ctx.result);
    }

    ctx.config = nlohmann::json::object();
    applyManagedSkeleton(ctx, options);
    if (!ctx.result.error.empty()) return std::move(ctx.result);
    // DIRECT 是普通 outbound；REJECT 在 1.14 已移除，由规则 action 承担。
    ctx.outbounds.push_back({{"type", "direct"}, {"tag", "DIRECT"}});
    ctx.knownTags.push_back("DIRECT");

    if (!trimmed.empty()) {
        // 订阅内容不可控：任何 yaml-cpp 访问异常都降级为编译错误，不崩应用。
        try {
            compileClashDocument(ctx, trimmed);
        } catch (const std::exception& error) {
            ctx.result.error = std::format("订阅 YAML 解析失败：{}", error.what());
            return std::move(ctx.result);
        }
        if (!ctx.result.error.empty()) return std::move(ctx.result);
    }

    // Explicit MATCH is authoritative, including DIRECT. The first selector
    // is only an application default when the source has no terminal rule.
    std::string finalOutbound = ctx.finalTarget;
    if (finalOutbound == "REJECT" || finalOutbound == "REJECT-DROP") {
        ctx.config["route"]["rules"].push_back(rejectRule(finalOutbound));
        finalOutbound = "DIRECT";
    }
    if (finalOutbound.empty()) finalOutbound = "DIRECT";
    if (finalOutbound != "DIRECT" && !std::ranges::any_of(ctx.outbounds, [&](const auto& output) { return output.value("tag", "") == finalOutbound; })) {
        ctx.result.error = std::format("MATCH 目标「{}」不可用，无法应用配置", finalOutbound);
        ctx.note(FidelityScope::Rule, Fidelity::Unsupported, "MATCH", ctx.result.error,
                 "修正兜底目标；不会改成 DIRECT 或首个代理组");
        return std::move(ctx.result);
    }
    if (ctx.finalTarget.empty()) {
        // 无订阅/无 MATCH：默认全局指向首个 selector 组，保持「有订阅即可用代理」。
        for (const auto& outbound : ctx.outbounds) {
            if (outbound.value("type", "") == "selector") {
                finalOutbound = outbound.value("tag", "DIRECT");
                break;
            }
        }
    }
    ctx.config["route"]["final"] = finalOutbound;
    if (!options.mainConnectionId.empty()) {
        namespaceClashSource(ctx, options.mainConnectionId, options.mainSourceName);
        ctx.sourceDefaults[options.mainConnectionId] = {ctx.config["route"]["final"].get<std::string>(),
            ctx.finalTarget == "REJECT" || ctx.finalTarget == "REJECT-DROP", ctx.finalTarget == "REJECT-DROP"};
        ctx.result.participatingSources.push_back(options.mainConnectionId);
    }
    if (!compileAuxiliarySources(ctx)) return std::move(ctx.result);

    if (!ctx.ruleSets.empty()) {
        ctx.config["route"]["rule_set"] = std::move(ctx.ruleSets);
    }
    ctx.config["outbounds"] = std::move(ctx.outbounds);
    if (!applyConnectionRules(ctx)) return std::move(ctx.result);
    ctx.result.json = ctx.config.dump();
    return std::move(ctx.result);
}

CompileResult inspectClashSourceObjects(const CompileOptions& options) {
    Context ctx{options};
    ctx.sourceId = options.mainConnectionId;
    if (ctx.sourceId.empty()) {
        ctx.result.error = "对象目录需要稳定的来源 ID";
        return std::move(ctx.result);
    }
    const auto text = trimCopy(options.profileYaml);
    if (text.empty() || text.front() == '{') {
        ctx.result.error = "对象目录需要 Clash YAML；原生连接或 JSON 请使用默认出口";
        return std::move(ctx.result);
    }
    try {
        ctx.config = nlohmann::json::object();
        applyManagedSkeleton(ctx, options);
        ctx.outbounds.push_back({{"type", "direct"}, {"tag", "DIRECT"}});
        ctx.knownTags.push_back("DIRECT");
        compileClashDocument(ctx, text, true);
        if (!ctx.result.error.empty()) return std::move(ctx.result);
        if (!ctx.finalTarget.empty() && ctx.finalTarget != "REJECT" && ctx.finalTarget != "REJECT-DROP" &&
            !std::ranges::any_of(ctx.outbounds, [&](const auto& out) {
                return out.value("tag", "") == ctx.finalTarget;
            })) {
            ctx.result.error = "来源默认目标不存在：" + ctx.finalTarget;
            return std::move(ctx.result);
        }
        namespaceClashSource(ctx, options.mainConnectionId, options.mainSourceName);
    } catch (const std::exception& error) {
        ctx.result.sourceObjects.clear();
        ctx.result.error = std::format("对象目录读取失败：{}", error.what());
    }
    return std::move(ctx.result);
}

std::optional<std::string> ReadRuleProviderText(const std::filesystem::path& path,
        std::size_t maxBytes, std::string& error) {
    error.clear();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        error = "不是可读的普通文件"; return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) { error = "文件无法打开"; return std::nullopt; }
    std::string content;
    std::array<char, 16 * 1024> buffer;
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto bytes = static_cast<std::size_t>(input.gcount());
        if (bytes > maxBytes - content.size()) {
            error = "文件超过读取上限"; return std::nullopt;
        }
        content.append(buffer.data(), bytes);
    }
    if (input.bad() || (input.fail() && !input.eof())) {
        error = "文件读取失败"; return std::nullopt;
    }
    return content;
}

std::string HttpRuleProviderCacheImage(const HttpRuleProviderResource& resource,
        const std::string& content) {
    if (resource.cacheKey != httpProviderKey(resource.identity) || resource.identity.size() > 32 * 1024 ||
        resource.identity.find('\n') != std::string::npos || content.size() > resource.maxBytes ||
        resource.maxBytes > 8 * 1024 * 1024) return {};
    return "clash-flux-rule-provider-v1\n" + resource.identity + "\n" + content;
}

std::optional<std::string> ReadHttpRuleProviderCache(const HttpRuleProviderResource& resource,
        const std::filesystem::path& directory, std::string& error) {
    if (resource.cacheKey != httpProviderKey(resource.identity) || resource.identity.size() > 32 * 1024 ||
        resource.maxBytes > 8 * 1024 * 1024) {
        error = "HTTP 缓存资源身份无效"; return std::nullopt;
    }
    const auto path = ruleProviderPath(directory.string(), resource.cacheKey + ".cache", error);
    if (!path) return std::nullopt;
    const auto image = ReadRuleProviderText(*path, resource.maxBytes + 32 * 1024 + 64, error);
    if (!image) return std::nullopt;
    const std::string prefix = "clash-flux-rule-provider-v1\n" + resource.identity + "\n";
    if (!image->starts_with(prefix)) {
        error = "HTTP 规则集缓存身份或版本不匹配"; return std::nullopt;
    }
    auto content = image->substr(prefix.size());
    if (content.size() > resource.maxBytes) { error = "缓存内容超过读取上限"; return std::nullopt; }
    return content;
}

bool ValidateHttpRuleProvider(const HttpRuleProviderResource& resource,
        const std::string& content, std::string& error) {
    error.clear();
    if (resource.asn || resource.behavior == "asn")
        return asnPrefixes(resource, content, error).has_value();
    if (content.size() > resource.maxBytes || resource.maxBytes > 8 * 1024 * 1024 ||
        (resource.format != "yaml" && resource.format != "text" && resource.format != "mrs") ||
        (resource.behavior != "domain" && resource.behavior != "ipcidr" && resource.behavior != "classical")) {
        error = "规则集格式、behavior 或大小不在支持范围"; return false;
    }
    CompileOptions options;
    Context context{options};
    context.providerSyntaxOnly = true;
    return ruleProviderContentRules(context, content, resource.format, resource.behavior, error).has_value();
}

std::optional<std::string> ReadHttpRuleProviderSeed(const HttpRuleProviderResource& resource,
        const std::filesystem::path& directory, std::string& error) {
    const auto path = ruleProviderPath(directory.string(), resource.seedPath, error);
    return path ? ReadRuleProviderText(*path, resource.maxBytes, error) : std::nullopt;
}

std::string BuiltinRuleSetUrl(std::string_view tag) {
    std::string_view kind;
    std::string_view name;
    if (tag.starts_with("geoip-")) {
        kind = "geoip";
        name = tag.substr(6);
        if (name.size() != 2 || !std::ranges::all_of(name, [](char c) {
                return c >= 'a' && c <= 'z';
            })) return {};
    } else if (tag.starts_with("geosite-")) {
        kind = "geosite";
        name = tag.substr(8);
        if (name.empty() || !std::ranges::all_of(name, [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                       c == '-' || c == '_';
            })) return {};
    } else {
        return {};
    }
    return "https://raw.githubusercontent.com/MetaCubeX/meta-rules-dat/sing/geo/" +
           std::string(kind) + "/" + std::string(name) + ".srs";
}

bool RuleSetCacheValid(const std::filesystem::path& path) {
    // 1.14.2 common/srs/binary.go：SRS + version（<=5）+ zlib stream。
    // 这里只筛掉错误页、短头、未来版本和非法 zlib 头；不替代内核解压/规则解析。
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size < 12) return false;
    std::ifstream in(path, std::ios::binary);
    std::array<unsigned char, 6> header{};
    in.read(reinterpret_cast<char*>(header.data()), header.size());
    if (in.gcount() != static_cast<std::streamsize>(header.size())) return false;
    const unsigned int cmf = header[4];
    const unsigned int flg = header[5];
    return header[0] == 'S' && header[1] == 'R' && header[2] == 'S' &&
           header[3] <= 5 && (cmf & 0x0f) == 8 && (cmf >> 4) <= 7 &&
           ((cmf << 8) + flg) % 31 == 0;
}

} // namespace singbox
