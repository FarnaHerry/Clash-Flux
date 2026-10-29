#include "android_update_installer.h"

#if defined(__ANDROID__)
#include <huxerui/android/platform_registry.h>
#include "task_bridge.h"
#include <optional>
#include <variant>

namespace clashflux::ui {

AndroidAppUpdateInstaller::AndroidAppUpdateInstaller(huxerui::PlatformChannel channel) : channel_(std::move(channel)) {}

huxerui::Task<huxerui::PlatformPayload> AndroidAppUpdateInstaller::Call(
    std::string method, huxerui::PlatformPayload arguments) const {
    using Result = huxerui::PlatformResult<huxerui::PlatformPayload>;
    auto completed = std::make_shared<std::optional<Result>>();
    const auto request = channel_.Invoke(std::move(method), std::move(arguments), [completed](Result result) {
        *completed = std::move(result);
    });
    struct CancelPending {
        huxerui::PlatformChannel channel;
        huxerui::PlatformRequestId request;
        ~CancelPending() { channel.Cancel(request); }
    } cancel{channel_, request};
    // PlatformChannel in the pinned SDK exposes callbacks. This local wait owns
    // one request, not a page data pump; cancellation closes its Java operation.
    while (!completed->has_value()) co_await huxerui::Delay(std::chrono::milliseconds(50));
    if (const auto* error = std::get_if<huxerui::PlatformError>(&**completed))
        throw std::runtime_error(error->message);
    co_return std::get<huxerui::PlatformPayload>(std::move(**completed));
}

huxerui::PlatformPayload AndroidAppUpdateInstaller::PackageArguments(const AppUpdatePackage& package) const {
    const std::string_view name = package.asset_.name;
    constexpr std::string_view prefix = "clash-flux-", suffix = "-android-arm64-v8a.apk";
    if (!name.starts_with(prefix) || !name.ends_with(suffix) || name.size() <= prefix.size() + suffix.size())
        throw std::runtime_error("Unexpected Android update asset");
    const std::string version(name.substr(prefix.size(), name.size() - prefix.size() - suffix.size()));
    if (!StableVersion(version)) throw std::runtime_error("Invalid Android update version");
    const auto path = package.Path().u8string();
    return huxerui::PlatformPayload::Object{
        {"path", std::string(reinterpret_cast<const char*>(path.data()), path.size())},
        {"size", package.asset_.size}, {"sha256", package.asset_.digest.substr(7)}, {"version", version}};
}

huxerui::Task<void> AndroidAppUpdateInstaller::VerifyAndCommit(std::shared_ptr<AppUpdatePackage> package) const {
    co_await RunOnTaskThread([package] { package->CloseDownload(); });
    const auto result = co_await Call("verify", PackageArguments(*package));
    if (!result.IsNull()) throw std::runtime_error("Invalid APK verification result");
    package->androidVerified_ = true;
    co_await RunOnTaskThread([package] { package->Commit(); });
}

huxerui::Task<bool> AndroidAppUpdateInstaller::EnsurePermission(std::shared_ptr<AppUpdatePackage> package) const {
    // A system permission/installer screen can reclaim the UI process. Keep the
    // APK in private cache rather than deleting a file the system may still read.
    package->handedOff_ = true;
    co_return (co_await Call("permission", {})).AsBoolean();
}

huxerui::Task<bool> AndroidAppUpdateInstaller::Install(std::shared_ptr<AppUpdatePackage> package) const {
    co_return (co_await Call("install", PackageArguments(*package))).AsBoolean();
}

void InstallAndroidAppUpdateSupport(huxerui::ApplicationContext& context) {
    constexpr auto name = "clashflux/AppUpdate";
    huxerui::android::JavaPlatformModuleFactory<std::shared_ptr<AndroidAppUpdateInstaller>> factory;
    factory.class_name = "dev.farna.clashflux.AppUpdateModule";
    factory.create = [](huxerui::PlatformChannel channel) {
        return std::make_shared<AndroidAppUpdateInstaller>(std::move(channel));
    };
    context.RegisterPlatformModule<std::shared_ptr<AndroidAppUpdateInstaller>>(name, std::move(factory));
    context.Provide(huxerui::OpenPlatformModule<std::shared_ptr<AndroidAppUpdateInstaller>>(name));
}

} // namespace clashflux::ui
#endif
