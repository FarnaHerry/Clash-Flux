#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include "wire_codec.h"

namespace clashflux::connection_trace {

struct Origin {
    std::size_t index = 0;
    std::string ruleId, sourceId, sourceName, tier;
    bool operator==(const Origin&) const = default;
};

// Compiler metadata, separate from the native config. Only accepted plans
// may be paired with a running instance; previews never publish a binding.
struct Plan {
    std::size_t ruleCount = 0;
    std::vector<Origin> origins;
    std::map<std::string, std::string> outboundLabels;
};
struct Binding {
    std::shared_ptr<const Plan> plan;
    std::map<std::string, std::size_t> descriptions;
    std::set<std::string> priorConnections;
    std::size_t ruleCount = 0;
    std::uint64_t epoch = 0, revision = 0, instance = 0;
};
enum class Status { Unavailable, PriorPlan, Unmapped, Ambiguous, Matched };
struct Match {
    Status status = Status::Unavailable;
    std::string ruleId, sourceId, sourceName, tier;
    std::uint64_t revision = 0;
    bool operator==(const Match&) const = default;
};
struct Frame {
    std::string body;
    std::shared_ptr<const Binding> binding;
};

inline std::shared_ptr<Binding> Bind(std::shared_ptr<const Plan> plan,
    const std::vector<wire::RuntimeRule>& rules, std::uint64_t epoch,
    std::uint64_t revision, std::uint64_t instance) {
    if (!plan || !epoch || !revision || !instance || plan->ruleCount != rules.size()) return {};
    auto binding = std::make_shared<Binding>();
    binding->plan = std::move(plan); binding->ruleCount = rules.size();
    binding->epoch = epoch; binding->revision = revision; binding->instance = instance;
    for (const auto& origin : binding->plan->origins)
        if (origin.index >= rules.size()) return {};
    for (std::size_t index = 0; index < rules.size(); ++index) {
        const auto& rule = rules[index];
        const auto [item, inserted] = binding->descriptions.emplace(rule.payload + " => " + rule.proxy, index);
        if (!inserted) item->second = rules.size();
    }
    return binding;
}

struct Operations {
    std::function<std::optional<std::uint64_t>()> instance;
    std::function<std::optional<std::string>()> rules, connections;
    std::function<bool(const Binding&)> current;
};

inline Frame Read(std::shared_ptr<const Plan> plan, std::uint64_t epoch,
    std::uint64_t revision, std::uint64_t instance, std::shared_ptr<const Binding> binding,
    const Operations& operations) {
    Binding expected{.plan = plan, .epoch = epoch, .revision = revision, .instance = instance};
    if (!plan || plan->origins.empty() || !epoch || !revision || !instance || !operations.current(expected) ||
        operations.instance() != instance) return {};
    std::shared_ptr<Binding> prepared;
    if (!binding || binding->plan != plan || binding->epoch != epoch ||
        binding->revision != revision || binding->instance != instance) {
        const auto response = operations.rules();
        if (!response) return {};
        const auto rules = wire::DecodeRuntimeRules(*response);
        if (!rules) return {};
        prepared = Bind(plan, rules.value, epoch, revision, instance);
        if (!prepared) return {};
        binding = prepared;
    }
    auto body = operations.connections();
    if (!body || operations.instance() != instance) return {};
    if (prepared) {
        const auto baseline = wire::DecodeConnectionIds(*body);
        if (!baseline) return {};
        prepared->priorConnections.insert(baseline.value.begin(), baseline.value.end());
    }
    if (!operations.current(*binding)) return {};
    return {std::move(*body), std::move(binding)};
}

inline Match Resolve(const Binding* binding, std::string_view rule,
    std::string_view payload, const std::string& connectionId) {
    if (!binding || !binding->plan || !binding->epoch || !binding->instance ||
        binding->ruleCount != binding->plan->ruleCount || !payload.empty()) return {};
    Match result{.status = Status::Unmapped, .revision = binding->revision};
    if (connectionId.empty() || binding->priorConnections.contains(connectionId)) {
        result.status = Status::PriorPlan; result.revision = 0; return result;
    }
    // Every native rule participates, including unannotated source rules.
    // Build the full-description index once per accepted instance/plan.
    const auto entry = binding->descriptions.find(std::string(rule));
    if (entry == binding->descriptions.end()) return result;
    const auto index = entry->second;
    if (index >= binding->ruleCount) { result.status = Status::Ambiguous; return result; }
    const Origin* found = nullptr;
    for (const auto& origin : binding->plan->origins) {
        if (origin.index != index) continue;
        if (found) { result.status = Status::Ambiguous; return result; }
        found = &origin;
    }
    if (!found || found->ruleId.empty()) return result;
    result.status = Status::Matched;
    result.ruleId = found->ruleId; result.sourceId = found->sourceId;
    result.sourceName = found->sourceName; result.tier = found->tier;
    return result;
}

} // namespace clashflux::connection_trace
