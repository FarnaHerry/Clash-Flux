#pragma once
#include <algorithm>
#include <filesystem>
#include <ranges>
#include <string>

namespace clashflux::rule_cache {
// A generic native EOF diagnostic cannot identify which set failed. Repair
// only when all file-backed inputs belong to the compiler's explicit GEO list.
// Inline sets have no binary cache; unknown/local/remote user inputs deny repair.
template<class Sets, class Resources>
bool MayRepairGeo(const Sets& sets, const Resources& owned, const std::filesystem::path& workDir) {
    if (!sets.is_array() || sets.empty()) return false;
    for (const auto& set : sets) {
        const auto type = set.value("type", std::string{});
        if (type == "inline") continue;
        if (!std::ranges::any_of(owned, [&](const auto& resource) {
            if (type == "remote") return set.value("url", std::string{}) == resource.url;
            if (type != "local") return false;
            const auto path = std::filesystem::path(set.value("path", std::string{}));
            return path.lexically_normal() == (workDir / (resource.tag + ".srs")).lexically_normal();
        })) return false;
    }
    return true;
}
} // namespace clashflux::rule_cache
