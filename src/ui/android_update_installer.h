#pragma once

#if defined(__ANDROID__)
#include <huxerui/huxerui.h>
#include "app_update_package.h"

namespace clashflux::ui {

// APK inspection, install-source authorization and installation are Android
// capabilities. HTTP and download state remain in the shared application model.
class AndroidAppUpdateInstaller {
public:
    explicit AndroidAppUpdateInstaller(huxerui::PlatformChannel channel);
    huxerui::Task<void> VerifyAndCommit(std::shared_ptr<AppUpdatePackage> package) const;
    huxerui::Task<bool> EnsurePermission(std::shared_ptr<AppUpdatePackage> package) const;
    huxerui::Task<bool> Install(std::shared_ptr<AppUpdatePackage> package) const;
private:
    huxerui::Task<huxerui::PlatformPayload> Call(std::string method, huxerui::PlatformPayload arguments) const;
    huxerui::PlatformPayload PackageArguments(const AppUpdatePackage& package) const;
    huxerui::PlatformChannel channel_;
};

void InstallAndroidAppUpdateSupport(huxerui::ApplicationContext& context);

} // namespace clashflux::ui
#endif
