// MaxMind DB v2 reader. Bounds cover pointer expansion as well as encoded input.
// No platform library, mmap, legacy Geo runtime or external process is needed.
struct MmdbDecoder {
    std::string_view bytes;
    std::size_t values = 0, payload = 0;
    std::uint64_t number(std::size_t& at, unsigned size) {
        if (size > 8 || at > bytes.size() || size > bytes.size() - at)
            throw std::runtime_error("MMDB 数值越界");
        std::uint64_t value = 0;
        while (size--) value = (value << 8) | static_cast<unsigned char>(bytes[at++]);
        return value;
    }
    nlohmann::json decode(std::size_t& at, unsigned depth = 0, bool pointed = false) {
        if (depth > 64 || ++values > 65536) throw std::runtime_error("MMDB 嵌套或展开条目超限");
        const auto control = number(at, 1);
        unsigned type = control >> 5;
        if (!type) type = 7 + number(at, 1);
        unsigned size = control & 31;
        if (type == 1) {
            if (pointed) throw std::runtime_error("MMDB 指针不能指向指针");
            const unsigned width = ((control >> 3) & 3) + 1;
            std::size_t target = number(at, width);
            if (width < 4) target += ((control & 7) << (width * 8)) +
                (width == 2 ? 2048 : width == 3 ? 526336 : 0);
            return decode(target, depth + 1, true);
        }
        if (size >= 29) size = (size == 29 ? 29 : size == 30 ? 285 : 65821) + number(at, size - 28);
        if (type == 7 || type == 11) {
            if (size > (65536 - values) / (type == 7 ? 2 : 1))
                throw std::runtime_error("MMDB 容器过大");
            auto result = type == 7 ? nlohmann::json::object() : nlohmann::json::array();
            for (unsigned i = 0; i < size; ++i) {
                if (type == 11) result.push_back(decode(at, depth + 1));
                else {
                    const auto key = decode(at, depth + 1);
                    if (!key.is_string() || result.contains(key.get_ref<const std::string&>()))
                        throw std::runtime_error("MMDB 键类型无效或重复");
                    result[key.get_ref<const std::string&>()] = decode(at, depth + 1);
                }
            }
            return result;
        }
        if (type == 14) {
            if (size > 1) throw std::runtime_error("MMDB 布尔值无效");
            return size == 1;
        }
        if (at > bytes.size() || size > bytes.size() - at || size > 1024 * 1024 - payload)
            throw std::runtime_error("MMDB 正文越界或展开超过 1 MiB");
        payload += size;
        if (type == 2) {
            auto result = std::string(bytes.substr(at, size)); at += size;
            // nlohmann performs strict UTF-8 validation during serialization.
            static_cast<void>(nlohmann::json(result).dump());
            return result;
        }
        if (type == 5 || type == 6 || type == 9) {
            if (size > (type == 5 ? 2 : type == 6 ? 4 : 8)) throw std::runtime_error("MMDB 整数过长");
            return number(at, size);
        }
        if ((type == 3 && size == 8) || (type == 15 && size == 4) || type == 4 ||
            (type == 8 && size <= 4) || (type == 10 && size <= 16)) {
            at += size; return nullptr; // ancillary fields are validated, never used as country/ASN keys
        }
        throw std::runtime_error("MMDB 未知或无效字段类型");
    }
};

struct MmdbNetwork {
    std::array<unsigned char, 16> address;
    unsigned char prefix;
    bool ipv4;
};
struct MmdbIndex {
    std::string kind, content;
    std::map<std::string, std::vector<MmdbNetwork>> entries;
};

nlohmann::json mmdbPrefixes(const std::vector<MmdbNetwork>& networks) {
    auto result = nlohmann::json::array();
    for (const auto& network : networks) {
        const auto& address = network.address;
        std::string cidr;
        if (network.ipv4) {
            for (unsigned i = 12; i < 16; ++i) { if (i != 12) cidr += '.'; cidr += std::to_string(address[i]); }
        } else {
            for (unsigned i = 0; i < 16; i += 2) {
                if (i) cidr += ':';
                cidr += std::format("{:x}", (unsigned(address[i]) << 8) | address[i + 1]);
            }
        }
        result.push_back(cidr + '/' + std::to_string(network.prefix));
    }
    return result;
}

std::shared_ptr<const MmdbIndex> mmdbIndex(std::string_view kind, const std::string& content) {
    static std::mutex mutex;
    static std::vector<std::shared_ptr<const MmdbIndex>> cache;
    std::lock_guard lock(mutex);
    for (const auto& item : cache) if (item->kind == kind && item->content == content) return item;
    if ((kind != "geoip" && kind != "asn") || content.size() > kGeoDataMaxBytes)
        throw std::runtime_error("MMDB 类型或大小无效");
    const std::string marker = "\xab\xcd\xefMaxMind.com";
    const auto metaAt = content.rfind(marker);
    if (metaAt == std::string::npos || content.size() - metaAt > 128 * 1024)
        throw std::runtime_error("MMDB 元数据缺失或超限");
    MmdbDecoder metadata{std::string_view(content).substr(metaAt + marker.size())};
    std::size_t at = 0;
    const auto meta = metadata.decode(at);
    if (!meta.is_object() || at != metadata.bytes.size()) throw std::runtime_error("MMDB 元数据结构无效");
    const auto integer = [&](const char* key) {
        if (!meta.contains(key) || !meta[key].is_number_unsigned()) throw std::runtime_error("MMDB 元数据类型无效");
        return meta[key].get<std::uint64_t>();
    };
    if (integer("binary_format_major_version") != 2) throw std::runtime_error("MMDB 主版本不支持");
    static_cast<void>(integer("binary_format_minor_version"));
    static_cast<void>(integer("build_epoch"));
    const auto nodes = integer("node_count"), bits = integer("record_size"), ip = integer("ip_version");
    if (!nodes || nodes > 0xffffffff || (bits != 24 && bits != 28 && bits != 32) || (ip != 4 && ip != 6) ||
        !meta.contains("database_type") || !meta["database_type"].is_string())
        throw std::runtime_error("MMDB 节点、地址族或记录宽度无效");
    const auto type = meta["database_type"].get<std::string>();
    if (kind == "asn" && type != "GeoLite2-ASN" && type != "DBIP-ASN-Lite (compat=GeoLite2-ASN)" &&
        type != "ipinfo generic_asn_free.mmdb") throw std::runtime_error("MMDB ASN 数据库类型不支持");
    const auto nodeSize = bits / 4, treeBytes = nodes * nodeSize;
    if (treeBytes > metaAt || metaAt - treeBytes < 16 ||
        content.substr(treeBytes, 16) != std::string(16, '\0')) throw std::runtime_error("MMDB 搜索树或分隔符无效");
    MmdbDecoder data{std::string_view(content).substr(treeBytes + 16, metaAt - treeBytes - 16)};
    auto index = std::make_shared<MmdbIndex>();
    index->kind = kind; index->content = content;
    std::map<std::uint64_t, std::vector<std::string>> labels;
    std::size_t visits = 0, prefixes = 0, decodedValues = 0, decodedPayload = 0;
    const auto label = [&](std::uint64_t record) -> const std::vector<std::string>& {
        if (const auto found = labels.find(record); found != labels.end()) return found->second;
        if (labels.size() >= 2'000'000 || record < nodes + 16) throw std::runtime_error("MMDB 数据指针无效或过多");
        std::size_t offset = record - nodes - 16;
        data.values = data.payload = 0;
        const auto value = data.decode(offset);
        decodedValues += data.values; decodedPayload += data.payload;
        if (decodedValues > 32'000'000 || decodedPayload > 256 * 1024 * 1024)
            throw std::runtime_error("MMDB 全库解码工作量超限");
        std::vector<std::string> keys;
        const auto addCode = [&](const nlohmann::json& code) {
            if (!code.is_string()) throw std::runtime_error("MMDB 国家分类类型无效");
            const auto text = lowerCopy(code.get<std::string>());
            if (!text.empty()) {
                if (!geoCode(text)) throw std::runtime_error("MMDB 国家分类无效");
                if (std::ranges::find(keys, text) == keys.end()) keys.push_back(text);
            }
        };
        if (kind == "asn") {
            std::uint64_t asn = 0;
            if (!value.is_object()) throw std::runtime_error("MMDB ASN 记录类型无效");
            if (type == "ipinfo generic_asn_free.mmdb") {
                const auto text = value.value("asn", std::string{});
                if (!text.starts_with("AS")) throw std::runtime_error("MMDB ASN 编号无效");
                const auto [end, ec] = std::from_chars(text.data() + 2, text.data() + text.size(), asn);
                if (ec != std::errc{} || end != text.data() + text.size()) throw std::runtime_error("MMDB ASN 编号无效");
            } else if (value.contains("autonomous_system_number") && value["autonomous_system_number"].is_number_unsigned())
                asn = value["autonomous_system_number"].get<std::uint64_t>();
            else throw std::runtime_error("MMDB 缺少 ASN 编号");
            if (!asn || asn > 0xffffffff) throw std::runtime_error("MMDB ASN 编号超限");
            keys.push_back(std::to_string(asn));
        } else if (type == "sing-geoip") addCode(value);
        else if (type == "Meta-geoip0") {
            if (value.is_array()) {
                if (value.size() > 64) throw std::runtime_error("MMDB 多分类超过上限");
                for (const auto& code : value) addCode(code);
            } else addCode(value);
        } else {
            if (!value.is_object()) throw std::runtime_error("MMDB 国家记录类型无效");
            if (value.contains("country")) {
                const auto& country = value["country"];
                if (!country.is_object()) throw std::runtime_error("MMDB country 类型无效");
                if (country.contains("iso_code")) addCode(country["iso_code"]);
            }
        }
        return labels.emplace(record, std::move(keys)).first->second;
    };
    const auto append = [&](const auto& keys, const std::array<unsigned char, 16>& address, unsigned prefix, bool v4) {
        for (const auto& key : keys) {
            if (++prefixes > 4'000'000 || index->entries.size() >= 200000)
                throw std::runtime_error("MMDB 展开网段超过上限");
            index->entries[key].push_back({address, static_cast<unsigned char>(prefix), v4});
        }
    };
    std::set<std::uint64_t> active;
    const auto walk = [&](auto&& self, std::uint64_t node, unsigned depth, std::array<unsigned char, 16> address) -> void {
        if (++visits > 16'000'000) throw std::runtime_error("MMDB 搜索树遍历超限");
        if (node == nodes) return;
        if (node > nodes) {
            const auto& keys = label(node);
            if (ip == 4) append(keys, address, depth, true);
            else {
                append(keys, address, depth, false);
                if (std::ranges::all_of(address.begin(), address.begin() + 12, [](auto b) { return b == 0; }))
                    append(keys, address, depth > 96 ? depth - 96 : 0, true);
            }
            return;
        }
        if (depth >= (ip == 4 ? 32 : 128) || !active.insert(node).second)
            throw std::runtime_error("MMDB 搜索树成环或深度无效");
        const auto base = node * nodeSize;
        for (unsigned side = 0; side < 2; ++side) {
            std::size_t position = base + (bits == 28 ? (side ? 4 : 0) : side * (bits / 8));
            MmdbDecoder tree{std::string_view(content).substr(0, treeBytes)};
            auto record = tree.number(position, bits == 28 ? 3 : bits / 8);
            if (bits == 28) record |= std::uint64_t((static_cast<unsigned char>(content[base + 3]) >> (side ? 0 : 4)) & 15) << 24;
            auto next = address;
            if (side) {
                const unsigned bit = (ip == 4 ? 96 : 0) + depth;
                next[bit / 8] |= 1U << (7 - bit % 8);
            }
            self(self, record, depth + 1, next);
        }
        active.erase(node);
    };
    walk(walk, 0, 0, {});
    if (index->entries.empty()) throw std::runtime_error("MMDB 不含可用的国家或 ASN 记录");
    if (cache.size() == 2) cache.erase(cache.begin());
    cache.push_back(index);
    return index;
}
