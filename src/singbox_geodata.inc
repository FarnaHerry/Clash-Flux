// Bounded v2ray GeoSiteList/GeoIPList protobuf reader. No legacy Geo database
// reaches sing-box: selected categories become native inline headless rules.
// Schema: v2fly/v2ray-core app/router/routercommon/common.proto.
constexpr std::size_t kGeoDataMaxBytes = 64 * 1024 * 1024;

struct GeoWireField { unsigned id = 0, wire = 0; std::uint64_t number = 0; std::string_view bytes; };
struct GeoWireReader {
    std::string_view input;
    std::uint64_t varint() {
        std::uint64_t result = 0;
        for (unsigned shift = 0; shift < 70; shift += 7) {
            if (input.empty()) throw std::runtime_error("截断的 varint");
            const auto byte = static_cast<unsigned char>(input.front()); input.remove_prefix(1);
            if (shift == 63 && byte > 1) throw std::runtime_error("varint 溢出");
            result |= std::uint64_t(byte & 127) << shift;
            if (!(byte & 128)) return result;
        }
        throw std::runtime_error("varint 溢出");
    }
    GeoWireField next() {
        const auto tag = varint();
        if (!(tag >> 3) || (tag >> 3) > 536870911) throw std::runtime_error("字段编号无效");
        GeoWireField field{static_cast<unsigned>(tag >> 3), static_cast<unsigned>(tag & 7)};
        if (field.wire == 0) field.number = varint();
        else if (field.wire == 2) {
            const auto length = varint();
            if (length > input.size()) throw std::runtime_error("截断的字段");
            field.bytes = input.substr(0, length); input.remove_prefix(length);
        } else throw std::runtime_error("不支持的字段编码");
        return field;
    }
};

bool geoCode(std::string_view code) {
    return !code.empty() && code.size() <= 128 && std::ranges::all_of(code, [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '!';
    });
}

std::string geoAttribute(std::string_view bytes) {
    GeoWireReader reader{bytes};
    bool key = false, value = false;
    std::string name;
    while (!reader.input.empty()) {
        const auto f = reader.next();
        if (f.id == 1 && f.wire == 2 && !key && geoCode(f.bytes)) { key = true; name = lowerCopy(std::string(f.bytes)); }
        else if ((f.id == 2 || f.id == 3) && f.wire == 0 && !value &&
                 (f.id == 3 || f.number <= 1)) value = true;
        else throw std::runtime_error("未知或重复的域名属性");
    }
    if (!key) throw std::runtime_error("域名属性缺少名称");
    return name;
}

void geoDomain(std::string_view bytes, nlohmann::json* rules, const std::vector<std::string>* filters = nullptr) {
    GeoWireReader reader{bytes};
    bool typeSeen = false, valueSeen = false;
    std::uint64_t type = 0;
    std::string_view value;
    unsigned attributes = 0;
    std::vector<std::string> names;
    while (!reader.input.empty()) {
        const auto f = reader.next();
        if (f.id == 1 && f.wire == 0 && !typeSeen) { typeSeen = true; type = f.number; }
        else if (f.id == 2 && f.wire == 2 && !valueSeen) { valueSeen = true; value = f.bytes; }
        else if (f.id == 3 && f.wire == 2 && ++attributes <= 64) {
            auto name = geoAttribute(f.bytes);
            if (filters && rules) names.push_back(std::move(name));
        }
        else throw std::runtime_error("未知或重复的域名字段");
    }
    if (type > 3 || value.empty() || value.size() > 4096 ||
        !std::ranges::all_of(value, [](unsigned char c) { return c >= 32 && c != 127; }))
        throw std::runtime_error("域名类型或内容无效");
    if (type == 2 || type == 3) {
        // Keep ordinary root/full domains exact; reject constructs whose legacy
        // matching would differ from native domain/domain_suffix semantics.
        if (value.front() == '.' || value.back() == '.' || value.find("..") != std::string_view::npos ||
            !std::ranges::all_of(value, [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
            })) throw std::runtime_error("不支持的完整域名或根域名");
    }
    if (rules && (!filters || std::ranges::all_of(*filters, [&](const auto& key) {
            return std::ranges::find(names, key) != names.end();
        }))) {
        static constexpr const char* keys[]{"domain_keyword", "domain_regex", "domain_suffix", "domain"};
        auto& values = (*rules)[keys[type]];
        if (values.is_null()) values = nlohmann::json::array();
        values.push_back(value);
    }
}

void geoCidr(std::string_view bytes, nlohmann::json* rules) {
    GeoWireReader reader{bytes};
    bool addressSeen = false, prefixSeen = false;
    std::string_view address;
    std::uint64_t prefix = 0;
    while (!reader.input.empty()) {
        const auto f = reader.next();
        if (f.id == 1 && f.wire == 2 && !addressSeen) { addressSeen = true; address = f.bytes; }
        else if (f.id == 2 && f.wire == 0 && !prefixSeen) { prefixSeen = true; prefix = f.number; }
        else throw std::runtime_error("未知或重复的网段字段");
    }
    if ((address.size() != 4 && address.size() != 16) || prefix > address.size() * 8)
        throw std::runtime_error("IP 地址或前缀无效");
    if (!rules) return;
    std::string text;
    if (address.size() == 4) {
        for (unsigned i = 0; i < 4; ++i) {
            if (i) text += '.';
            text += std::to_string(static_cast<unsigned char>(address[i]));
        }
    } else {
        for (unsigned i = 0; i < 16; i += 2) {
            if (i) text += ':';
            text += std::format("{:x}", (static_cast<unsigned char>(address[i]) << 8) |
                                      static_cast<unsigned char>(address[i + 1]));
        }
    }
    auto& values = (*rules)["ip_cidr"];
    if (values.is_null()) values = nlohmann::json::array();
    values.push_back(text + "/" + std::to_string(prefix));
}

struct GeoDataIndex {
    struct Entry { std::size_t offset, length; };
    std::string kind, content;
    std::map<std::string, Entry> entries;
};

std::string geoEntry(std::string_view bytes, bool site, nlohmann::json* rules, std::size_t& count,
        const std::vector<std::string>* filters = nullptr) {
    GeoWireReader reader{bytes};
    std::string code;
    bool inverseSeen = false, inverse = false;
    std::size_t records = 0;
    while (!reader.input.empty()) {
        const auto f = reader.next();
        if (f.id == 1 && f.wire == 2 && code.empty() && geoCode(f.bytes)) code = lowerCopy(std::string(f.bytes));
        else if (f.id == 2 && f.wire == 2) {
            if (++count > 2'000'000) throw std::runtime_error("条目总数超过上限");
            ++records;
            if (site) geoDomain(f.bytes, rules, filters); else geoCidr(f.bytes, rules);
        } else if (!site && f.id == 3 && f.wire == 0 && !inverseSeen && f.number <= 1) {
            inverseSeen = true; inverse = f.number != 0;
        } else throw std::runtime_error("未知或重复的 Geo 分类字段");
    }
    if (code.empty() || !records) throw std::runtime_error("Geo 分类缺少名称或内容");
    if (rules && inverse) (*rules)["invert"] = true;
    return code;
}

std::shared_ptr<const GeoDataIndex> geoDataIndex(std::string_view kind, const std::string& content) {
    // Index offsets only, never a second expanded rules database. Bounded to two
    // validated files; full byte equality, not a hash, decides cache identity.
    static std::mutex mutex;
    static std::vector<std::shared_ptr<const GeoDataIndex>> cache;
    std::lock_guard lock(mutex);
    for (const auto& item : cache) if (item->kind == kind && item->content == content) return item;
    if ((kind != "geoip" && kind != "geosite") || content.empty() || content.size() > kGeoDataMaxBytes)
        throw std::runtime_error("Geo 类型或文件大小无效（最大 64 MiB）");
    auto index = std::make_shared<GeoDataIndex>();
    index->kind = kind; index->content = content;
    GeoWireReader reader{index->content};
    std::size_t count = 0;
    while (!reader.input.empty()) {
        const auto f = reader.next();
        if (f.id != 1 || f.wire != 2) throw std::runtime_error("未知的 Geo 数据库字段");
        const auto code = geoEntry(f.bytes, kind == "geosite", nullptr, count);
        const auto offset = static_cast<std::size_t>(f.bytes.data() - index->content.data());
        if (!index->entries.emplace(code, GeoDataIndex::Entry{offset, f.bytes.size()}).second || index->entries.size() > 16384)
            throw std::runtime_error("Geo 分类重复或超过上限");
    }
    if (index->entries.empty()) throw std::runtime_error("Geo 数据库为空");
    if (cache.size() == 2) cache.erase(cache.begin());
    cache.push_back(index);
    return index;
}

#include "singbox_mmdb.inc"

bool geoDataResource(const HttpRuleProviderResource& r) {
    return (r.format == "geodata" && (r.behavior == "geosite" || r.behavior == "geoip")) ||
        (r.format == "mmdb" && (r.behavior == "geoip" || r.behavior == "asn"));
}

std::optional<HttpRuleProviderResource> geoDeclaration(const YAML::Node& root, std::string_view kind,
        const std::string& sourceId, std::string& error) {
    const auto geox = root["geox-url"];
    if (!geox) return std::nullopt;
    if (!geox.IsMap()) { error = "geox-url 必须为映射"; return std::nullopt; }
    std::set<std::string> fields;
    for (const auto& field : geox) {
        if (!field.first.IsScalar() || !fields.insert(field.first.Scalar()).second) {
            error = "geox-url 含重复或无效字段"; return std::nullopt;
        }
    }
    bool geodata = false;
    try { if (root["geodata-mode"]) geodata = root["geodata-mode"].as<bool>(); }
    catch (...) { error = "geodata-mode 必须为布尔值"; return std::nullopt; }
    const bool mmdb = kind == "mmdb" || kind == "asn" || (kind == "geoip" && !geodata);
    if (kind == "mmdb" && geodata) return std::nullopt;
    const auto value = geox[std::string(kind == "geoip" && !geodata ? "mmdb" : kind)];
    if (kind == "geoip" && geodata && !value) return std::nullopt;
    if (!value) return std::nullopt;
    if (!value.IsScalar() || value.Scalar().size() > 8192 ||
        !clashflux::profile_link::IsHttpUrl(value.Scalar())) {
        error = "Geo 需要有效 HTTP(S) URL"; return std::nullopt;
    }
    const auto url = value.Scalar();
    const auto begin = url.find("://") + 3, end = url.find_first_of("/?#", begin);
    if (url.substr(begin, end - begin).find('@') != std::string::npos) {
        error = "Geo URL userinfo 鉴权尚未支持"; return std::nullopt;
    }
    HttpRuleProviderResource resource;
    resource.name = kind == "geoip" && !geodata ? "mmdb" : kind; resource.sourceId = sourceId; resource.url = url;
    resource.behavior = kind == "mmdb" ? "geoip" : kind; resource.format = mmdb ? "mmdb" : "geodata";
    resource.maxBytes = kGeoDataMaxBytes;
    resource.intervalSeconds = 24 * 3600;
    try {
        if (const auto interval = root["geo-update-interval"]) {
            const auto hours = interval.as<unsigned>();
            if (hours > 8760) throw std::runtime_error("interval");
            resource.intervalSeconds = std::uint64_t(hours) * 3600;
        }
        if (!root["geo-auto-update"] || !root["geo-auto-update"].as<bool>()) resource.intervalSeconds = 0;
    } catch (...) { error = "Geo 自动更新/间隔无效（0–8760 小时）"; return std::nullopt; }
    resource.identity = nlohmann::json::array({mmdb ? "geo-mmdb-v1" : "geo-dat-v1", sourceId,
        kind == "geoip" && !geodata ? "mmdb" : kind, url,
        resource.intervalSeconds, resource.maxBytes}).dump();
    resource.cacheKey = httpProviderKey(resource.identity);
    return resource;
}

bool configureGeoData(Context& ctx, const YAML::Node& root) {
    for (const auto* kind : {"geosite", "geoip", "asn"}) {
        std::string error;
        auto resource = geoDeclaration(root, kind, ctx.sourceId, error);
        if (!error.empty()) ctx.geoFailures[kind] = std::move(error);
        if (!resource) continue;
        ctx.customGeo[kind] = *resource;
        // Explicit row refresh can download an otherwise unused database.
        if (ctx.opt.refreshProviderKey == resource->cacheKey)
            ctx.result.httpRuleProviders.push_back(*resource);
    }
    return true;
}

bool customGeoRuleSet(Context& ctx, std::string_view kind, std::string code) {
    const auto failure = ctx.geoFailures.find(std::string(kind));
    const auto fail = [&](std::string reason) {
        ctx.result.error = std::string(kind) + "," + code + ": " + reason;
        ctx.note(FidelityScope::Rule, Fidelity::Unsupported, std::string(kind) + "," + code, ctx.result.error);
        return false;
    };
    if (failure != ctx.geoFailures.end()) return fail(failure->second);
    const auto& resource = ctx.customGeo.at(std::string(kind));
    std::string category = code;
    std::vector<std::string> filters;
    if (kind == "geosite") {
        const auto at = category.find('@');
        if (at != std::string::npos) {
            category.resize(at);
            auto rest = std::string_view(code).substr(at + 1);
            for (;;) {
                const auto end = rest.find('@');
                const auto key = trimCopy(std::string(rest.substr(0, end)));
                if (!geoCode(key) || filters.size() >= 64) return fail("Geo 属性筛选无效或超过上限");
                filters.push_back(lowerCopy(key));
                if (end == std::string_view::npos) break;
                rest.remove_prefix(end + 1);
            }
        }
    }
    if (!geoCode(category) || (kind != "geosite" && code.find('@') != std::string::npos)) return fail("Geo 分类无效");
    if (!std::ranges::any_of(ctx.result.httpRuleProviders, [&](const auto& r) { return r.identity == resource.identity; }))
        ctx.result.httpRuleProviders.push_back(resource);
    std::string error;
    const auto pinned = ctx.opt.ruleProviderContents.find(resource.cacheKey);
    const auto previous = ctx.result.ruleProviderContents.find(resource.cacheKey);
    std::optional<std::string> loaded;
    const std::string* bytes = nullptr;
    if (pinned != ctx.opt.ruleProviderContents.end()) bytes = &pinned->second;
    else if (previous != ctx.result.ruleProviderContents.end()) bytes = &previous->second;
    else {
        loaded = ReadHttpRuleProviderCache(resource, ctx.opt.ruleProviderCacheDir, error);
        if (loaded) bytes = &*loaded;
    }
    if (!bytes) return fail("Geo 数据库尚未准备：" + error);
    try {
        nlohmann::json rule = nlohmann::json::object();
        if (resource.format == "mmdb") {
            const auto index = mmdbIndex(kind, *bytes);
            const auto found = index->entries.find(category);
            if (found == index->entries.end()) return fail("自定义数据库不含该分类");
            rule["ip_cidr"] = mmdbPrefixes(found->second);
        } else {
            auto& index = ctx.geoIndexes[resource.cacheKey];
            if (!index) index = geoDataIndex(kind, *bytes);
            const auto found = index->entries.find(category);
            if (found == index->entries.end()) return fail("自定义数据库不含该分类");
            std::size_t count = 0;
            geoEntry(std::string_view(index->content).substr(found->second.offset, found->second.length), kind == "geosite", &rule, count,
                filters.empty() ? nullptr : &filters);
        }
        const auto tag = std::string(kind) + "-" + code;
        // Native 1.14 requires non-empty inline rules. Two mutually exclusive
        // exact domains joined with AND represent the empty set; an empty
        // object would match every destination instead.
        auto rules = nlohmann::json::array();
        if (!rule.empty()) rules.push_back(std::move(rule));
        else rules.push_back({{"type", "logical"}, {"mode", "and"}, {"rules", nlohmann::json::array({
            {{"domain", {"empty-geosite-a.invalid"}}}, {{"domain", {"empty-geosite-b.invalid"}}}})}});
        ctx.ruleSets.push_back({{"type", "inline"}, {"tag", tag}, {"rules", std::move(rules)}});
        if (loaded) ctx.result.ruleProviderContents.try_emplace(resource.cacheKey, std::move(*loaded));
        ctx.note(FidelityScope::Field, Fidelity::Approx, "geox-url." + std::string(kind),
            "自定义 Geo 转为原生 inline rule_set；应用资源调度器按间隔刷新，配置交接可能重启内核");
        return true;
    } catch (const std::exception& exception) { return fail(exception.what()); }
}
