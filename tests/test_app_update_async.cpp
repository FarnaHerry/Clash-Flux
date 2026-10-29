#include <huxerui/huxerui.h>
#include <huxerui/testing/ui_test.h>
#include "../src/ui/app_update_model.h"
#include <chrono>
#include <iostream>
#include <random>
#include <thread>

using namespace huxerui;
using namespace clashflux::ui;
static std::shared_ptr<AppUpdateModel> model;
static std::shared_ptr<AppHttpClient> http;
static std::optional<TaskScope> tasks;

[[huxerui::composable]] View UpdatePanel() {
    const auto shared = UseService<AppUpdateModel>();
    return Text(std::to_string(static_cast<unsigned>(shared->phase.Get()))).Key("update-panel");
}
static View Root() {
    const auto page = UseState(true);
    model = UseService<AppUpdateModel>();
    http = UseService<AppHttpClient>();
    tasks = UseApplicationTaskScope();
    return Column {
      Button("Toggle page").OnClick([page] { page = !page.Get(); }),
      page.Get() ? UpdatePanel() : Text("Another page"),
    };
}
static void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> static void Wait(testing::UiTest& ui, F done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        ui.Pump(std::chrono::milliseconds(10));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Require(done(), "asynchronous update did not settle");
}
int main() {
    const auto cache = std::filesystem::temp_directory_path() /
        ("clash-flux-update-async-" + std::to_string(std::random_device{}()));
    try {
        const Application application{Root, AppOptions{.application_hooks = {InstallAppHttpClient, InstallAppUpdateModel}}};
        testing::UiTest ui(application, {.viewport = {480, 320}});
        ui.PumpAndSettle();
        const auto shared = model;
        const clashflux::wire::ReleaseAsset asset{
            "clash-flux-1.2.3-windows-x86_64.exe", "https://github.com/FarnaHerry/Clash-Flux/releases/download/v1.2.3/clash-flux-1.2.3-windows-x86_64.exe",
            "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "uploaded", 3};
        model->release = AppUpdate{"v1.2.3", true, {}, asset};
        model->Download(*tasks, http, cache);
        const auto started = model->serial;
        model->Download(*tasks, http, cache);
        model->Check(*tasks, http, "1.2.2");
        Require(model->serial == started && model->Transferring(), "download/check must share single-flight ownership");
        model->CancelDownload();
        Require(!model->Transferring() && model->phase.Get() == AppUpdatePhase::Idle, "cancel clears busy immediately");
        model->Download(*tasks, http, cache);
        ui.Find(testing::UiSelector::Text("Toggle page")).Tap();
        ui.PumpAndSettle();
        Require(!ui.Find(testing::UiSelector::Key("update-panel")).Exists(), "initiating panel unmounts");
        // The headless platform deliberately has no production HTTP transport.
        // Its real failure must still finish the application task after unmount.
        Wait(ui, [] { return model->phase.Get() == AppUpdatePhase::DownloadFailed; });
        Require(!model->error.Get().empty() && !model->package, "failure retains no installable candidate");
        Require(model->noticeIsError.Get() && model->notice.Get() == model->error.Get(),
            "download or APK validation failure remains visible after the initiating panel unmounts");
        ui.Find(testing::UiSelector::Text("Toggle page")).Tap();
        ui.PumpAndSettle();
        Require(shared == model && model->phase.Get() == AppUpdatePhase::DownloadFailed,
            "remount keeps the authoritative update result");
        model->Check(*tasks, http, "1.2.2");
        Wait(ui, [] { return !model->checking.Get(); });
        Require(model->checked.Get() && !model->release.Get().error.empty(), "failed check can finish and retry");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    model.reset(); http.reset(); tasks.reset();
    // Cancelled workers can own their candidate until the write/prepare returns.
    for (unsigned attempt = 0; attempt < 500 && std::filesystem::exists(cache / "app-updates") &&
         !std::filesystem::is_empty(cache / "app-updates"); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (std::filesystem::exists(cache / "app-updates")) {
        Require(std::filesystem::is_empty(cache / "app-updates"), "cancelled downloads must clean their exclusive stages");
        std::filesystem::remove(cache / "app-updates");
    }
    std::filesystem::remove(cache);
    std::cout << "app_update_async: cancellation, single-flight, failure and page retirement passed\n";
    return 0;
}
