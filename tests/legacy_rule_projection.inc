// A read-only projection using the same yaml-cpp scanner/parser as the DOM
// path. Complex YAML falls back to that path; no source text is rewritten.
struct RuleProjectionFallback {};

// A common subscription has a block sequence of single-line rules, either
// plain or quoted without escapes.
// Parse those values directly, and ask yaml-cpp to validate the entire document
// with this one sequence represented as a literal scalar. Its scanner still
// consumes every byte, including UTF-8; the selected node's source mark proves
// that the candidate header really was the root rules key, not quoted content.
std::optional<std::vector<ProfileRule>> projectPlainRuleBlock(const std::string& content) {
    std::size_t header = 0, headerLine = 0;
    while (header < content.size()) {
        const auto end = content.find('\n', header);
        const auto line = std::string_view(content).substr(header, end == std::string::npos ? end : end - header);
        const auto first = line.find_first_not_of(" \r");
        if (first != std::string_view::npos && line[first] == '&') return std::nullopt;
        if (line.starts_with("rules:")) {
            const auto rest = line.substr(6);
            const auto token = rest.find_first_not_of(" \r");
            if ((rest.empty() || rest.front() == ' ' || rest.front() == '\r') &&
                (token == std::string_view::npos || rest[token] == '#')) break;
        }
        if (end == std::string::npos) return std::nullopt;
        header = end + 1; ++headerLine;
    }
    const auto headerEnd = content.find('\n', header);
    if (headerEnd == std::string::npos) return std::nullopt;
    std::size_t cursor = headerEnd + 1;
    std::optional<std::size_t> indentation;
    std::vector<ProfileRule> rows;
    std::string literal = content.substr(0, header + 6) + " |" +
        content.substr(header + 6, headerEnd + 1 - header - 6);
    while (cursor < content.size()) {
        const auto end = content.find('\n', cursor);
        const auto length = end == std::string::npos ? content.size() - cursor : end - cursor;
        auto line = std::string_view(content).substr(cursor, length);
        if (line.ends_with('\r')) line.remove_suffix(1);
        const auto indent = line.find_first_not_of(' ');
        if (indent != std::string_view::npos && line[indent] != '#') {
            if (indent == 0 && !line.starts_with("- ")) break;
            if (!line.substr(indent).starts_with("- ") || (indentation && *indentation != indent)) return std::nullopt;
            indentation = indent;
            auto value = line.substr(indent + 2);
            const auto first = value.find_first_not_of(' ');
            if (first == std::string_view::npos) return std::nullopt;
            value.remove_prefix(first);
            while (value.ends_with(' ')) value.remove_suffix(1);
            const bool quoted = value.front() == '\'' || value.front() == '"';
            if (quoted) {
                if (value.size() < 2 || value.back() != value.front()) return std::nullopt;
                const auto quote = value.front();
                value = value.substr(1, value.size() - 2);
                if (value.empty() || value.find(quote) != std::string_view::npos ||
                    value.find('\\') != std::string_view::npos) return std::nullopt;
            }
            if (!((value.front() >= 'A' && value.front() <= 'Z') ||
                  (value.front() >= 'a' && value.front() <= 'z'))) return std::nullopt;
            if (value.find(',') == std::string_view::npos || value.find("\xc2\x85") != std::string_view::npos ||
                value.find("\xe2\x80\xa8") != std::string_view::npos ||
                value.find("\xe2\x80\xa9") != std::string_view::npos) return std::nullopt;
            for (std::size_t i = 0; i < value.size(); ++i) {
                const auto c = static_cast<unsigned char>(value[i]);
                if (c < 0x20 || c == 0x7f || (!quoted &&
                    (std::string_view{"#[]{}\"'\\"}.find(c) != std::string_view::npos ||
                     (c == ':' && (i + 1 == value.size() || value[i + 1] == ' '))))) return std::nullopt;
            }
            rows.push_back(clashProfileRule(std::string(value)));
        }
        literal += ' ';
        literal.append(content, cursor, length);
        if (end != std::string::npos) literal += '\n';
        cursor += length + (end != std::string::npos);
    }
    if (rows.empty()) return std::nullopt;
    literal.append(content, cursor);
    try {
        const auto root = YAML::Load(literal);
        if (!root.IsMap()) return std::nullopt;
        const auto rules = root["rules"];
        if (!rules || !rules.IsScalar() || rules.Mark().line != static_cast<int>(headerLine)) return std::nullopt;
        std::size_t validated = 0;
        for (const auto& row : rows) {
            const auto found = rules.Scalar().find(row.raw, validated);
            if (found == std::string::npos) return std::nullopt;
            validated = found + row.raw.size();
        }
        if (!std::ranges::any_of(rows, [](const auto& row) { return row.terminal; })) {
            auto fallback = clashProfileRules(root);
            rows.push_back(std::move(fallback.back()));
        }
        return rows;
    } catch (const YAML::Exception&) {
        // Report original-document diagnostics through the general parser.
        return std::nullopt;
    }
}

class RuleProjectionEvents final : public YAML::EventHandler {
    enum class Kind { Root, Rules, Groups, Group, Skip };
    struct Frame {
        Kind kind;
        bool keyNext = true, hasName = false, hasType = false;
        std::string key, name, type;
    };
    std::vector<Frame> stack_;
    bool rootMap_ = false, seenRules_ = false, seenGroups_ = false, terminal_ = false;
    std::optional<std::string> defaultTarget_;
    std::vector<ProfileRule> rows_;

    static void Check(const std::string& tag, YAML::anchor_t anchor) {
        if (anchor || (!tag.empty() && tag != "?" && tag != "!")) throw RuleProjectionFallback{};
    }
    Kind Collection(bool sequence) {
        if (stack_.empty()) {
            if (sequence || rootMap_) throw RuleProjectionFallback{};
            rootMap_ = true;
            return Kind::Root;
        }
        auto& parent = stack_.back();
        if (parent.kind == Kind::Root) {
            if (parent.keyNext) throw RuleProjectionFallback{};
            parent.keyNext = true;
            if (sequence && parent.key == "rules") return Kind::Rules;
            if (sequence && parent.key == "proxy-groups") return Kind::Groups;
        } else if (parent.kind == Kind::Rules) {
            throw RuleProjectionFallback{};
        } else if (parent.kind == Kind::Groups) {
            if (sequence) throw RuleProjectionFallback{};
            return Kind::Group;
        } else if (parent.kind == Kind::Group) {
            if (parent.keyNext || parent.key == "name" || parent.key == "type") throw RuleProjectionFallback{};
            parent.keyNext = true;
        }
        return Kind::Skip;
    }
    void Scalar(const std::string& value) {
        if (stack_.empty()) throw RuleProjectionFallback{};
        auto& parent = stack_.back();
        if (parent.kind == Kind::Rules) {
            auto row = clashProfileRule(value);
            terminal_ |= row.terminal;
            rows_.push_back(std::move(row));
        } else if (parent.kind == Kind::Groups) {
            throw RuleProjectionFallback{};
        } else if (parent.kind == Kind::Root || parent.kind == Kind::Group) {
            if (parent.keyNext) {
                parent.key = value;
                if (parent.kind == Kind::Root) {
                    if (value == "<<") throw RuleProjectionFallback{};
                    if (value == "rules" && std::exchange(seenRules_, true)) throw RuleProjectionFallback{};
                    if (value == "proxy-groups" && std::exchange(seenGroups_, true)) throw RuleProjectionFallback{};
                } else {
                    if (value == "<<") throw RuleProjectionFallback{};
                    if (value == "name" && std::exchange(parent.hasName, true)) throw RuleProjectionFallback{};
                    if (value == "type" && std::exchange(parent.hasType, true)) throw RuleProjectionFallback{};
                }
            } else if (parent.kind == Kind::Group) {
                if (parent.key == "name") parent.name = trimCopy(value);
                if (parent.key == "type") parent.type = trimCopy(value);
            }
            parent.keyNext = !parent.keyNext;
        }
    }

public:
    void OnDocumentStart(const YAML::Mark&) override {}
    void OnDocumentEnd() override {}
    void OnNull(const YAML::Mark&, YAML::anchor_t anchor) override {
        if (anchor || stack_.empty() || stack_.back().kind == Kind::Rules ||
            ((stack_.back().kind == Kind::Root || stack_.back().kind == Kind::Group) &&
             stack_.back().keyNext)) throw RuleProjectionFallback{};
        Scalar("");
    }
    void OnAlias(const YAML::Mark&, YAML::anchor_t) override { throw RuleProjectionFallback{}; }
    void OnScalar(const YAML::Mark&, const std::string& tag,
                  YAML::anchor_t anchor, const std::string& value) override {
        Check(tag, anchor);
        Scalar(value);
    }
    void OnSequenceStart(const YAML::Mark&, const std::string& tag,
                         YAML::anchor_t anchor, YAML::EmitterStyle::value) override {
        Check(tag, anchor);
        stack_.push_back({Collection(true)});
    }
    void OnSequenceEnd() override { stack_.pop_back(); }
    void OnMapStart(const YAML::Mark&, const std::string& tag,
                    YAML::anchor_t anchor, YAML::EmitterStyle::value) override {
        Check(tag, anchor);
        stack_.push_back({Collection(false)});
    }
    void OnMapEnd() override {
        const auto& frame = stack_.back();
        if (frame.kind == Kind::Group && frame.type == "select" && !defaultTarget_) defaultTarget_ = frame.name;
        stack_.pop_back();
    }
    std::vector<ProfileRule> Finish() {
        if (!rootMap_) throw RuleProjectionFallback{};
        if (!terminal_) rows_.push_back({.anchor = "implicit:final", .type = "MATCH",
            .target = defaultTarget_.value_or("DIRECT"), .editable = true, .terminal = true, .implicit = true});
        return std::move(rows_);
    }
};

std::optional<std::vector<ProfileRule>> projectClashRuleDocument(const std::string& content) {
    if (auto rows = projectPlainRuleBlock(content)) return rows;
    try {
        std::istringstream input(content);
        YAML::Parser parser(input);
        RuleProjectionEvents events;
        if (!parser.HandleNextDocument(events)) return std::nullopt;
        return events.Finish();
    } catch (const RuleProjectionFallback&) {
        return std::nullopt;
    }
}
