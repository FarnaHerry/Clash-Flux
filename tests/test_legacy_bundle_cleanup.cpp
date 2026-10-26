#include "../platform/windows/package/legacy_bundle_cleanup.h"

#include <fstream>
#include <iostream>

namespace {

void Require(bool condition) {
    if (!condition) throw std::runtime_error("Legacy bundle cleanup regression failed");
}

void Set(HKEY key, const wchar_t* name, const std::wstring& value, DWORD type = REG_SZ) {
    auto data = value;
    if (type == REG_MULTI_SZ) data.push_back(L'\0');
    Require(RegSetValueExW(key, name, 0, type, reinterpret_cast<const BYTE*>(data.c_str()),
                          static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS);
}

struct Fixture {
    std::wstring registry = L"SOFTWARE\\ClashFluxLegacyCleanupTest-" + std::to_wstring(GetCurrentProcessId());
    std::filesystem::path cache = std::filesystem::temp_directory_path() /
                                  (L"clashflux-legacy-cleanup-" + std::to_wstring(GetCurrentProcessId()));
    const std::wstring uninstall = registry + L"\\Uninstall";
    const std::wstring dependencies = registry + L"\\Dependencies";

    Fixture() {
        Require(!std::filesystem::exists(cache));
        clashflux::win32::UniqueHkey existing;
        Require(RegOpenKeyExW(HKEY_CURRENT_USER, registry.c_str(), 0, KEY_READ, existing.put()) == ERROR_FILE_NOT_FOUND);
        std::filesystem::create_directories(cache);
    }
    ~Fixture() {
        RegDeleteTreeW(HKEY_CURRENT_USER, registry.c_str());
        std::filesystem::remove_all(cache);
    }
    void Add(const std::wstring& id, const std::wstring& version, bool bundle = true,
             const std::wstring& upgrade = clashflux::installer::legacy_upgrade_code) {
        clashflux::win32::UniqueHkey key;
        Require(RegCreateKeyExW(HKEY_CURRENT_USER, (uninstall + L"\\" + id).c_str(), 0, nullptr, 0,
                                KEY_ALL_ACCESS, nullptr, key.put(), nullptr) == ERROR_SUCCESS);
        Set(key.get(), L"DisplayName", L"Clash-Flux");
        Set(key.get(), L"Publisher", L"dev.farna.clashflux");
        Set(key.get(), L"DisplayVersion", version);
        if (!bundle) return;
        Set(key.get(), L"BundleUpgradeCode", upgrade, REG_MULTI_SZ);
        Set(key.get(), L"BundleProviderKey", id);
        const auto directory = cache / id;
        std::filesystem::create_directory(directory);
        const auto executable = directory / (L"clash-flux-Setup-" + version + L".exe");
        std::ofstream(executable) << "cached bundle";
        std::ofstream(directory / L"state.rsm") << "state";
        Set(key.get(), L"BundleCachePath", executable.wstring());
        key.reset();
        Require(RegCreateKeyExW(HKEY_CURRENT_USER, (dependencies + L"\\" + id + L"\\Dependents\\" + id).c_str(),
                                0, nullptr, 0, KEY_ALL_ACCESS, nullptr, key.put(), nullptr) == ERROR_SUCCESS);
    }
    bool Has(const std::wstring& id) {
        const auto names = clashflux::installer::Subkeys(HKEY_CURRENT_USER, uninstall);
        return std::find(names.begin(), names.end(), id) != names.end();
    }
    void Clean() {
        clashflux::installer::CleanupLegacyBundles(HKEY_CURRENT_USER, uninstall, dependencies, cache,
                                                   [](const auto&) {});
    }
};

} // namespace

int main() {
    using namespace clashflux::installer;
    try {
        for (const auto* value : {L"0.3.0", L"0.3.60", L"0.3.68", L"0.3.68.0"}) Require(IsLegacyVersion(value));
        for (const auto* value : {L"", L"0.3.69", L"0.3.68.1", L"0.3.68.", L"0.3.-1", L"0.3.999999"})
            Require(!IsLegacyVersion(value));
        const std::wstring old = L"{3768F513-99E6-444B-843D-5A6C539B4587}";
        const std::wstring msi = L"{582EC471-FEEE-477D-A8F0-56172E5F685A}";
        const std::wstring current = L"{3DA78CF7-E6F6-4876-8D94-E9BCCEF1A017}";
        const std::wstring future = L"{7D95E6A5-BD18-44B5-84D7-44CA814B95BB}";
        const std::wstring shared = L"{A6B0C880-BAFC-4626-B32F-1CD9E226739E}";
        Fixture fixture;
        fixture.Add(old, L"0.3.61");
        fixture.Add(msi, L"0.3.68", false);
        fixture.Add(current, L"0.3.68", true, L"{00000000-0000-0000-0000-000000000001}");
        fixture.Add(future, L"0.3.69");
        fixture.Add(shared, L"0.3.62");
        clashflux::win32::UniqueHkey dependent;
        Require(RegCreateKeyExW(HKEY_CURRENT_USER,
                                (fixture.dependencies + L"\\" + shared + L"\\Dependents\\AnotherProduct").c_str(),
                                0, nullptr, 0, KEY_ALL_ACCESS, nullptr, dependent.put(), nullptr) == ERROR_SUCCESS);
        dependent.reset();
        std::ofstream(fixture.cache / L"shared-msi-cache") << "must survive";
        fixture.Clean();
        Require(!fixture.Has(old) && !std::filesystem::exists(fixture.cache / old));
        for (const auto& id : {msi, current, future, shared}) Require(fixture.Has(id));
        Require(std::filesystem::exists(fixture.cache / L"shared-msi-cache"));
        fixture.Clean();
        fixture.Add(old, L"0.3.61");
        clashflux::win32::UniqueHkey key;
        Require(RegOpenKeyExW(HKEY_CURRENT_USER, (fixture.uninstall + L"\\" + old).c_str(), 0,
                              KEY_SET_VALUE, key.put()) == ERROR_SUCCESS);
        Set(key.get(), L"BundleCachePath", (fixture.cache / L"shared-msi-cache").wstring());
        key.reset();
        bool rejected = false;
        try { fixture.Clean(); } catch (const std::runtime_error&) { rejected = true; }
        Require(rejected && fixture.Has(old));
        Require(std::filesystem::exists(fixture.cache / L"shared-msi-cache"));
        std::cout << "Legacy bundle cleanup: identity, version, MSI preservation, shared cache, retry and path guards passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
