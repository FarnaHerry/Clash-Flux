#include "legacy_bundle_cleanup.h"

#include <msi.h>
#include <msiquery.h>
#include <shlobj.h>

#include <memory>

extern "C" __declspec(dllexport) UINT __stdcall CleanupLegacyBundles(MSIHANDLE installer) noexcept {
    const auto log = [installer](const std::wstring& message) {
        const MSIHANDLE record = MsiCreateRecord(0);
        if (record) {
            MsiRecordSetStringW(record, 0, message.c_str());
            MsiProcessMessage(installer, INSTALLMESSAGE_INFO, record);
            MsiCloseHandle(record);
        }
    };
    try {
        wchar_t* program_data = nullptr;
        const HRESULT result = SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &program_data);
        if (FAILED(result)) throw std::runtime_error("Clash-Flux ProgramData lookup failed");
        const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned_program_data(program_data, CoTaskMemFree);
        const std::filesystem::path cache_root = std::filesystem::path(owned_program_data.get()) / L"Package Cache";
        clashflux::installer::CleanupLegacyBundles(
            HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
            L"SOFTWARE\\Classes\\Installer\\Dependencies", cache_root, log);
        return ERROR_SUCCESS;
    } catch (const std::exception& error) {
        const std::string message = error.what();
        log(L"Clash-Flux legacy bundle cleanup failed: " + std::wstring(message.begin(), message.end()));
        return ERROR_INSTALL_FAILURE;
    } catch (...) {
        log(L"Clash-Flux legacy bundle cleanup failed unexpectedly");
        return ERROR_INSTALL_FAILURE;
    }
}
