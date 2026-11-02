struct ProxyTextFilters {
    std::vector<std::regex> include;
    std::vector<std::regex> exclude;
    std::set<std::string> excludeTypes;
};

std::optional<std::regex> proxyFilterRegex(std::string_view pattern, std::string& error) {
    if (pattern.size() > 512) {
        error = "单个筛选表达式超过 512 字节";
        return std::nullopt;
    }
    auto flags = std::regex_constants::ECMAScript | std::regex_constants::optimize;
    if (pattern.starts_with("(?i)")) {
        pattern.remove_prefix(4);
        flags |= std::regex_constants::icase;
    }
    int parentheses = 0;
    bool escaped = false;
    bool inClass = false;
    for (std::size_t index = 0; index < pattern.size(); ++index) {
        const unsigned char c = pattern[index];
        if (escaped) {
            if (std::string_view("dDsSwWbBfnrtv/\\.^$|()[]{}*+?-").find(c) ==
                std::string_view::npos) {
                error = "筛选含尚未支持的转义";
                return std::nullopt;
            }
            escaped = false;
            continue;
        }
        if (c == '\\') { escaped = true; continue; }
        if (c == '[' && !inClass) { inClass = true; continue; }
        if (c == ']' && inClass) { inClass = false; continue; }
        if (inClass) continue;
        if (c == '(') {
            if (index + 1 < pattern.size() && pattern[index + 1] == '?' &&
                (index + 2 >= pattern.size() || pattern[index + 2] != ':')) {
                error = "筛选含尚未支持的 .NET 分组或内联选项";
                return std::nullopt;
            }
            for (std::size_t nested = index + 1; nested < pattern.size(); ++nested) {
                if (pattern[nested] == '\\') { ++nested; continue; }
                if (pattern[nested] == '[') {
                    while (++nested < pattern.size() && pattern[nested] != ']')
                        if (pattern[nested] == '\\') ++nested;
                    continue;
                }
                if (pattern[nested] == ')') break;
                if (pattern[nested] == '*' || pattern[nested] == '+' ||
                    pattern[nested] == '?' || pattern[nested] == '{') {
                    error = "筛选暂不支持分组内量词，以避免不可控回溯";
                    return std::nullopt;
                }
            }
            ++parentheses;
        } else if (c == ')') {
            if (--parentheses < 0) {
                error = "筛选括号不匹配";
                return std::nullopt;
            }
        }
    }
    if (escaped || inClass || parentheses != 0) {
        error = "筛选转义、字符类或括号不完整";
        return std::nullopt;
    }
    try {
        return std::regex{std::string(pattern), flags};
    } catch (const std::regex_error&) {
        error = "筛选表达式不是受支持的 ECMAScript 子集";
        return std::nullopt;
    }
}

std::optional<std::vector<std::regex>> proxyFilterPatterns(
        std::string_view text, std::string& error) {
    std::vector<std::regex> patterns;
    if (text.empty()) return patterns;
    for (std::size_t begin = 0; begin <= text.size();) {
        const auto end = text.find('`', begin);
        const auto part = text.substr(begin, end == std::string_view::npos
            ? std::string_view::npos : end - begin);
        auto pattern = proxyFilterRegex(part, error);
        if (!pattern) return std::nullopt;
        patterns.push_back(std::move(*pattern));
        if (patterns.size() > 16) {
            error = "筛选表达式数量超过 16";
            return std::nullopt;
        }
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return patterns;
}

bool compileProxyTextFilters(std::string_view include, std::string_view exclude,
        std::string_view excludeTypes, ProxyTextFilters& filters, std::string& error) {
    auto includes = proxyFilterPatterns(include, error);
    if (!includes) return false;
    auto excludes = proxyFilterPatterns(exclude, error);
    if (!excludes) return false;
    filters.include = std::move(*includes);
    filters.exclude = std::move(*excludes);
    filters.excludeTypes.clear();
    for (std::size_t begin = 0; begin <= excludeTypes.size();) {
        const auto end = excludeTypes.find('|', begin);
        auto type = lowerCopy(std::string(excludeTypes.substr(begin,
            end == std::string_view::npos ? std::string_view::npos : end - begin)));
        if (!type.empty()) filters.excludeTypes.insert(std::move(type));
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return true;
}

bool proxyFilterMatch(const std::regex& pattern, const std::string& name) {
    return std::regex_search(name, pattern);
}

template<class Match>
std::vector<std::size_t> proxyFilterUnion(const std::vector<std::regex>& patterns,
        const std::vector<std::string>& names, Match match) {
    std::vector<std::size_t> selected;
    std::set<std::size_t> seen;
    for (const auto& pattern : patterns) {
        for (std::size_t index = 0; index < names.size(); ++index) {
            if (seen.contains(index) || !proxyFilterMatch(pattern, names[index])) continue;
            if (!match(index)) continue;
            seen.insert(index);
            selected.push_back(index);
        }
    }
    return selected;
}

bool proxyFilterExcluded(const ProxyTextFilters& filters, const std::string& name) {
    return std::ranges::any_of(filters.exclude, [&](const auto& pattern) {
        return proxyFilterMatch(pattern, name);
    });
}

std::string proxyGroupType(std::string_view singboxType) {
    if (singboxType == "ss") return "shadowsocks";
    if (singboxType == "vmess") return "vmess";
    if (singboxType == "vless") return "vless";
    if (singboxType == "trojan") return "trojan";
    if (singboxType == "hysteria") return "hysteria";
    if (singboxType == "hysteria2") return "hysteria2";
    if (singboxType == "tuic") return "tuic";
    if (singboxType == "socks") return "socks5";
    if (singboxType == "http") return "http";
    if (singboxType == "ssh") return "ssh";
    if (singboxType == "anytls") return "anytls";
    if (singboxType == "snell") return "snell";
    return std::string(singboxType);
}
