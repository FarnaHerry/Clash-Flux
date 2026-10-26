#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace clashflux::wire {
// Persist user intent, never generated outbound tags or rewritten YAML.
struct ProfileRuleEdit {
    std::string id;
    std::string operation = "prepend"; // prepend, before_final, replace, disable, final
    std::string anchor; // Exact canonical source rule, not its changing list index.
    bool enabled = true;
    std::string match = "domain_suffix";
    std::string pattern;
    std::string target = "DIRECT"; // Source-local object name, DIRECT, REJECT, REJECT-DROP.
    std::string unavailable = "reject";
    bool operator==(const ProfileRuleEdit&) const = default;
};
struct ProfileRuleEdits {
    int format_version = 1;
    std::uint64_t revision = 0;
    std::vector<ProfileRuleEdit> rules;
    bool operator==(const ProfileRuleEdits&) const = default;
};
// Shared resource library. Profile edits reference stable IDs, not display names.
struct PersonalRuleSet {
    std::string id, name;
    std::string type = "remote"; // remote or inline (imported source JSON snapshot)
    std::string format = "binary"; // binary (.srs) or source (sing-box JSON)
    std::string url, content;
    std::uint32_t update_hours = 24;
    std::uint64_t refresh_generation = 0; // Explicit forced fetch; old native cache stays available for rollback.
    bool operator==(const PersonalRuleSet&) const = default;
};
struct RuleSetLibrary {
    int format_version = 1;
    std::uint64_t revision = 0;
    std::vector<PersonalRuleSet> resources;
    bool operator==(const RuleSetLibrary&) const = default;
};
} // namespace clashflux::wire
