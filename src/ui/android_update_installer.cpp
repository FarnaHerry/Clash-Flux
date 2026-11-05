#include "android_update_installer.h"

#if defined(__ANDROID__)
#include <huxerui/android/platform_registry.h>
#include "task_bridge.h"
#include "app_update_pending_calls.h"

namespace clashflux::ui {

struct AndroidUpdateChannel : std::enable_shared_from_this<AndroidUpdateChannel> {
    huxerui::PlatformChannel channel;
    bool closed = false;
    AppUpdatePendingCalls pending;

    explicit AndroidUpdateChannel(huxerui::PlatformChannel value) : channel(std::move(value)) {}

    huxerui::Task<huxerui::PlatformPayload> Call(std::string method, huxerui::PlatformPayload arguments) {
        const auto self = shared_from_this();
        co_return co_await huxerui::AwaitCallback<huxerui::PlatformPayload>(
            [self, method = std::move(method), arguments = std::move(arguments)](auto completion) {
                if (self->closed || !self->channel.IsOpen()) {
                    completion.Fail(std::make_exception_ptr(std::runtime_error("Android update module is closed")));
                    return std::function<void()>{};
                }
                const auto token = self->pending.Add(completion);
                huxerui::PlatformRequestId request;
                try {
                    request = self->channel.Invoke(method, arguments, [self, token](auto result) {
                        self->pending.Finish(token, std::move(result));
                    });
                } catch (...) {
                    self->pending.Abandon(token);
                    throw;
                }
                return std::function<void()>{[self, token, request] {
                    self->pending.Abandon(token);
                    self->channel.Cancel(request);
                }};
            });
    }

    void Close() {
        if (closed) return;
        closed = true;
        pending.Close();
        channel.Close();
    }
};

AndroidAppUpdateVerifier::AndroidAppUpdateVerifier(huxerui::PlatformChannel channel)
    : calls_(std::make_shared<AndroidUpdateChannel>(std::move(channel))) {}
AndroidAppUpdateVerifier::~AndroidAppUpdateVerifier() { calls_->Close(); }

huxerui::Task<void> AndroidAppUpdateVerifier::Verify(std::shared_ptr<AppUpdatePackage> package) const {
    const std::string_view name = package->asset_.name;
    constexpr std::string_view prefix = "clash-flux-", suffix = "-android-arm64-v8a.apk";
    if (!name.starts_with(prefix) || !name.ends_with(suffix) || name.size() <= prefix.size() + suffix.size())
        throw std::runtime_error("Unexpected Android update asset");
    const std::string version(name.substr(prefix.size(), name.size() - prefix.size() - suffix.size()));
    if (!StableVersion(version)) throw std::runtime_error("Invalid Android update version");
    const auto path = package->Path().u8string();
    const auto result = co_await calls_->Call("verify", huxerui::PlatformPayload::Object{
        {"path", std::string(reinterpret_cast<const char*>(path.data()), path.size())},
        {"size", package->asset_.size}, {"sha256", package->asset_.digest.substr(7)}, {"version", version}});
    if (!result.IsNull()) throw std::runtime_error("Invalid APK verification result");
}

huxerui::Task<void> AndroidAppUpdateVerifier::VerifyAndCommit(std::shared_ptr<AppUpdatePackage> package) const {
    co_await RunOnTaskThread([package] { package->CloseDownload(); });
    co_await Verify(package);
    package->androidVerified_ = true;
    co_await RunOnTaskThread([package] { package->Commit(); });
}

AndroidAppUpdatePresentation::AndroidAppUpdatePresentation(huxerui::PlatformChannel channel)
    : calls_(std::make_shared<AndroidUpdateChannel>(std::move(channel))) {}
AndroidAppUpdatePresentation::~AndroidAppUpdatePresentation() { Close(); }
void AndroidAppUpdatePresentation::Close() { calls_->Close(); }

huxerui::Task<bool> AndroidAppUpdatePresentation::EnsurePermission(std::shared_ptr<AppUpdatePackage> package) const {
    // The system may reclaim the UI process while permission/installation is open.
    package->handedOff_ = true;
    co_return (co_await calls_->Call("permission", {})).AsBoolean();
}

huxerui::Task<bool> AndroidAppUpdatePresentation::Install(std::shared_ptr<AppUpdatePackage> package) const {
    const auto path = package->Path().u8string();
    co_return (co_await calls_->Call("install",
        std::string(reinterpret_cast<const char*>(path.data()), path.size()))).AsBoolean();
}

void InstallAndroidAppUpdateSupport(huxerui::ApplicationContext& context) {
    constexpr auto name = "clashflux/AppUpdate";
    huxerui::android::JavaPlatformModuleFactory<std::shared_ptr<AndroidAppUpdateVerifier>> verifier;
    verifier.class_name = "dev.farna.clashflux.AppUpdateModule";
    verifier.create = [](huxerui::PlatformChannel channel) {
        return std::make_shared<AndroidAppUpdateVerifier>(std::move(channel));
    };
    context.RegisterPlatformModule<std::shared_ptr<AndroidAppUpdateVerifier>>(name, std::move(verifier));
    context.Provide(huxerui::OpenPlatformModule<std::shared_ptr<AndroidAppUpdateVerifier>>(name));

    huxerui::android::JavaWindowPlatformModuleFactory<std::shared_ptr<AndroidAppUpdatePresentation>> presentation;
    presentation.class_name = "dev.farna.clashflux.AppUpdatePresentationModule";
    presentation.create = [](huxerui::PlatformChannel channel) {
        return std::make_shared<AndroidAppUpdatePresentation>(std::move(channel));
    };
    context.RegisterPlatformModule<std::shared_ptr<AndroidAppUpdatePresentation>>(
        "clashflux/AppUpdatePresentation", std::move(presentation));
}

std::shared_ptr<AndroidAppUpdatePresentation> OpenAndroidAppUpdatePresentation() {
    return huxerui::OpenPlatformModule<std::shared_ptr<AndroidAppUpdatePresentation>>("clashflux/AppUpdatePresentation");
}

} // namespace clashflux::ui
#endif
