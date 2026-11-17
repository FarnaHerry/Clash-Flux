// Private native payload. The public module only exports owning descriptors.
struct TargetSourceModel::Data {
    std::string sourceId, nativeDirect, sourceText, legacyClashDefault;
    std::size_t retained = 0;
    ProfileFormat format = ProfileFormat::ClashYaml;
    std::vector<TargetRule> rules;
    std::vector<TargetRuleOrigin> origins;
    TargetDefaultAction defaultAction;
    std::vector<SourceObject> objects;
    std::vector<FidelityNote> diagnostics;
    std::vector<std::uint64_t> noResolveRuleIds;
    nlohmann::json payload = nlohmann::json::array();
    nlohmann::json ruleSets = nlohmann::json::array();
};

TargetSourceModel::TargetSourceModel(std::shared_ptr<const Data> data) : data_(std::move(data)) {}
const std::string& TargetSourceModel::sourceId() const noexcept { return data_->sourceId; }
const std::string& TargetSourceModel::sourceText() const noexcept { return data_->sourceText; }
ProfileFormat TargetSourceModel::format() const noexcept { return data_->format; }
const std::vector<TargetRule>& TargetSourceModel::rules() const noexcept { return data_->rules; }
const std::vector<TargetRuleOrigin>& TargetSourceModel::origins() const noexcept { return data_->origins; }
const TargetDefaultAction& TargetSourceModel::defaultAction() const noexcept { return data_->defaultAction; }
const std::vector<SourceObject>& TargetSourceModel::objects() const noexcept { return data_->objects; }
const std::vector<FidelityNote>& TargetSourceModel::diagnostics() const noexcept { return data_->diagnostics; }
std::optional<std::string> TargetSourceModel::ruleJson(std::uint64_t id) const {
    if (!id || id > data_->rules.size()) return std::nullopt;
    return data_->payload[std::size_t(id-1)].dump();
}
std::string TargetSourceModel::ruleSetsJson() const { return data_->ruleSets.dump(); }
std::size_t TargetSourceModel::retainedBytes() const noexcept { return data_->retained; }

// Conservative billing, not an allocator-exact measurement. Count payload,
// descriptors and capacities once per immutable artifact before publishing it.
std::size_t targetJsonBytes(const nlohmann::json& value) {
    std::size_t bytes = sizeof(value);
    if (value.is_object()) for (auto it = value.begin(); it != value.end(); ++it)
        bytes += sizeof(nlohmann::json::object_t::value_type) + 4*sizeof(void*) + it.key().capacity() + targetJsonBytes(it.value());
    else if (value.is_array()) {
        bytes += value.get_ref<const nlohmann::json::array_t&>().capacity()*sizeof(nlohmann::json);
        for (const auto& item : value) bytes += targetJsonBytes(item);
    } else if (value.is_string()) bytes += value.get_ref<const std::string&>().capacity();
    return bytes;
}

struct TargetProjection {
    static const TargetSourceModel::Data& Read(const TargetSourceModel& model) { return *model.data_; }
};

struct TargetCapture {
    std::shared_ptr<TargetSourceModel::Data> data = std::make_shared<TargetSourceModel::Data>();
    bool explicitDefault = false;
    std::optional<std::string> nativeDefaultAnchor;
    std::size_t nextSourceIndex = 0;

    void Record(std::string anchor, TargetRuleState state,
                const nlohmann::json& emitted, std::size_t begin,
                std::size_t firstNote, std::size_t endNote, bool clashNonScalar = false) {
        const auto originIndex = data->origins.size();
        TargetRuleOrigin origin{nextSourceIndex++, std::move(anchor), state, {}, {}};
        origin.clashNonScalar = clashNonScalar;
        origin.synthetic = data->format == ProfileFormat::NodeSubscription || origin.anchor.starts_with("implicit:") ||
            (data->format == ProfileFormat::SingBoxJson && state == TargetRuleState::Default && !explicitDefault);
        if (state == TargetRuleState::Default) explicitDefault = true;
        for (auto index = firstNote; index < endNote; ++index) origin.diagnostics.push_back(index);
        for (auto index = begin; index < emitted.size(); ++index) {
            const auto& native = emitted[index];
            TargetRule rule{std::uint64_t(data->rules.size()+1), originIndex, {}, {}, {}, {}, false};
            if (native.is_object()) {
                const auto stringField = [&](const char* key) {
                    const auto found = native.find(key);
                    if (found == native.end()) return std::string{};
                    if (!found->is_string()) { rule.opaque = true; return std::string{}; }
                    return found->get<std::string>();
                };
                rule.action = stringField("action");
                rule.outbound = stringField("outbound");
                rule.rejectMethod = stringField("method");
                if (!native.contains("action") && !rule.outbound.empty()) rule.action = "route";
                for (auto field = native.begin(); field != native.end(); ++field) {
                    if (field.key() == "action" || field.key() == "outbound" || field.key() == "method") continue;
                    static const std::set<std::string> stringMatches{"domain", "domain_suffix", "domain_keyword", "domain_regex",
                        "ip_cidr", "source_ip_cidr", "process_name", "process_name_regex", "process_path", "process_path_regex",
                        "rule_set", "network", "protocol", "inbound"};
                    if (stringMatches.contains(field.key()) && field->is_array() && !field->empty() &&
                        std::ranges::all_of(*field, [](const auto& value) { return value.is_string(); })) {
                        TargetMatch match{field.key(), {}};
                        for (const auto& value : *field) match.values.push_back(value.template get<std::string>());
                        rule.matches.push_back(std::move(match));
                    } else rule.opaque = true;
                }
            } else rule.opaque = true;
            origin.rules.push_back(rule.id);
            data->rules.push_back(std::move(rule));
            data->payload.push_back(native);
        }
        if (!origin.rules.empty()) origin.state = TargetRuleState::Emitted;
        data->origins.push_back(std::move(origin));
    }

    std::shared_ptr<const TargetSourceModel> Finish() {
        auto bytes = sizeof(*data) + sizeof(TargetSourceModel) + data->sourceId.capacity() + data->nativeDirect.capacity() +
            data->sourceText.capacity() + data->legacyClashDefault.capacity() +
            data->defaultAction.action.capacity() + data->defaultAction.outbound.capacity() + data->defaultAction.rejectMethod.capacity() +
            targetJsonBytes(data->payload) + targetJsonBytes(data->ruleSets) +
            data->rules.capacity()*sizeof(TargetRule) + data->origins.capacity()*sizeof(TargetRuleOrigin) +
            data->objects.capacity()*sizeof(SourceObject) + data->diagnostics.capacity()*sizeof(FidelityNote);
        bytes += data->noResolveRuleIds.capacity()*sizeof(std::uint64_t);
        for (const auto& rule : data->rules) {
            bytes += rule.action.capacity() + rule.outbound.capacity() + rule.rejectMethod.capacity() + rule.matches.capacity()*sizeof(TargetMatch);
            for (const auto& match : rule.matches) {
                bytes += match.field.capacity() + match.values.capacity()*sizeof(std::string);
                for (const auto& value : match.values) bytes += value.capacity();
            }
        }
        for (const auto& origin : data->origins)
            bytes += origin.anchor.capacity() + origin.rules.capacity()*sizeof(std::uint64_t) + origin.diagnostics.capacity()*sizeof(std::size_t);
        for (const auto& object : data->objects)
            bytes += object.sourceId.capacity() + object.sourceName.capacity() + object.objectId.capacity() + object.tag.capacity();
        for (const auto& note : data->diagnostics)
            bytes += note.sourceId.capacity() + note.subject.capacity() + note.detail.capacity() + note.action.capacity();
        data->retained = bytes;
        // Detach all mutable builder ownership when publishing the snapshot.
        std::shared_ptr<const TargetSourceModel::Data> immutable = std::move(data);
        return std::shared_ptr<const TargetSourceModel>(new TargetSourceModel(std::move(immutable)));
    }
};
