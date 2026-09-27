// singbox.cpp — clashflux.singbox 实现单元（yaml-cpp 解析订阅 + nlohmann 合成 JSON）。
//
// 产物形态对齐 sing-box 1.14：special outbounds（block/dns）已移除，REJECT
// 走 route rule action；sniff 走首条 rule action（不再写 inbound.sniff）；
// DNS 用 1.12+ 的 typed server。字段集保持严格最小——sing-box 对未知字段
// 直接拒绝启动。
module;

#include <yaml-cpp/yaml.h>

module clashflux.singbox;

import std;
import nlohmann.json;

namespace singbox {
namespace {

#include "singbox_yaml_openvpn.inc"
#include "singbox_context_dns.inc"
#include "singbox_proxy.inc"
#include "singbox_rules.inc"
#include "singbox_compile.inc"
} // namespace

std::string FidelitySummary(const std::vector<FidelityNote>& notes) {
    std::size_t nodeSkip = 0;
    std::size_t nodeApprox = 0;
    std::size_t groupSkip = 0;
    std::size_t groupApprox = 0;
    std::size_t ruleSkip = 0;
    std::size_t ruleApprox = 0;
    std::size_t dnsMiss = 0;
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
            ++dnsMiss;
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
            if (!ctx.result.error.empty() || !applyConnectionRules(ctx)) return std::move(ctx.result);
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
    }

    // final：MATCH 目标优先；REJECT 目标转成兜底 reject 规则；缺省回落首个组。
    std::string finalOutbound = ctx.finalTarget;
    if (finalOutbound == "REJECT" || finalOutbound == "REJECT-DROP") {
        ctx.config["route"]["rules"].push_back({{"action", "reject"}});
        finalOutbound = "DIRECT";
    }
    if (finalOutbound.empty() || finalOutbound == "PASS") finalOutbound = "DIRECT";
    if (finalOutbound != "DIRECT" && !ctx.tagKnown(finalOutbound)) {
        ctx.note(FidelityScope::Rule, Fidelity::Approx, "MATCH",
                 std::format("MATCH 目标「{}」不存在，回落 DIRECT", finalOutbound),
                 "订阅里的兜底规则引用了一个不存在的节点/组；确认它没有被编译器跳过");
        finalOutbound = "DIRECT";
    }
    if (finalOutbound == "DIRECT") {
        // 无订阅/无 MATCH：默认全局指向首个 selector 组，保持「有订阅即可用代理」。
        for (const auto& outbound : ctx.outbounds) {
            if (outbound.value("type", "") == "selector") {
                finalOutbound = outbound.value("tag", "DIRECT");
                break;
            }
        }
    }
    ctx.config["route"]["final"] = finalOutbound;

    if (!ctx.ruleSets.empty()) {
        ctx.config["route"]["rule_set"] = std::move(ctx.ruleSets);
    }
    ctx.config["outbounds"] = std::move(ctx.outbounds);
    if (!applyConnectionRules(ctx)) return std::move(ctx.result);
    ctx.result.json = ctx.config.dump();
    return std::move(ctx.result);
}

bool RuleSetCacheValid(const std::filesystem::path& path) {
    // 头部 = "SRS" 魔数 + 版本号；只做低成本校验，完整校验需要解压 zlib 流。
    constexpr std::size_t kHeaderBytes = 4;
    constexpr std::string_view kMagic = "SRS";
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size < kHeaderBytes) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    char magic[kMagic.size()]{};
    in.read(magic, static_cast<std::streamsize>(sizeof(magic)));
    return in.gcount() == static_cast<std::streamsize>(sizeof(magic)) &&
           std::string_view(magic, sizeof(magic)) == kMagic;
}

} // namespace singbox
