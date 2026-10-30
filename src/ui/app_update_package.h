#pragma once

#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include "about_update.h"

namespace clashflux::ui {

AppUpdateTarget DetectAppUpdateTarget();
std::string ReadAppUpdateNotice(const std::filesystem::path& cache);

// Own only this download's exclusive directory. Never touch subscriptions or
// another download. A successful handoff transfers cleanup to the helper.
class AppUpdatePackage {
public:
    static std::shared_ptr<AppUpdatePackage> Prepare(const std::filesystem::path& cache,
                                                    wire::ReleaseAsset asset);
    ~AppUpdatePackage();
    AppUpdatePackage() = default;
    AppUpdatePackage(const AppUpdatePackage&) = delete;
    AppUpdatePackage& operator=(const AppUpdatePackage&) = delete;
    const std::filesystem::path& Path() const { return path_; }
    void Write(std::span<const std::byte> bytes);
    void CloseDownload();
    void Commit();
    // Windows transfers a ready helper; Linux waits for authorized installation.
    // The caller exits only on success and restores its writable session on error.
    void InstallDesktopUpdate(const std::filesystem::path& executableDirectory);
private:
    std::filesystem::path directory_, path_;
    wire::ReleaseAsset asset_;
    std::ofstream output_;
    std::uint64_t written_ = 0;
    bool handedOff_ = false;
    bool androidVerified_ = false;
    friend class AndroidAppUpdateInstaller;
};

#if !defined(_WIN32) && !defined(__linux__)
inline AppUpdateTarget DetectAppUpdateTarget() { return AppUpdateTarget::Manual; }
inline std::string ReadAppUpdateNotice(const std::filesystem::path&) { return {}; }
inline std::shared_ptr<AppUpdatePackage> AppUpdatePackage::Prepare(const std::filesystem::path&, wire::ReleaseAsset) {
    throw std::runtime_error("In-app installation is unavailable on this platform");
}
inline AppUpdatePackage::~AppUpdatePackage() = default;
inline void AppUpdatePackage::Write(std::span<const std::byte>) { throw std::runtime_error("Unsupported update package"); }
inline void AppUpdatePackage::CloseDownload() { throw std::runtime_error("Unsupported update package"); }
inline void AppUpdatePackage::Commit() { throw std::runtime_error("Unsupported update package"); }
inline void AppUpdatePackage::InstallDesktopUpdate(const std::filesystem::path&) {
    throw std::runtime_error("Unsupported update installation");
}
#endif

} // namespace clashflux::ui
