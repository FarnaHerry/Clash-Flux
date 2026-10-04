// MRSv1 wire decoder: bounded Zstandard frame + domain succinct trie/IP ranges.
// Format references: MetaCubeX/mihomo rules/provider/mrs_reader.go and
// component/{trie/domain_set_bin,cidr/ipcidr_set_bin}.go. No Mihomo runtime.
std::optional<std::vector<std::string>> mrsEntries(const std::string& compressed,
        std::string_view behavior, std::string& failure) {
    const auto fail = [&](std::string reason) -> std::optional<std::vector<std::string>> {
        failure = "MRS: " + std::move(reason); return std::nullopt;
    };
    if ((behavior != "domain" && behavior != "ipcidr") || compressed.empty() ||
        compressed.size() > 8 * 1024 * 1024) return fail("behavior 或压缩大小无效");
    std::unique_ptr<ZSTD_DStream, decltype(&ZSTD_freeDStream)> decoder(ZSTD_createDStream(), ZSTD_freeDStream);
    if (!decoder || ZSTD_isError(ZSTD_DCtx_setParameter(decoder.get(), ZSTD_d_windowLogMax, 25)))
        return fail("无法初始化解压器");
    ZSTD_inBuffer input{compressed.data(), compressed.size(), 0};
    std::string data;
    std::array<char, 65536> chunk;
    std::size_t remaining = 1;
    while (remaining) {
        ZSTD_outBuffer output{chunk.data(), chunk.size(), 0};
        const auto before = input.pos;
        remaining = ZSTD_decompressStream(decoder.get(), &output, &input);
        if (ZSTD_isError(remaining) || output.pos > 32 * 1024 * 1024 - data.size())
            return fail("损坏或解压超过 32 MiB 上限");
        data.append(chunk.data(), output.pos);
        if (remaining && before == input.pos && output.pos == 0) return fail("压缩帧被截断");
    }
    if (input.pos != input.size) return fail("压缩帧存在附加数据");
    std::size_t offset = 0;
    const auto byte = [&]() -> unsigned {
        if (offset >= data.size()) throw std::invalid_argument("数据被截断");
        return static_cast<unsigned char>(data[offset++]);
    };
    const auto integer = [&]() -> std::uint64_t {
        std::uint64_t value = 0;
        for (unsigned i = 0; i < 8; ++i) value = (value << 8) | byte();
        return value;
    };
    std::vector<std::string> payload;
    try {
        if (byte() != 'M' || byte() != 'R' || byte() != 'S' || byte() != 1)
            return fail("未知 magic/version");
        if (byte() != (behavior == "domain" ? 0u : 1u)) return fail("声明与文件 behavior 不一致");
        const auto count = integer();
        if (!count || count > 1000000 || integer() != 0) return fail("规则数量或扩展头不支持");
        if (byte() != 1) return fail("未知集合版本");
        payload.reserve(static_cast<std::size_t>(count));
        if (behavior == "domain") {
            const auto bitmap = [&]() {
                const auto size = integer();
                if (!size || size > (data.size() - offset) / 8) throw std::invalid_argument("位图长度无效");
                std::vector<std::uint64_t> result;
                result.reserve(size);
                for (std::uint64_t i = 0; i < size; ++i) result.push_back(integer());
                return result;
            };
            const auto leaves = bitmap();
            const auto edges = bitmap();
            const auto size = integer();
            if (!size || size > data.size() - offset || size > 4000000) return fail("标签长度无效");
            const std::string_view labels(data.data() + offset, size); offset += size;
            const auto bit = [](const auto& words, std::size_t i) {
                return i / 64 < words.size() && ((words[i / 64] >> (i % 64)) & 1) != 0;
            };
            // Each zero is an edge; each one terminates one BFS node. Nodes
            // are numbered by incoming edge + 1, with root node zero.
            std::vector<std::pair<std::size_t, std::size_t>> nodes;
            std::size_t position = 0, edge = 0;
            for (std::size_t node = 0; node <= labels.size(); ++node) {
                const auto start = edge;
                unsigned previous = 0;
                while (position < edges.size() * 64 && !bit(edges, position)) {
                    if (edge >= labels.size()) return fail("树边数无效");
                    const auto label = static_cast<unsigned char>(labels[edge]);
                    if (!label || label >= 128 || (edge != start && label <= previous))
                        return fail("仅支持有序 ASCII 域名标签");
                    previous = label; ++position; ++edge;
                }
                if (position >= edges.size() * 64) return fail("树节点终止标记缺失");
                ++position; nodes.emplace_back(start, edge);
                if (node < labels.size() && edge <= node) return fail("树节点不可达");
            }
            if (leaves.size() * 64 < nodes.size()) return fail("叶位图长度不足");
            if (edge != labels.size()) return fail("树边数不一致");
            for (std::size_t i = position; i < edges.size() * 64; ++i)
                if (bit(edges, i)) return fail("树位图含附加标记");
            for (std::size_t i = nodes.size(); i < leaves.size() * 64; ++i)
                if (bit(leaves, i)) return fail("叶位图越界");
            const auto visit = [&](auto&& self, std::size_t node, std::string& reversed) -> bool {
                if (reversed.size() > 255 || payload.size() >= 1000000) return false;
                if (bit(leaves, node)) {
                    if (reversed.empty()) return false;
                    auto value = reversed;
                    if (value.back() == '+') {
                        if (value.size() < 2 || value[value.size() - 2] != '.') return false;
                        value.pop_back(); // subdomain-only marker
                    }
                    std::reverse(value.begin(), value.end());
                    payload.push_back(value);
                }
                const auto [first, last] = nodes[node];
                for (std::size_t i = first; i < last; ++i) {
                    if (i + 1 <= node) return false;
                    reversed.push_back(labels[i]);
                    if (!self(self, i + 1, reversed)) return false;
                    reversed.pop_back();
                }
                return true;
            };
            std::string reversed;
            if (!visit(visit, 0, reversed)) return fail("域名树深度、数量或路径无效");
        } else {
            const auto size = integer();
            if (!size || size > (data.size() - offset) / 32) return fail("IP 区间数量无效");
            using Address = std::array<unsigned char, 16>;
            const auto mapped = [](const Address& value) {
                for (unsigned i = 0; i < 10; ++i) if (value[i]) return false;
                return value[10] == 255 && value[11] == 255;
            };
            Address previous{}; bool havePrevious = false;
            for (std::uint64_t range = 0; range < size; ++range) {
                Address first{}, last{};
                for (auto& part : first) part = byte();
                for (auto& part : last) part = byte();
                if (first > last || mapped(first) != mapped(last) ||
                    (havePrevious && (mapped(first) == mapped(previous)
                        ? first <= previous : mapped(first)))) return fail("IP 区间逆序、交叠或跨地址族");
                previous = last; havePrevious = true;
                const bool ipv4 = mapped(first);
                const unsigned minimum = ipv4 ? 96 : 0;
                while (true) {
                    unsigned prefix = 128;
                    Address end = first;
                    while (prefix > minimum) {
                        const unsigned b = prefix - 1;
                        if ((first[b / 8] >> (7 - b % 8)) & 1) break;
                        auto next = end; next[b / 8] |= 1 << (7 - b % 8);
                        if (next > last) break;
                        end = next; --prefix;
                    }
                    std::string cidr;
                    if (ipv4) cidr = std::format("{}.{}.{}.{}/{}", first[12], first[13], first[14], first[15], prefix - 96);
                    else {
                        for (unsigned i = 0; i < 16; i += 2) {
                            if (i) cidr += ':';
                            cidr += std::format("{:x}", (unsigned(first[i]) << 8) | first[i + 1]);
                        }
                        cidr += '/' + std::to_string(prefix);
                    }
                    if (payload.size() >= 1000000) return fail("展开网段超过数量上限");
                    payload.push_back(cidr);
                    if (end == last) break;
                    first = end;
                    for (unsigned i = 16; i-- > 0;) if (++first[i] != 0) break;
                }
            }
        }
        if (offset != data.size() || payload.size() == 0) return fail("集合为空或包含附加数据");
    } catch (const std::exception& error) { return fail(error.what()); }
    return payload;
}
