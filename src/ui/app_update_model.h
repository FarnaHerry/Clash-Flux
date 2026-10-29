#pragma once

#include <huxerui/huxerui.h>
#include "app_http_client.h"
#include "app_update_package.h"
#include "task_bridge.h"
#if defined(__ANDROID__)
#include "android_update_installer.h"
#endif

namespace clashflux::ui {

enum class AppUpdatePhase { Idle, Downloading, Verifying, Ready, DownloadFailed, Installing, InstallFailed };

// One model per Runtime. Both tasks and downloaded packages survive navigation,
// viewport changes and retirement/recreation of the tray window.
struct AppUpdateModel : std::enable_shared_from_this<AppUpdateModel> {
    huxerui::State<bool> checking{false}, checked{false};
    huxerui::State<AppUpdate> release{AppUpdate{}};
    huxerui::State<AppUpdatePhase> phase{AppUpdatePhase::Idle};
    huxerui::State<unsigned> percent{0};
    huxerui::State<std::string> error{std::string{}};
    huxerui::State<std::string> notice{std::string{}};
    huxerui::State<bool> noticeIsError{true};
    std::shared_ptr<AppUpdatePackage> package;
    huxerui::TaskHandle transfer;
    std::uint64_t serial = 0;
#if defined(__ANDROID__)
    std::shared_ptr<AndroidAppUpdateInstaller> androidInstaller;
#endif

    bool Transferring() const {
        return phase.Get() == AppUpdatePhase::Downloading || phase.Get() == AppUpdatePhase::Verifying;
    }
    bool CanInstall() const {
        return package && (phase.Get() == AppUpdatePhase::Ready || phase.Get() == AppUpdatePhase::InstallFailed);
    }

    void Check(huxerui::TaskScope tasks, std::shared_ptr<AppHttpClient> http, std::string current) {
        if (checking.Get() || Transferring() || CanInstall() || phase.Get() == AppUpdatePhase::Installing) return;
        checking = true;
        const auto ticket = ++serial;
        auto self = shared_from_this();
        tasks.Launch([self, http, current = std::move(current), ticket]() -> huxerui::Task<void> {
            AppUpdate next;
            try {
                huxerui::HttpRequest request;
                request.url = "https://api.github.com/repos/FarnaHerry/Clash-Flux/releases/latest";
                request.timeout = std::chrono::seconds{10};
                request.headers = {{"User-Agent", "Clash-Flux/" + current},
                    {"Accept", "application/vnd.github+json"}, {"X-GitHub-Api-Version", "2022-11-28"}};
                auto response = co_await http->SendAsync(std::move(request), [](huxerui::HttpProgress progress) {
                    if (progress.kind == huxerui::HttpProgressKind::Download && progress.transferred_bytes > 2 * 1024 * 1024)
                        throw std::runtime_error("Release response too large");
                });
                if (!response.Succeeded()) throw std::runtime_error(response.Error().message);
                if (response.Value().status_code != 200)
                    throw std::runtime_error("Release HTTP status " + std::to_string(response.Value().status_code));
                if (response.Value().body.size() > 2 * 1024 * 1024)
                    throw std::runtime_error("Release response too large");
                auto body = std::move(response.Value().body);
                next = co_await RunOnTaskThread([body = std::move(body), current] {
                    return ReadAppUpdate(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()),
                        current, DetectAppUpdateTarget());
                });
            } catch (const std::exception& exception) { next.error = exception.what(); }
            catch (...) { next.error = "Release check failed"; }
            if (ticket != self->serial) co_return;
            self->release = std::move(next);
            self->checked = true;
            self->checking = false;
            self->phase = AppUpdatePhase::Idle;
        });
    }

    void CancelDownload() {
        if (!Transferring()) return;
        ++serial;
        transfer.Cancel();
        package.reset();
        percent = 0;
        error = std::string{};
        phase = AppUpdatePhase::Idle;
    }

    void Download(huxerui::TaskScope tasks, std::shared_ptr<AppHttpClient> http,
                  std::filesystem::path cache) {
        const auto update = release.Get();
        if (checking.Get() || Transferring() || CanInstall() || phase.Get() == AppUpdatePhase::Installing ||
            !update.newer || !update.asset) return;
        const auto asset = *update.asset;
        phase = AppUpdatePhase::Downloading;
        percent = 0;
        error = std::string{};
        const auto ticket = ++serial;
        auto self = shared_from_this();
        transfer = tasks.Launch([self, http, asset, cache = std::move(cache), ticket]() -> huxerui::Task<void> {
            try {
                const auto candidate = co_await RunOnTaskThread([cache, asset] {
                    return AppUpdatePackage::Prepare(cache, asset);
                });
                huxerui::HttpRequest request;
                request.url = asset.browser_download_url;
                request.timeout = std::chrono::minutes{30};
                request.headers = {{"User-Agent", "Clash-Flux"}, {"Accept", "application/octet-stream"}};
                auto received = co_await http->SendStreamAsync(std::move(request));
                if (!received.Succeeded()) throw std::runtime_error(received.Error().message);
                auto response = std::move(received).Value();
                if (response.StatusCode() != 200 ||
                    (response.Url() != asset.browser_download_url &&
                     !response.Url().starts_with("https://release-assets.githubusercontent.com/")))
                    throw std::runtime_error("Unexpected update download response");
                std::uint64_t downloaded = 0;
                for (;;) {
                    auto chunk = co_await response.Body().ReadAsync(64 * 1024);
                    if (!chunk.Succeeded()) throw std::runtime_error("Update download interrupted");
                    auto bytes = std::move(chunk).Value();
                    if (bytes.empty()) break;
                    downloaded += bytes.size();
                    if (downloaded > asset.size) throw std::runtime_error("Update download exceeds declared size");
                    // The worker retains the package through any pending write,
                    // so cancellation cannot delete a file still being written.
                    co_await RunOnTaskThread([candidate, bytes = std::move(bytes)] { candidate->Write(bytes); });
                    if (ticket != self->serial) co_return;
                    self->percent = static_cast<unsigned>(downloaded * 100 / asset.size);
                }
                if (ticket != self->serial) co_return;
                self->phase = AppUpdatePhase::Verifying;
#if defined(__ANDROID__)
                co_await self->androidInstaller->VerifyAndCommit(candidate);
#else
                co_await RunOnTaskThread([candidate] { candidate->Commit(); });
#endif
                if (ticket != self->serial) co_return;
                self->package = candidate;
                self->phase = AppUpdatePhase::Ready;
            } catch (const std::exception& exception) {
                if (ticket != self->serial) co_return;
                self->error = exception.what();
                self->noticeIsError = true;
                self->notice = self->error.Get();
                self->phase = AppUpdatePhase::DownloadFailed;
            } catch (...) {
                if (ticket != self->serial) co_return;
                self->error = std::string{"Update download failed"};
                self->noticeIsError = true;
                self->notice = self->error.Get();
                self->phase = AppUpdatePhase::DownloadFailed;
            }
        });
    }
};

inline void InstallAppUpdateModel(huxerui::ApplicationContext& context) {
    auto model = std::make_shared<AppUpdateModel>();
#if defined(__ANDROID__)
    model->androidInstaller = huxerui::UseService<AndroidAppUpdateInstaller>();
#endif
    context.Provide(model);
    try {
        const auto cache = huxerui::UseApplication().Directories().cache_directory;
        huxerui::UseApplicationTaskScope().Launch([model, cache]() -> huxerui::Task<void> {
            try {
                auto notice = co_await RunOnTaskThread([cache] {
                    return ReadAppUpdateNotice(std::filesystem::path(cache.Path()));
                });
                if (!notice.empty() && model->notice.Get().empty()) {
                    model->noticeIsError = true;
                    model->notice = std::move(notice);
                }
            } catch (...) { /* An unavailable cache must not block application startup. */ }
        });
    } catch (...) { /* Headless/mobile hosts may not expose application cache directories. */ }
}

} // namespace clashflux::ui
