// Compatibility belongs to the target model, not to a page's parser. Only
// sufficient source anchors are retained; no source text/DOM is loaded here.
std::string targetDefaultTarget(const auto& data) {
    return data.defaultAction.action == "reject"
        ? (data.defaultAction.rejectMethod == "drop" ? std::string("REJECT-DROP") : std::string("REJECT"))
        : data.defaultAction.outbound;
}

std::vector<ProfileRule> targetSourceOriginals(const auto& data) {
    std::vector<ProfileRule> rows;
    rows.reserve(data.origins.size());
    if (data.format == ProfileFormat::SingBoxJson) {
        for (const auto& origin : data.origins) {
            if (origin.state == TargetRuleState::Default) {
                constexpr std::string_view prefix = "json:final:";
                rows.push_back({.anchor=origin.anchor, .type="MATCH", .target=origin.anchor.substr(prefix.size()),
                    .editable=true, .terminal=true, .implicit=true});
            } else {
                auto row = nativeProfileRule(data.payload[origin.rules.front() - 1]);
                row.anchor = origin.anchor;
                rows.push_back(std::move(row));
            }
        }
        return rows;
    }
    for (const auto& origin : data.origins) {
        if (!origin.anchor.starts_with("clash:")) continue;
        const auto raw = origin.anchor.substr(6);
        rows.push_back(origin.clashNonScalar ? clashNonScalarRule(raw) : clashProfileRule(raw));
    }
    if (!std::ranges::any_of(rows, &ProfileRule::terminal))
        rows.push_back(clashImplicitDefault(data.legacyClashDefault));
    return rows;
}

void prepareTargetEditBindings(TargetCapture& capture) {
    auto& data = *capture.data;
    std::unordered_map<std::string_view, std::size_t> counts;
    counts.reserve(data.origins.size());
    for (const auto& origin : data.origins) ++counts[origin.anchor];
    for (auto& origin : data.origins) {
        if (origin.synthetic || origin.clashNonScalar || origin.rules.size() != 1 || counts[origin.anchor] != 1) continue;
        try {
            if (data.format == ProfileFormat::SingBoxJson) {
                origin.editable = nativeProfileRule(data.payload[origin.rules.front() - 1]).editable;
            } else if (origin.anchor.starts_with("clash:")) {
                const auto row = clashProfileRule(origin.anchor.substr(6));
                origin.editable = row.editable && !row.terminal;
            }
        } catch (const nlohmann::json::exception&) {
            // Unknown/invalid native fields remain opaque, never an editable
            // capability or a reason to fail an otherwise valid capture.
            origin.editable = false;
        }
    }
}

const TargetRuleOrigin* targetEditOrigin(const auto& data, std::string_view targetId) {
    constexpr std::string_view prefix = "target:";
    if (!targetId.starts_with(prefix)) return nullptr;
    const auto digits = targetId.substr(prefix.size());
    if (digits.empty() || digits.front() < '1' || digits.front() > '9') return nullptr;
    std::uint64_t id = 0;
    const auto [end, result] = std::from_chars(digits.data(), digits.data()+digits.size(), id);
    if (result != std::errc{} || end != digits.data()+digits.size() || id > data.rules.size()) return nullptr;
    const auto& origin = data.origins[data.rules[id - 1].origin];
    return origin.editable ? &origin : nullptr;
}

bool bindTargetSourceEdit(const TargetSourceModel& model, std::string_view targetId,
                          ProfileRuleEdit& edit, std::string& error) {
    error.clear();
    if (targetId == "target:final" && edit.operation == "final") { edit.anchor.clear(); return true; }
    if (edit.operation == "replace" || edit.operation == "disable") {
        if (const auto* origin = targetEditOrigin(TargetProjection::Read(model), targetId)) {
            edit.anchor = origin->anchor;
            return true;
        }
    }
    error = "目标规则无法唯一映射到可编辑的来源；请重新打开规则列表";
    return false;
}
