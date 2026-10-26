#pragma once

#include "../../../src/win32_raii.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace clashflux::installer {

inline constexpr wchar_t legacy_upgrade_code[] = L"{6D6BD18F-EDA4-5D11-BA5A-DB11FB4C6A69}";

inline void CheckRegistry(LSTATUS result) {
    if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND && result != ERROR_PATH_NOT_FOUND)
        throw std::runtime_error("Clash-Flux legacy bundle registry operation failed: " + std::to_string(result));
}

inline std::vector<std::wstring> Subkeys(HKEY root, const std::wstring& path) {
    win32::UniqueHkey key;
    const auto opened = RegOpenKeyExW(root, path.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, key.put());
    CheckRegistry(opened);
    std::vector<std::wstring> names;
    if (opened != ERROR_SUCCESS) return names;
    for (DWORD index = 0;; ++index) {
        std::array<wchar_t, 256> name{};
        DWORD size = static_cast<DWORD>(name.size());
        const auto result = RegEnumKeyExW(key.get(), index, name.data(), &size, nullptr, nullptr, nullptr, nullptr);
        if (result == ERROR_NO_MORE_ITEMS) return names;
        CheckRegistry(result);
        names.emplace_back(name.data(), size);
    }
}

inline std::wstring StringValue(HKEY key, const wchar_t* name, DWORD type = RRF_RT_REG_SZ) {
    DWORD bytes = 0;
    if (RegGetValueW(key, nullptr, name, type, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes > 32768 || bytes < sizeof(wchar_t)) return {};
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(key, nullptr, name, type, nullptr, value.data(), &bytes) != ERROR_SUCCESS) return {};
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

inline bool IsGuid(const std::wstring& value) {
    if (value.size() != 38 || value.front() != L'{' || value.back() != L'}') return false;
    for (size_t i = 1; i < 37; ++i) {
        if (i == 9 || i == 14 || i == 19 || i == 24) {
            if (value[i] != L'-') return false;
        } else if (!((value[i] >= L'0' && value[i] <= L'9') || (value[i] >= L'a' && value[i] <= L'f') ||
                     (value[i] >= L'A' && value[i] <= L'F'))) return false;
    }
    return true;
}

inline std::optional<std::array<unsigned, 4>> ParseVersion(const std::wstring& value) {
    std::array<unsigned, 4> version{};
    size_t offset = 0;
    for (size_t field = 0; field < version.size(); ++field) {
        if (offset == value.size()) return std::nullopt;
        size_t digits = 0;
        while (offset < value.size() && value[offset] >= L'0' && value[offset] <= L'9') {
            if (++digits > 5) return std::nullopt;
            version[field] = version[field] * 10 + value[offset++] - L'0';
        }
        if (!digits || version[field] > 65535) return std::nullopt;
        if (offset == value.size()) return version;
        if (value[offset++] != L'.') return std::nullopt;
    }
    return std::nullopt;
}

inline bool IsLegacyVersion(const std::wstring& value) {
    const auto version = ParseVersion(value);
    return version && *version >= std::array<unsigned, 4>{0, 3, 0, 0} &&
           *version <= std::array<unsigned, 4>{0, 3, 68, 0};
}

inline bool Same(const std::wstring& left, const std::wstring& right) {
    return left.size() == right.size() && left.find(L'\0') == std::wstring::npos &&
           right.find(L'\0') == std::wstring::npos && _wcsicmp(left.c_str(), right.c_str()) == 0;
}

inline void DeleteKey(HKEY root, const std::wstring& path) {
    win32::UniqueHkey key;
    const auto opened = RegOpenKeyExW(root, path.c_str(), 0, KEY_READ | KEY_WRITE | DELETE | KEY_WOW64_64KEY, key.put());
    CheckRegistry(opened);
    if (opened != ERROR_SUCCESS) return;
    CheckRegistry(RegDeleteTreeW(key.get(), nullptr));
    key.reset();
    CheckRegistry(RegDeleteKeyExW(root, path.c_str(), KEY_WOW64_64KEY, 0));
}

inline void RemoveCache(const std::filesystem::path& cache_root, const std::wstring& id,
                        const std::wstring& version, const std::wstring& registered_cache) {
    const auto directory = cache_root / id;
    const auto executable = directory / (L"clash-flux-Setup-" + version + L".exe");
    if (!Same(executable.wstring(), registered_cache))
        throw std::runtime_error("Clash-Flux legacy cache path does not match its bundle identity");
    for (auto path = directory; !path.empty() && path != path.root_path(); path = path.parent_path()) {
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Clash-Flux legacy cache contains a reparse point");
    }
    // Each bundle owns only its executable and state. MSI payload caches may be shared.
    for (const auto& path : {executable, directory / L"state.rsm"}) {
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
            throw std::runtime_error("Clash-Flux legacy cache file is not an ordinary file");
        if (!DeleteFileW(path.c_str())) {
            const auto error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                throw std::runtime_error("Clash-Flux legacy cache file could not be removed");
        }
    }
    if (!RemoveDirectoryW(directory.c_str())) {
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
            throw std::runtime_error("Clash-Flux legacy cache directory could not be removed");
    }
}

template <class Log>
void CleanupLegacyBundles(HKEY root, const std::wstring& uninstall_path, const std::wstring& dependency_path,
                          const std::filesystem::path& cache_root, Log log) {
    for (const auto& id : Subkeys(root, uninstall_path)) {
        if (!IsGuid(id)) continue;
        win32::UniqueHkey key;
        CheckRegistry(RegOpenKeyExW(root, (uninstall_path + L"\\" + id).c_str(), 0,
                                    KEY_READ | KEY_WOW64_64KEY, key.put()));
        if (!key.valid() || StringValue(key.get(), L"DisplayName") != L"Clash-Flux" ||
            StringValue(key.get(), L"Publisher") != L"dev.farna.clashflux" ||
            !Same(StringValue(key.get(), L"BundleUpgradeCode", RRF_RT_REG_MULTI_SZ), legacy_upgrade_code) ||
            !Same(StringValue(key.get(), L"BundleProviderKey"), id)) continue;
        const auto version = StringValue(key.get(), L"DisplayVersion");
        if (!IsLegacyVersion(version)) continue;
        const auto cache = StringValue(key.get(), L"BundleCachePath");
        key.reset();
        const auto provider = dependency_path + L"\\" + id;
        const auto dependents = Subkeys(root, provider + L"\\Dependents");
        if (std::any_of(dependents.begin(), dependents.end(), [&](const auto& dependent) { return !Same(dependent, id); })) {
            log(L"Clash-Flux: retained legacy bundle with another dependent: " + id);
            continue;
        }
        RemoveCache(cache_root, id, version, cache);
        for (const auto& name : Subkeys(root, dependency_path))
            DeleteKey(root, dependency_path + L"\\" + name + L"\\Dependents\\" + id);
        DeleteKey(root, provider);
        DeleteKey(root, uninstall_path + L"\\" + id);
        log(L"Clash-Flux: removed legacy bundle registration and private cache: " + id);
    }
}

} // namespace clashflux::installer
