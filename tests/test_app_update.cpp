#include "../src/ui/app_update_package.h"
#include <fstream>
#include <iostream>
#include <random>
#include <thread>
#include <chrono>
#if defined(_WIN32)
#include "../src/win32_raii.h"
#endif

using namespace clashflux::ui;
namespace fs = std::filesystem;
static int failures = 0;
static void Check(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
template<class F> static bool Throws(F operation) {
    try { operation(); return false; } catch (const std::exception&) { return true; }
}
static constexpr auto kAbcHash = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

static clashflux::wire::ReleaseAsset Asset(AppUpdateTarget target) {
    const auto name = AppUpdateAssetName("v1.2.3", target);
    return {name, "https://github.com/FarnaHerry/Clash-Flux/releases/download/v1.2.3/" + name,
        std::string("sha256:") + kAbcHash, "uploaded", 3};
}
static std::string Manifest(const clashflux::wire::ReleaseAsset& asset, std::string extra = {}) {
    return R"({"tag_name":"v1.2.3","draft":false,"prerelease":false,"assets":[{"name":")" + asset.name +
        R"(","browser_download_url":")" + asset.browser_download_url + R"(","digest":")" + asset.digest +
        R"(","size":)" + std::to_string(asset.size) + R"(,"state":")" + asset.state + R"("})" + extra + "]}";
}

int main(int argc, char** argv) {
#if defined(_WIN32)
    // Test-only parent uses a harmless, self-contained fake installer. The
    // production helper and package handoff run unchanged, without UAC/MSI.
    if (argc == 7 && std::string_view(argv[1]) == "--handoff") {
        auto asset = Asset(AppUpdateTarget::WindowsX64);
        asset.size = std::stoull(argv[5]);
        asset.digest = "sha256:" + std::string(argv[6]);
        auto candidate = AppUpdatePackage::Prepare(fs::absolute(argv[2]), asset);
        std::ifstream source(argv[4], std::ios::binary);
        std::array<char, 65536> chunk{};
        while (source) {
            source.read(chunk.data(), chunk.size());
            candidate->Write(std::as_bytes(std::span(chunk.data(), static_cast<std::size_t>(source.gcount()))));
        }
        candidate->Commit();
        const auto stage = candidate->Path().parent_path();
        std::ofstream(stage / "expected-parent.txt") << GetCurrentProcessId();
        candidate->StartInstallerAfterExit(fs::absolute(argv[3]));
        std::cout << stage.string() << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        return 0;
    }
#else
    (void)argc; (void)argv;
#endif
    for (const auto target : {AppUpdateTarget::WindowsX64, AppUpdateTarget::LinuxX64Deb,
            AppUpdateTarget::LinuxArm64Deb, AppUpdateTarget::LinuxX64Rpm, AppUpdateTarget::LinuxArm64Rpm,
            AppUpdateTarget::AndroidArm64}) {
        const auto asset = Asset(target);
        const auto manifest = Manifest(asset);
        const auto next = ReadAppUpdate(manifest, "1.2.2", target);
        Check(next.error.empty() && next.newer && next.asset.has_value(), "matching stable package is offered");
        Check(!ReadAppUpdate(manifest, "1.2.3", target).asset &&
              !ReadAppUpdate(manifest, "1.2.4", target).asset, "same/older release cannot be installed");
        Check(!ReadAppUpdate(manifest, "1.2.2").asset, "manual platforms retain only the release link");
        auto invalid = asset;
        invalid.browser_download_url = "https://evil.example/" + asset.name;
        Check(!ReadAppUpdate(Manifest(invalid), "1.2.2", target).asset, "foreign download origin rejected");
        invalid.browser_download_url = asset.browser_download_url + "?redirect=evil";
        Check(!ValidAppUpdateAsset("v1.2.3", invalid, target), "download query injection rejected");
        invalid = asset; invalid.digest = "sha256:" + std::string(64, 'g');
        Check(!ValidAppUpdateAsset("v1.2.3", invalid, target), "nonhex digest rejected");
        invalid = asset; invalid.digest.clear();
        Check(!ReadAppUpdate(Manifest(invalid), "1.2.2", target).asset, "missing digest disables installation");
        invalid = asset; invalid.size = 0;
        Check(!ValidAppUpdateAsset("v1.2.3", invalid, target), "empty package rejected");
        invalid.size = kMaxAppUpdateBytes + 1;
        Check(!ValidAppUpdateAsset("v1.2.3", invalid, target), "oversized package rejected");
        invalid = asset; invalid.state = "new";
        Check(!ValidAppUpdateAsset("v1.2.3", invalid, target), "unfinished upload rejected");
        const auto duplicate = "," + manifest.substr(manifest.find("[{" ) + 1,
            manifest.size() - manifest.find("[{") - 3);
        Check(!ReadAppUpdate(Manifest(asset, duplicate), "1.2.2", target).asset, "ambiguous package names rejected");
        Check(!ReadAppUpdate(Manifest(asset, R"(,{"name":")" + asset.name + R"(","digest":null})"),
            "1.2.2", target).asset, "a malformed duplicate cannot conceal an ambiguous asset");
    }
    Check(AppUpdateAssetName("v1.2.3", AppUpdateTarget::WindowsX64) == "clash-flux-1.2.3-windows-x86_64.exe",
        "Windows installer follows version-platform-architecture release naming");
    auto legacy = Asset(AppUpdateTarget::WindowsX64);
    legacy.name = "clash-flux-Setup-1.2.3.exe";
    legacy.browser_download_url = "https://github.com/FarnaHerry/Clash-Flux/releases/download/v1.2.3/" + legacy.name;
    Check(ReadAppUpdate(Manifest(legacy), "1.2.2", AppUpdateTarget::WindowsX64).asset.has_value(),
        "previously published Windows installer names remain supported");
    Check(!ValidAppUpdateAsset("v1.2.3", legacy, AppUpdateTarget::LinuxX64Deb),
        "legacy Windows names are not accepted on other platforms");
    const auto legacyManifest = Manifest(legacy);
    const auto legacyEntry = "," + legacyManifest.substr(legacyManifest.find("[{") + 1,
        legacyManifest.size() - legacyManifest.find("[{") - 3);
    const auto canonical = Asset(AppUpdateTarget::WindowsX64);
    const auto preferred = ReadAppUpdate(Manifest(canonical, legacyEntry), "1.2.2", AppUpdateTarget::WindowsX64);
    Check(preferred.asset && preferred.asset->name == canonical.name, "canonical Windows name takes precedence");
    auto invalidCanonical = canonical;
    invalidCanonical.digest.clear();
    Check(!ReadAppUpdate(Manifest(invalidCanonical, legacyEntry), "1.2.2", AppUpdateTarget::WindowsX64).asset,
        "invalid canonical metadata cannot fall back to a legacy installer");
    Check(!ReadAppUpdate(Manifest(legacy, legacyEntry), "1.2.2", AppUpdateTarget::WindowsX64).asset,
        "ambiguous legacy installer names remain rejected");
    legacy.browser_download_url = canonical.browser_download_url;
    Check(!ValidAppUpdateAsset("v1.2.3", legacy, AppUpdateTarget::WindowsX64),
        "legacy filename must match its exact download URL");
    const auto root = fs::temp_directory_path() / ("clash-flux-update-test-" + std::to_string(std::random_device{}()));
    fs::create_directory(root);
    const auto cache = root / "cache";
    const auto unrelated = root / "profile.yaml";
    std::ofstream(unrelated) << "user subscription";
    auto asset = Asset(AppUpdateTarget::WindowsX64);
    fs::path stage;
    {
        const auto candidate = AppUpdatePackage::Prepare(cache, asset);
        stage = candidate->Path().parent_path();
        const std::string bytes = "abc";
        candidate->Write(std::as_bytes(std::span(bytes)));
        candidate->Commit();
        Check(candidate->Path().filename() == "package.exe" && fs::exists(candidate->Path()),
            "verified bytes atomically become the install candidate");
        std::ofstream(candidate->Path(), std::ios::binary) << "abd";
        Check(Throws([&] { candidate->StartInstallerAfterExit(root); }), "tampering rejected again before handoff");
        Check(!fs::exists(stage / "ready.txt"), "tampered bytes never start an installer");
    }
    Check(!fs::exists(stage), "unhanded package directory is cleaned");
    for (const auto content : {"", "ab", "abd", "abcd"}) {
        const auto candidate = AppUpdatePackage::Prepare(cache, asset);
        std::ofstream(candidate->Path(), std::ios::binary) << content;
        Check(Throws([&] { candidate->Commit(); }), "empty/truncated/corrupt/oversized download rejected");
        Check(candidate->Path().extension() == ".part", "failure does not expose an executable package");
    }
    {
        auto first = AppUpdatePackage::Prepare(cache, asset);
        auto second = AppUpdatePackage::Prepare(cache, asset);
        Check(first->Path() != second->Path(), "downloads use exclusive staging directories");
        const auto other = second->Path().parent_path();
        first.reset();
        Check(fs::exists(other), "cancelling one download preserves another candidate");
    }
    Check(fs::file_size(unrelated) == 17, "update cleanup preserves unrelated user files");
    fs::remove(unrelated);
    fs::remove(cache / "app-updates");
    fs::remove(cache);
    fs::remove(root);
    std::cout << "app_update: " << failures << " failures\n";
    return failures ? 1 : 0;
}
