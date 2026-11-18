// Included by the opt-in Linux test binary. Operates only on benchmark fixtures.
namespace {
namespace restart_probe {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using Cache = clashflux::target_source::Cache<CompileOptions, TargetSourceModel>;

double milliseconds(Clock::time_point from, Clock::time_point to = Clock::now()) {
    return std::chrono::duration<double, std::milli>(to - from).count();
}

std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto size = input.tellg();
    if (size < 0 || size > 128 * 1024 * 1024) throw std::runtime_error("Invalid probe file size");
    std::string data(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    if (!input.read(data.data(), size)) throw std::runtime_error("Cannot read probe file");
    return data;
}

std::string sha256(std::string_view bytes) {
    std::array<unsigned char, 32> digest;
    unsigned size = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &size, EVP_sha256(), nullptr) != 1 ||
        size != digest.size())
        throw std::runtime_error("Cannot hash probe input");
    std::string hex;
    for (const auto byte : digest) {
        hex += "0123456789abcdef"[byte >> 4];
        hex += "0123456789abcdef"[byte & 15];
    }
    return hex;
}

Json pack(const TargetSourceModel& model) {
    const auto& data = TargetProjection::Read(model);
    auto origins = Json::array(), objects = Json::array(), diagnostics = Json::array();
    for (const auto& origin : data.origins) {
        origins.push_back(Json::array({origin.sourceIndex, origin.anchor, origin.state,
            origin.rules.size(), origin.diagnostics, origin.synthetic, origin.editable, origin.clashNonScalar}));
    }
    for (const auto& object : data.objects)
        objects.push_back(Json::array({object.sourceId, object.sourceName, object.kind, object.objectId, object.tag}));
    for (const auto& note : data.diagnostics)
        diagnostics.push_back(Json::array({note.scope, note.level, note.subject, note.detail, note.action, note.sourceId}));
    const auto& action = data.defaultAction;
    auto noResolve = Json::array();
    for (const auto id : data.noResolveRuleIds) noResolve.push_back(id);
    return Json::array({data.sourceId, data.nativeDirect, data.legacyClashDefault, data.format,
        Json::array({action.action, action.outbound, action.rejectMethod, action.implicit}),
        std::move(origins), std::move(objects), std::move(diagnostics), data.payload, data.ruleSets,
        std::move(noResolve)});
}

std::shared_ptr<const TargetSourceModel> unpack(Json packed, std::string source) {
    TargetCapture capture;
    auto& data = *capture.data;
    data.sourceText = std::move(source);
    data.sourceId = packed.at(0).get<std::string>();
    data.nativeDirect = packed.at(1).get<std::string>();
    data.legacyClashDefault = packed.at(2).get<std::string>();
    data.format = packed.at(3).get<ProfileFormat>();
    const auto& action = packed.at(4);
    data.defaultAction = {action.at(0), action.at(1), action.at(2), action.at(3)};
    for (const auto& row : packed.at(6))
        data.objects.push_back({row.at(0), row.at(1), row.at(2), row.at(3), row.at(4)});
    for (const auto& row : packed.at(7))
        data.diagnostics.push_back({row.at(0), row.at(1), row.at(2), row.at(3), row.at(4), row.at(5)});
    auto& rules = packed.at(8);
    std::size_t nextRule = 0;
    auto branches = Json::array();
    for (const auto& row : packed.at(5)) {
        branches.clear();
        const auto count = row.at(3).get<std::size_t>();
        if (count > rules.size() - nextRule || row.at(0) != data.origins.size())
            throw std::runtime_error("Invalid probe rule span");
        for (std::size_t i = 0; i < count; ++i) branches.push_back(std::move(rules.at(nextRule++)));
        capture.Record(row.at(1), row.at(2), branches, 0, 0, 0, row.at(7));
        auto& origin = data.origins.back();
        origin.diagnostics = row.at(4).get<std::vector<std::size_t>>();
        if (std::ranges::any_of(origin.diagnostics, [&](auto i) { return i >= data.diagnostics.size(); }))
            throw std::runtime_error("Invalid probe diagnostic index");
        origin.synthetic = row.at(5);
        origin.editable = row.at(6);
    }
    if (nextRule != rules.size()) throw std::runtime_error("Unreferenced probe rules");
    data.ruleSets = std::move(packed.at(9));
    for (const auto& value : packed.at(10)) {
        if (!value.is_number_unsigned()) throw std::runtime_error("Invalid no-resolve rule id");
        const auto id = value.get<std::uint64_t>();
        if (id == 0 || id > data.rules.size() ||
            (!data.noResolveRuleIds.empty() && id <= data.noResolveRuleIds.back()))
            throw std::runtime_error("Invalid no-resolve rule order");
        data.noResolveRuleIds.push_back(id);
    }
    return capture.Finish();
}

std::vector<std::uint8_t> encode(const TargetSourceModel& model, bool cbor) {
    auto packed = pack(model);
    if (cbor) return Json::to_cbor(packed);
    const auto json = packed.dump();
    return {json.begin(), json.end()};
}

// Fixed fixture identity. The probe never accepts application cache files.
std::string identity(std::string_view source, bool cbor) {
    return std::string(cbor ? "target-r5-probe-2-cbor\n" : "target-r5-probe-2-json\n") + sha256(source);
}

CompileOptions optionsFor(const std::string& source) {
    CompileOptions options;
    options.profileYaml = source;
    options.captureTargetSource = true;
    options.mainConnectionId = "target-restart-probe";
    options.mainSourceName = "R5 synthetic source";
    return options;
}

std::shared_ptr<const TargetSourceModel> rebuild(const CompileOptions& options) {
    Cache cache;
    auto result = cache.Get(options, 0, [&] { return compileConfig(options); });
    if (!result.model || !result.error.empty()) throw std::runtime_error("Probe compilation failed: " + result.error);
    return std::move(result.model);
}

void verify(const TargetSourceModel& model) {
    auto options = optionsFor(model.sourceText());
    const auto fresh = rebuild(options);
    if (pack(model).dump() != pack(*fresh).dump() || model.ruleSetsJson() != fresh->ruleSetsJson())
        throw std::runtime_error("Rebuilt and decoded artifacts differ");
    options.profileRuleEdits.rules.push_back(
        {.id="probe", .match="domain", .pattern="personal.test", .target="DIRECT"});
    const auto originalView = ProjectTargetSource(*fresh, options.profileRuleEdits);
    const auto loadedView = ProjectTargetSource(model, options.profileRuleEdits);
    if (originalView.rules != loadedView.rules || originalView.original != loadedView.original ||
        originalView.error != loadedView.error)
        throw std::runtime_error("Rebuilt and decoded edit projections differ");
    options.captureTargetSource = false;
    const auto cold = compileConfig(options, {}, fresh.get());
    const auto loaded = compileConfig(options, {}, &model);
    if (!cold.error.empty() || !loaded.error.empty())
        throw std::runtime_error("Probe edit candidate failed: " + cold.error + " / " + loaded.error);
    if (cold.json != loaded.json ||
        cold.fidelity != loaded.fidelity || cold.warnings != loaded.warnings ||
        cold.targetRulesReused != loaded.targetRulesReused || cold.rulePlan.origins != loaded.rulePlan.origins)
        throw std::runtime_error("Decoded artifact changed compiler output");
}
} // namespace restart_probe
} // namespace

int RunTargetRestartProbe(std::string_view mode, const std::filesystem::path& sourcePath,
    const std::filesystem::path& artifactPath) {
    using namespace restart_probe;
    const bool cbor = mode.ends_with("-cbor");
    const bool write = mode == "write-cbor" || mode == "write-json";
    const bool check = mode == "verify-cbor" || mode == "verify-json";
    const bool load = check || mode == "load-cbor" || mode == "load-json";
    if (!load && !write && mode != "rebuild") return 2;
    try {
        const auto started = Clock::now();
        auto source = read(sourcePath);
        auto stage = Clock::now();
        Json stats{{"source_bytes", source.size()}, {"source_read_ms", milliseconds(started, stage)}};
        std::shared_ptr<const TargetSourceModel> model;
        if (load) {
            const auto bytes = read(artifactPath);
            auto next = Clock::now();
            stats["artifact_read_ms"] = milliseconds(stage, next);
            stage = next;
            const auto key = identity(source, cbor);
            const std::string_view artifact(bytes);
            if (!artifact.starts_with(key) || artifact.size() <= key.size() + 64)
                throw std::runtime_error("Probe source or encoding identity mismatch");
            const auto body = artifact.substr(key.size() + 64);
            if (artifact.substr(key.size(), 64) != sha256(body))
                throw std::runtime_error("Probe body mismatch");
            next = Clock::now();
            stats["hash_check_ms"] = milliseconds(stage, next);
            stage = next;
            auto packed = cbor ? Json::from_cbor(body.begin(), body.end()) : Json::parse(body);
            next = Clock::now();
            stats["codec_decode_ms"] = milliseconds(stage, next);
            stage = next;
            model = unpack(std::move(packed), std::move(source));
            stats["model_restore_ms"] = milliseconds(stage);
            stats["artifact_bytes"] = bytes.size();
        } else {
            model = rebuild(optionsFor(source));
            stats["compile_capture_ms"] = milliseconds(stage);
        }
        stage = Clock::now();
        const auto view = ProjectTargetSource(*model);
        const auto ready = Clock::now();
        if (!view.error.empty()) throw std::runtime_error(view.error);
        stats["project_ms"] = milliseconds(stage, ready);
        stats["ready_ms"] = milliseconds(started, ready);
        stats["view_rows"] = view.rules.size();
        stats["model_billed_bytes"] = model->retainedBytes();
        if (check) verify(*model);
        if (write) {
            stage = Clock::now();
            const auto bytes = encode(*model, cbor);
            const std::string_view body(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            const auto header = identity(source, cbor) + sha256(body);
            stats["encode_hash_ms"] = milliseconds(stage);
            stage = Clock::now();
            std::ofstream output(artifactPath, std::ios::binary | std::ios::trunc);
            output << header << body;
            output.close();
            if (!output) throw std::runtime_error("Cannot write probe artifact");
            stats["write_ms"] = milliseconds(stage);
            stats["artifact_bytes"] = header.size() + body.size();
        }
        std::println("{}", stats.dump());
        return 0;
    } catch (const std::exception& error) {
        std::println(stderr, "target restart probe: {}", error.what());
        return 1;
    }
}
