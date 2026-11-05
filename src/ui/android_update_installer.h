#pragma once

#if defined(__ANDROID__)
#include <huxerui/huxerui.h>
#include "app_update_package.h"

namespace clashflux::ui {

struct AndroidUpdateChannel;

// Application service: verification and download commit survive window closure.
class AndroidAppUpdateVerifier {
public:
    explicit AndroidAppUpdateVerifier(huxerui::PlatformChannel channel);
    ~AndroidAppUpdateVerifier();
    huxerui::Task<void> Verify(std::shared_ptr<AppUpdatePackage> package) const;
    huxerui::Task<void> VerifyAndCommit(std::shared_ptr<AppUpdatePackage> package) const;
private:
    std::shared_ptr<AndroidUpdateChannel> calls_;
};

// Session-owned module bound to the original window. Close also settles Tasks
// whose channel delivery is dropped when that window retires.
class AndroidAppUpdatePresentation {
public:
    explicit AndroidAppUpdatePresentation(huxerui::PlatformChannel channel);
    ~AndroidAppUpdatePresentation();
    void Close();
    huxerui::Task<bool> EnsurePermission(std::shared_ptr<AppUpdatePackage> package) const;
    huxerui::Task<bool> Install(std::shared_ptr<AppUpdatePackage> package) const;
private:
    std::shared_ptr<AndroidUpdateChannel> calls_;
};

void InstallAndroidAppUpdateSupport(huxerui::ApplicationContext& context);
std::shared_ptr<AndroidAppUpdatePresentation> OpenAndroidAppUpdatePresentation();

} // namespace clashflux::ui
#endif
