#pragma once

#include <array>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "../wire_codec.h"

namespace clashflux::ui {

inline std::optional<std::array<unsigned, 3>> StableVersion(std::string_view tag) {
    if (tag.starts_with('v')) tag.remove_prefix(1);
    std::array<unsigned, 3> parts{};
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto end = i == 2 ? tag.size() : tag.find('.');
        if (end == std::string_view::npos || end == 0) return std::nullopt;
        const auto [ptr, error] = std::from_chars(tag.data(), tag.data() + end, parts[i]);
        if (error != std::errc{} || ptr != tag.data() + end) return std::nullopt;
        tag.remove_prefix(end);
        if (i != 2) tag.remove_prefix(1);
    }
    return parts;
}

struct AppUpdate {
    std::string tag;
    bool newer = false;
    std::string error;
};

inline AppUpdate ReadAppUpdate(std::string_view body, std::string_view current) {
    auto release = wire::DecodeStableRelease(body);
    if (!release) return {{}, false, std::move(release.error)};
    const auto latest = StableVersion(release.value.tag);
    const auto installed = StableVersion(current);
    if (!latest || !installed) return {{}, false, "unsupported release version"};
    return {std::move(release.value.tag), *latest > *installed, {}};
}

// Tags are validated before use; do not launch an arbitrary URL from a response.
inline std::string AppReleaseUrl(const std::string& tag) {
    return StableVersion(tag)
        ? "https://github.com/FarnaHerry/Clash-Flux/releases/tag/" + tag : std::string{};
}

} // namespace clashflux::ui
