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
    std::optional<wire::ReleaseAsset> asset;
};

enum class AppUpdateTarget { Manual, WindowsX64, LinuxX64Deb, LinuxArm64Deb, LinuxX64Rpm, LinuxArm64Rpm, AndroidArm64 };

inline constexpr std::uint64_t kMaxAppUpdateBytes = 512ULL * 1024 * 1024;

inline std::string AppUpdateAssetName(std::string_view tag, AppUpdateTarget target) {
    if (!StableVersion(tag) || target == AppUpdateTarget::Manual) return {};
    if (tag.starts_with('v')) tag.remove_prefix(1);
    const std::string version(tag);
    if (target == AppUpdateTarget::WindowsX64) return "clash-flux-" + version + "-windows-x86_64.exe";
    if (target == AppUpdateTarget::AndroidArm64) return "clash-flux-" + version + "-android-arm64-v8a.apk";
    const bool arm = target == AppUpdateTarget::LinuxArm64Deb || target == AppUpdateTarget::LinuxArm64Rpm;
    const bool deb = target == AppUpdateTarget::LinuxX64Deb || target == AppUpdateTarget::LinuxArm64Deb;
    return "clash-flux-" + version + "-linux-" + (arm ? "aarch64" : "x86_64") + (deb ? ".deb" : ".rpm");
}

inline std::string LegacyWindowsAppUpdateAssetName(std::string_view tag) {
    if (!StableVersion(tag)) return {};
    if (tag.starts_with('v')) tag.remove_prefix(1);
    return "clash-flux-Setup-" + std::string(tag) + ".exe";
}

inline bool ValidAppUpdateAsset(std::string_view tag, const wire::ReleaseAsset& asset, AppUpdateTarget target) {
    const auto name = AppUpdateAssetName(tag, target);
    const auto legacy = target == AppUpdateTarget::WindowsX64 ? LegacyWindowsAppUpdateAssetName(tag) : std::string{};
    if (name.empty() || (asset.name != name && (legacy.empty() || asset.name != legacy)) || asset.state != "uploaded" ||
        asset.size == 0 || asset.size > kMaxAppUpdateBytes ||
        asset.browser_download_url != "https://github.com/FarnaHerry/Clash-Flux/releases/download/" +
            std::string(tag) + "/" + asset.name || asset.digest.size() != 71 || !asset.digest.starts_with("sha256:"))
        return false;
    for (const char c : std::string_view(asset.digest).substr(7))
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

inline AppUpdate ReadAppUpdate(std::string_view body, std::string_view current,
                               AppUpdateTarget target = AppUpdateTarget::Manual) {
    auto release = wire::DecodeStableRelease(body);
    if (!release) return {{}, false, std::move(release.error), {}};
    const auto latest = StableVersion(release.value.tag);
    const auto installed = StableVersion(current);
    if (!latest || !installed) return {{}, false, "unsupported release version", {}};
    AppUpdate result{std::move(release.value.tag), *latest > *installed, {}, {}};
    const std::array names{AppUpdateAssetName(result.tag, target),
        target == AppUpdateTarget::WindowsX64 ? LegacyWindowsAppUpdateAssetName(result.tag) : std::string{}};
    for (const auto& name : names) {
        if (name.empty()) continue;
        unsigned matches = 0;
        for (const auto& asset : release.value.assets) {
            if (asset.name != name) continue;
            ++matches;
            if (ValidAppUpdateAsset(result.tag, asset, target)) result.asset = asset;
        }
        // Only fall back when the canonical name is absent, never when its
        // metadata is invalid or ambiguous.
        if (matches == 0) continue;
        if (!result.newer || matches != 1) result.asset.reset();
        break;
    }
    return result;
}

// Tags are validated before use; do not launch an arbitrary URL from a response.
inline std::string AppReleaseUrl(const std::string& tag) {
    return StableVersion(tag)
        ? "https://github.com/FarnaHerry/Clash-Flux/releases/tag/" + tag : std::string{};
}

} // namespace clashflux::ui
