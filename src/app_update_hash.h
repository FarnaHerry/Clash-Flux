#pragma once

#include <array>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include "win32_raii.h"
#include <bcrypt.h>
#else
#include <openssl/evp.h>
#endif

namespace clashflux::update {

struct CloseFile {
    void operator()(std::FILE* file) const noexcept { std::fclose(file); }
};
using FileOwner = std::unique_ptr<std::FILE, CloseFile>;

// Shared by the downloader and the independent installer handoff. Hashing is
// incremental, including large packages; no package-sized allocation is needed.
inline void VerifyStream(std::FILE* file, std::uint64_t expectedSize, std::string_view expectedHash) {
    std::array<unsigned char, 64 * 1024> buffer{};
    std::array<unsigned char, 32> digest{};
    std::uint64_t size = 0;
#if defined(_WIN32)
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Cannot initialize update verification");
    const auto closeAlgorithm = [](BCRYPT_ALG_HANDLE value) { BCryptCloseAlgorithmProvider(value, 0); };
    std::unique_ptr<void, decltype(closeAlgorithm)> algorithmOwner(algorithm, closeAlgorithm);
    BCRYPT_HASH_HANDLE hash{};
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0)
        throw std::runtime_error("Cannot initialize update hash");
    const auto closeHash = [](BCRYPT_HASH_HANDLE value) { BCryptDestroyHash(value); };
    std::unique_ptr<void, decltype(closeHash)> hashOwner(hash, closeHash);
#else
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> hash(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!hash || EVP_DigestInit_ex(hash.get(), EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("Cannot initialize update verification");
#endif
    for (;;) {
        const auto count = std::fread(buffer.data(), 1, buffer.size(), file);
        size += count;
        if (size > expectedSize) throw std::runtime_error("Update package exceeds the declared size");
#if defined(_WIN32)
        if (BCryptHashData(hash, buffer.data(), static_cast<ULONG>(count), 0) < 0)
#else
        if (EVP_DigestUpdate(hash.get(), buffer.data(), count) != 1)
#endif
            throw std::runtime_error("Cannot hash update package");
        if (count < buffer.size()) {
            if (std::ferror(file)) throw std::runtime_error("Cannot read update package");
            break;
        }
    }
#if defined(_WIN32)
    if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
#else
    unsigned length = 0;
    if (EVP_DigestFinal_ex(hash.get(), digest.data(), &length) != 1 || length != digest.size())
#endif
        throw std::runtime_error("Cannot finish update verification");
    std::string actual;
    for (const auto value : digest) {
        actual += "0123456789abcdef"[value >> 4];
        actual += "0123456789abcdef"[value & 15];
    }
    if (size != expectedSize || actual != expectedHash)
        throw std::runtime_error("Update package size or SHA-256 mismatch");
}

inline void VerifyFile(const std::filesystem::path& path, std::uint64_t size, std::string_view hash) {
    if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path)))
        throw std::runtime_error("Update package is not a regular file");
#if defined(_WIN32)
    std::FILE* raw = _wfopen(path.c_str(), L"rb");
#else
    std::FILE* raw = std::fopen(path.c_str(), "rb");
#endif
    const FileOwner file(raw);
    if (!file) throw std::runtime_error("Cannot open update package");
    VerifyStream(file.get(), size, hash);
}

} // namespace clashflux::update
