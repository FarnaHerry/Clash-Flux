// platform_app.cpp — 平台专属应用壳层与订阅刷新泵。
//
// AppRoot 只负责组装通用页面；窗口/托盘生命周期、Android 数据目录和
// 平台网络刷新通道在这里按平台函数整体实现，再由调用点宏选择。
#include <huxerui/huxerui.h>

#include <chrono>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#if defined(__ANDROID__)
#include <android/log.h>
#endif

#include "ui.h"
#include "app.h"
#include "page_layout.h"
#include "app_resources.h"
#include "proxies_model.h"
#include "settings_model.h"
#include "app_runtime_services.h"
#include "task_bridge.h"

import clashflux.config;
import clashflux.cli;
import clashflux.core;
import clashflux.db;
import clashflux.persistence;
import clashflux.service;
import clashflux.store.core;
import clashflux.store.profiles;
import clashflux.store.vpn;
import clashflux.stream;
#if !defined(__ANDROID__)
import clashflux.instance;
#endif

// CoreView 含 store::CoreSnapshot、ProfilesModel 含 db::Profile，必须在模块导入
// 之后（同 profiles_cache.h）。
#include "core_model.h"
#include "core_control.h"
#include "profiles_model.h"

namespace clashflux::ui {

#if defined(__ANDROID__)

void AndroidPreparePlatformDataDirectory(
    const huxerui::ApplicationHandle& application) {
    // HuxerUI owns the Android Context and prepares its directories before the
    // runtime is created. Clash-Flux uses that official data root.
    const std::string dataDirectory = application.Directories().data_directory.Path();
    cfg::setAndroidDataDir(dataDirectory);
    __android_log_print(ANDROID_LOG_INFO, "ClashFlux",
                        "HuxerUI data directory: %s", dataDirectory.c_str());
}

[[huxerui::composable]] huxerui::View AndroidProfileRefreshPump() {
    auto tasks = huxerui::UseTaskScope();
    auto http = huxerui::UseService<AppHttpClient>();
    huxerui::Lifecycle(
        [tasks, http] {
            tasks.Launch([http]() -> huxerui::Task<void> {
                for (;;) {
                    co_await huxerui::Delay(std::chrono::duration<double>{30.0});
                    co_await AndroidRefreshProfilesDueOnce(http);
                }
            });
            return [] {};
        },
        0);
    return {};
}

[[huxerui::composable]] huxerui::View AndroidApplicationEffects(
    const huxerui::ApplicationHandle&, const huxerui::ThemeSpec&, bool) {
    return {};
}

[[huxerui::composable]] huxerui::View AndroidAppContent(
    huxerui::View mainRow, const huxerui::ThemeSpec& rootSpec) {
    const huxerui::Color pageBackground = ResolveIslandTheme(rootSpec).base;
    huxerui::SystemBarsAppearance systemBars =
        huxerui::UseEnvironment<huxerui::SystemBarsAppearance>();
    systemBars.status_bar_background = pageBackground;
    systemBars.navigation_bar_background = pageBackground;
    // EdgeToEdge 下背景覆盖系统栏，内容在壳层统一消费安全区。
    // 一级、二级页与底部导航不重复增加 inset，状态栏图标明暗跟随实际底色。
    return huxerui::View{mainRow}.With(
        huxerui::SafeAreaPadding{},
        huxerui::Background(pageBackground),
        systemBars,
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

#else

struct DesktopTrayRuntime : std::enable_shared_from_this<DesktopTrayRuntime> {
    explicit DesktopTrayRuntime(huxerui::ApplicationHandle app)
        : application(std::move(app)), tray(application->SystemTray()) {}

    std::optional<huxerui::ApplicationHandle> application;
    std::optional<huxerui::WindowHandle> window;
    std::optional<CoreControl> control;
    std::optional<huxerui::TaskScope> tasks;
    std::shared_ptr<SettingsModel> settingsModel;
    std::shared_ptr<AppRuntimeServices> runtimeServices;
    huxerui::SystemTrayHandle tray;
    std::function<void()> refresh;
    bool started = false;
    bool retired = true;
    bool startupDecisionPending = false;
    bool trayRefreshFailed = false;
    bool basicTrayVisible = false;
    bool exitRequested = false;
    std::uint64_t windowGeneration = 0;
    std::string presentationKey;
    std::string menuStructureKey;

    void HideTray() {
        tray.Hide();
        // Hide discards the native presentation, so unchanged model values
        // must still republish it after tray enablement or host recovery.
        presentationKey.clear();
        menuStructureKey.clear();
        basicTrayVisible = false;
    }

    CoreControl MenuControl() {
        CoreControl next = *control;
        const std::weak_ptr<DesktopTrayRuntime> weak = shared_from_this();
        const auto generation = windowGeneration;
        next.presentationAvailable = [weak, generation] {
            const auto runtime = weak.lock();
            return runtime && !runtime->retired &&
                   runtime->windowGeneration == generation;
        };
        next.onHeadlessFailure = [weak](bool permissionDenied,
                                        const std::string& error) {
            if (const auto runtime = weak.lock()) {
                stream::logApplication("error", error.empty()
                    ? "托盘内核操作失败" : error);
                if (permissionDenied) runtime->ShowWindow();
            }
        };
        return next;
    }

    void ShowWindow() {
        if (!application) return;
        const bool opening = retired || !window;
        try {
            if (opening) {
                ++windowGeneration;
                retired = false;
                window = application->OpenWindow([] { return AppRoot(); }, huxerui::WindowOptions{
                    .title = "Clash-Flux",
                    .initial_size = {1080.0F, 720.0F},
                    .minimum_size = huxerui::Size{320.0F, 480.0F},
                    .chrome_mode = huxerui::WindowChromeMode::Custom,
                    .title_bar_height = kDesktopTitleBarHeight,
                });
                presentationKey.clear();
            }
            if (window) {
                window->Show();
                window->Activate();
            }
        } catch (const std::exception& error) {
            if (opening) retired = true;
            stream::logApplication("error", std::string{"唤出主窗口失败："} + error.what());
        }
    }

    void OpenHiddenWindow() {
        if (!application || window) return;
        window = application->OpenWindow([] { return AppRoot(); }, huxerui::WindowOptions{
            .title = "Clash-Flux",
            .initial_size = {1080.0F, 720.0F},
            .minimum_size = huxerui::Size{320.0F, 480.0F},
            .chrome_mode = huxerui::WindowChromeMode::Custom,
            .title_bar_height = kDesktopTitleBarHeight,
        });
        retired = false;
        window->Hide();
    }

    void RequestExit() {
        if (exitRequested || !application) return;
        exitRequested = true;
        refresh = {};
        instance::markClosing();
        HideTray();
        if (window) window->Hide();
        const huxerui::ApplicationHandle app = *application;
        auto coreStopped = std::make_shared<std::atomic_bool>(false);
        auto cleanupFinished = std::make_shared<std::atomic_bool>(false);
        if (!tasks) return;
        tasks->Launch(
            [app, coreStopped, cleanupFinished]() -> huxerui::Task<void> {
            try {
                bool stopped = co_await RunOnTaskThread([] {
                    store::vpnStore().shutdown();
                    return store::coreStore().stopCore();
                });
                if (!stopped) {
                    stopped = co_await RunOnTaskThread([] {
                        return store::coreStore().stopCore();
                    });
                    if (!stopped) stream::logApplication(
                        "error", "退出时停止内核失败：" +
                            store::coreStore().snapshot().lastError);
                }
                coreStopped->store(stopped);
                auto& database = clashflux::persistence::persistence();
                const bool settingsSaved = co_await database.flushSettings();
                const bool profilesSaved = co_await database.flushProfiles();
                if (!settingsSaved || !profilesSaved) {
                    stream::logApplication("error", "退出前落库失败：" +
                        (database.lastError().empty()
                            ? std::string{"未知原因"} : database.lastError()));
                }
                cleanupFinished->store(true);
                app.Quit();
            } catch (const std::exception& error) {
                stream::logApplication("error", std::string{"托盘退出清理异常："} + error.what());
                cleanupFinished->store(true);
                app.Quit();
            } catch (...) {
                stream::logApplication("error", "托盘退出清理未知异常");
                cleanupFinished->store(true);
                app.Quit();
            }
        });
        std::thread([coreStopped, cleanupFinished] {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            if (!coreStopped->load()) store::coreStore().stopCore();
            while (!cleanupFinished->load())
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::this_thread::sleep_for(std::chrono::seconds(3));
            std::_Exit(0);
        }).detach();
    }

    void RefreshBasicTray() {
        if (exitRequested) return;
        if (refresh) {
            // The windowless startup menu and the full window menu have
            // different trees. Give StatusNotifier hosts one update cycle with
            // an empty menu before publishing the replacement; otherwise KDE
            // can retain an action from the old tree (notably a second Quit).
            if (basicTrayVisible) {
                HideTray();
                return;
            }
            refresh();
            return;
        }
        if (!tray.IsAvailable() || !control) return;
        const bool running = control->Running();
        const bool proxyEnabled = control->SystemProxyEnabled();
        const bool tunEnabled = control->TunEnabled();
        const bool busy = control->Busy();
        const SettingsView settings = settingsModel
            ? settingsModel->view.Get() : SettingsView{};
        const bool english = settings.ready ? settings.language == "en"
            : runtimeServices && runtimeServices->startupLanguage == "en";
        const CoreView view = control->model->view.Get();
        const bool coreRunning = view.core.state == core::CoreState::Running;
        const std::string key = std::format("{}:{}:{}:{}:{}:{}:{}:{}", running, proxyEnabled,
            tunEnabled, busy, english, coreRunning, view.systemProxyActive, view.TunActive());
        if (key == presentationKey) return;
        std::vector<huxerui::MenuEntry> entries;
        const auto self = shared_from_this();
        entries.push_back(huxerui::MenuItem(english ? "Open main window" : "显示主窗口",
                                            [self] { self->ShowWindow(); }));
        entries.push_back(huxerui::MenuSection{});
        entries.push_back(huxerui::MenuItem(
            running ? (english ? "Stop core" : "停止内核")
                    : (english ? "Start core" : "启动内核"), [self] {
                if (self->control) self->MenuControl().Run({
                    .action = self->control->Running() ? CoreAction::Stop : CoreAction::Start});
            }).Checked(running).Enabled(!busy));
        entries.push_back(huxerui::MenuItem(english ? "System proxy" : "系统代理", [self] {
            if (self->control) self->MenuControl().Run({
                .action = CoreAction::SystemProxy,
                .enabled = !self->control->SystemProxyEnabled()});
        }).Checked(proxyEnabled).Enabled(!busy));
        entries.push_back(huxerui::MenuItem(english ? "TUN mode" : "TUN 模式", [self] {
            if (self->control) self->MenuControl().Run({
                .action = CoreAction::Tun,
                .enabled = !self->control->TunEnabled()});
        }).Checked(tunEnabled).Enabled(!busy));
        entries.push_back(huxerui::MenuSection{});
        entries.push_back(huxerui::MenuItem(english ? "Quit" : "退出",
                                            [self] { self->RequestExit(); }));
        huxerui::ImageVariant icon = app::images::tray_default;
        if (coreRunning && view.TunActive()) icon = app::images::tray_tun;
        else if (coreRunning && view.systemProxyActive) icon = app::images::tray_system_proxy;
        tray.Show(std::move(icon), huxerui::SystemTrayOptions{
            .tooltip = "Clash-Flux", .menu = std::move(entries)});
        basicTrayVisible = true;
        presentationKey = std::string{key};
    }

    void HideWindow(bool lightweight) {
        if (!window) return;
        if (lightweight) {
            retired = true;
            window->Retire();
        } else {
            window->Hide();
        }
    }
};

void InstallDesktopTrayRuntime(huxerui::ApplicationContext& context) {
#if defined(CLASHFLUX_IOS)
    context.Provide(std::make_shared<DesktopTrayRuntime>(huxerui::UseApplication()));
    return;
#else
    auto runtime = std::make_shared<DesktopTrayRuntime>(huxerui::UseApplication());
    context.Provide(runtime);
    const huxerui::TaskScope tasks = huxerui::UseApplicationTaskScope();
    const auto runtimeServices = huxerui::UseService<AppRuntimeServices>();
    auto coreModel = huxerui::UseService<CoreModel>();
    auto settingsModel = huxerui::UseService<SettingsModel>();
    runtime->tasks = tasks;
    runtime->runtimeServices = runtimeServices;
    runtime->settingsModel = settingsModel;
    const std::weak_ptr<DesktopTrayRuntime> weak = runtime;
    CoreControl control;
    control.model = coreModel;
    control.tasks = tasks;
    control.presentationAvailable = [] { return false; };
    control.onHeadlessFailure = [runtimeServices, weak](bool, const std::string& error) {
        const std::string message = error.empty() ? "托盘内核操作失败" : error;
        runtimeServices->startupError = message;
        stream::logApplication("error", message);
        if (const auto trayRuntime = weak.lock()) trayRuntime->ShowWindow();
    };
    runtime->control = std::move(control);
    runtimeServices->showWindow = [weak] {
        if (const auto trayRuntime = weak.lock()) trayRuntime->ShowWindow();
    };

    const auto startupSettings = persistence::readStartupSettings(cfg::databaseFile());
    if (!startupSettings.error.empty()) {
        std::fprintf(stderr, "Clash-Flux 启动失败：%s；原数据库与订阅文件已保留\n",
                     startupSettings.error.c_str());
        cli::setPendingExitCode(1);
        runtime->application->Quit();
        return;
    }
    runtimeServices->startupSettingsLoaded = true;
    runtimeServices->startupThemeMode = startupSettings.themeMode;
    runtimeServices->startupThemeColor = startupSettings.themeColor;
    runtimeServices->startupLanguage = startupSettings.language;
    runtimeServices->startupTrayEnabled = startupSettings.trayEnabled;
    runtimeServices->startupStartMinimized = startupSettings.startMinimized;

    const auto& startupActivation = runtime->application->StartupActivation();
    const bool startupUrl = startupActivation &&
        std::holds_alternative<huxerui::UrlActivation>(*startupActivation);
    runtime->tray.OnActivate([runtime] { runtime->ShowWindow(); });

    const bool commandMode = cli::runtimeCommandMode();
    const bool startupWantsHidden = !commandMode && !startupUrl &&
        startupSettings.trayEnabled && startupSettings.startMinimized;
    runtime->startupDecisionPending = startupWantsHidden;
    runtime->started = true;
    // Application hooks run before Runtime enters Running. Both OpenWindow()
    // paths must wait until the platform event loop can create native windows.
    // Linux discovers the status notifier host asynchronously after OnActivate.
    tasks.Launch([runtime, commandMode, startupWantsHidden]() -> huxerui::Task<void> {
        if (commandMode) {
            runtime->OpenHiddenWindow();
        } else if (startupWantsHidden) {
            for (int attempt = 0; attempt < 20 && !runtime->tray.IsAvailable(); ++attempt)
                co_await huxerui::Delay(std::chrono::duration<double>{0.1});
            if (runtime->tray.IsAvailable()) {
                try {
                    runtime->RefreshBasicTray();
                } catch (const std::exception& error) {
                    runtime->trayRefreshFailed = true;
                    stream::logApplication("error", std::string{"初始化托盘失败："} + error.what());
                    runtime->ShowWindow();
                }
            } else {
                runtime->ShowWindow();
            }
        } else {
            runtime->ShowWindow();
        }
        runtime->startupDecisionPending = false;
    });
    if (startupWantsHidden) StartDesktopWindowlessRuntime();

    const auto tray = runtime->tray;
    const bool initialTrayEnabled = startupSettings.trayEnabled;
    tasks.Launch([runtime, tray, settingsModel,
                            initialTrayEnabled]() -> huxerui::Task<void> {
        for (;;) {
            if (runtime->exitRequested) co_return;
            if (instance::consumeActivation()) runtime->ShowWindow();
            const SettingsView settings = settingsModel->view.Get();
            const bool enabled = settings.ready ? settings.trayEnabled : initialTrayEnabled;
            if (!runtime->startupDecisionPending && runtime->retired &&
                (!tray.IsAvailable() || !enabled))
                runtime->ShowWindow();
            if (enabled && tray.IsAvailable()) {
                try {
                    runtime->RefreshBasicTray();
                    runtime->trayRefreshFailed = false;
                } catch (const std::exception& error) {
                    if (!runtime->trayRefreshFailed) {
                        stream::logApplication("error", std::string{"刷新托盘失败："} + error.what());
                        runtime->ShowWindow();
                    }
                    runtime->trayRefreshFailed = true;
                }
            } else {
                runtime->HideTray();
                runtime->trayRefreshFailed = false;
            }
            co_await huxerui::Delay(std::chrono::duration<double>{0.25});
        }
    });
#endif
}

void DesktopPreparePlatformDataDirectory(
    const huxerui::ApplicationHandle& application) {
    static_cast<void>(application);
}

bool HasDesktopCoreRuntime() {
#if defined(CLASHFLUX_IOS)
    // sing-box is linked into the Packet Tunnel extension, not shipped as an
    // executable beside the iOS app.
    return true;
#else
    return !cfg::singboxBinary().empty();
#endif
}

[[huxerui::composable]] huxerui::View DesktopProfileRefreshPump() {
    auto tasks = huxerui::UseApplicationTaskScope();
    auto runtimeServices = huxerui::UseService<AppRuntimeServices>();
    huxerui::Lifecycle(
        [tasks, runtimeServices] {
            if (!runtimeServices->profileRefreshStarted) {
                runtimeServices->profileRefreshStarted = true;
                tasks.Launch([]() -> huxerui::Task<void> {
                    for (;;) {
                        co_await huxerui::Delay(std::chrono::duration<double>{30.0});
                        co_await RunOnTaskThread([] {
                            store::profilesStore().refreshDue();
                        });
                    }
                });
            }
            return [] {};
        },
        0);
    return {};
}

namespace {

#if defined(_WIN32)
// HuxerUI's Windows tray menu is a native HMENU and SystemTrayOptions does not
// expose MenuStyle. Set the process menu preference from the active app theme;
// resolve the versioned uxtheme entry points dynamically and leave older systems
// untouched when the dark-menu API is unavailable.
void ApplyWindowsNativeMenuTheme(bool dark) noexcept {
    using RtlGetVersionFunction = LONG(WINAPI*)(OSVERSIONINFOW*);
    using SetPreferredAppModeFunction = int(WINAPI*)(int);
    using FlushMenuThemesFunction = void(WINAPI*)();

    const HMODULE nativeLibrary = GetModuleHandleW(L"ntdll.dll");
    const auto getVersion = reinterpret_cast<RtlGetVersionFunction>(
        nativeLibrary ? GetProcAddress(nativeLibrary, "RtlGetVersion") : nullptr);
    if (!getVersion) return;

    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (getVersion(&version) < 0 || version.dwMajorVersion < 10 ||
        version.dwBuildNumber < 17763) {
        return;
    }

    static const HMODULE themeLibrary = LoadLibraryW(L"uxtheme.dll");
    if (!themeLibrary) return;

    auto setPreferredAppMode = reinterpret_cast<SetPreferredAppModeFunction>(
        GetProcAddress(themeLibrary, "SetPreferredAppMode"));
    const bool modernApi = setPreferredAppMode != nullptr ||
                           version.dwBuildNumber >= 18362;
    if (!setPreferredAppMode) {
        setPreferredAppMode = reinterpret_cast<SetPreferredAppModeFunction>(
            GetProcAddress(themeLibrary, MAKEINTRESOURCEA(135)));
    }
    if (!setPreferredAppMode) return;

    // Windows 10 1809 used ordinal 135 as AllowDarkModeForApp(BOOL); later
    // releases use SetPreferredAppMode(PreferredAppMode).
    const int mode = modernApi ? (dark ? 2 : 3) : (dark ? 1 : 0);
    static_cast<void>(setPreferredAppMode(mode));

    auto flushMenuThemes = reinterpret_cast<FlushMenuThemesFunction>(
        GetProcAddress(themeLibrary, "FlushMenuThemes"));
    if (!flushMenuThemes) {
        flushMenuThemes = reinterpret_cast<FlushMenuThemesFunction>(
            GetProcAddress(themeLibrary, MAKEINTRESOURCEA(136)));
    }
    if (flushMenuThemes) flushMenuThemes();
}
#else
void ApplyWindowsNativeMenuTheme(bool) noexcept {}
#endif

// 原生托盘按应用级 ResourceConfiguration 解析资源，不继承窗口的 Locale。
// 在组合期解析为拥有型文字，再把同一语言的菜单交给平台托盘。
struct TrayMenuText {
    std::string showWindow;
    std::string noProfiles;
    std::string selectProfile;
    std::string selectLine;
    std::string noLines;
    std::string autoLineSuffix;
    std::string startCore;
    std::string stopCore;
    std::string systemProxy;
    std::string tun;
    std::string lightweight;
    std::string quit;
};

} // namespace

[[huxerui::composable]] huxerui::View DesktopApplicationEffects(
    const huxerui::ApplicationHandle& application,
    const huxerui::ThemeSpec& rootSpec,
    bool startupTrayEnabled) {
    const huxerui::WindowHandle window = huxerui::UseWindow();
    // 策略组快照 / 内核与接管状态的全局唯一来源（见 *_model.h）：托盘菜单只读。
    const auto proxiesModel = huxerui::UseService<ProxiesModel>();
    const auto coreModel = huxerui::UseService<CoreModel>();
    const auto profilesModel = huxerui::UseService<ProfilesModel>();
    const auto settingsModel = huxerui::UseService<SettingsModel>();
    const auto trayRuntime = huxerui::UseService<DesktopTrayRuntime>();
    const auto runtimeServices = huxerui::UseService<AppRuntimeServices>();
    const auto startupTasks = huxerui::UseApplicationTaskScope();
    const huxerui::SystemTrayHandle tray = application.SystemTray();
    const bool trayAvailable = tray.IsAvailable();
    const auto control = UseCoreControl();
    trayRuntime->application = application;
    trayRuntime->window = window;
    trayRuntime->control = control;
    auto closeDialogOpen = huxerui::UseState(false);
    auto dialog = huxerui::UseDialog();
    auto toast = huxerui::UseToast();
    const bool darkTheme = IsDarkTheme(rootSpec);

    huxerui::Lifecycle(
        [runtimeServices, toast] {
            const std::string error = runtimeServices->startupError.Get();
            if (!error.empty() && !runtimeServices->startupErrorReported) {
                runtimeServices->startupErrorReported = true;
                toast.Show(error);
            }
            return [] {};
        },
        runtimeServices->startupError);

    huxerui::Lifecycle(
        [darkTheme] {
            ApplyWindowsNativeMenuTheme(darkTheme);
            return [] {};
        },
        darkTheme);

    // 托盘与单实例唤醒在应用级任务中运行，主窗口 Retire 后继续响应。
    huxerui::Lifecycle(
        [startupTasks, trayRuntime, tray, settingsModel] {
            if (!trayRuntime->started) {
                trayRuntime->started = true;
                tray.OnActivate([trayRuntime] { trayRuntime->ShowWindow(); });
                startupTasks.Launch([trayRuntime, tray, settingsModel]() -> huxerui::Task<void> {
                    for (;;) {
                        if (instance::consumeActivation()) trayRuntime->ShowWindow();
                        const SettingsView settings = settingsModel->view.Get();
                        if (trayRuntime->retired &&
                            (!tray.IsAvailable() || (settings.ready && !settings.trayEnabled)))
                            trayRuntime->ShowWindow();
                        try {
                            if (trayRuntime->refresh) trayRuntime->refresh();
                        } catch (const std::exception& error) {
                            stream::logApplication("error", std::string{"刷新托盘菜单失败："} + error.what());
                        }
                        co_await huxerui::Delay(std::chrono::duration<double>{0.25});
                    }
                });
            }
            return [] {};
        },
        0);

    // 每次 GUI 启动都检查已安装服务，随后按设置自启内核；任务线程执行
    // 阻塞 IPC/升级，应用级保留状态避免重组重复检查；不主动安装或触发 UAC。
    huxerui::Lifecycle(
        [startupTasks, coreModel, settingsModel, toast, control] {
            const auto noCleanup = [] {};
            const SettingsView settings = settingsModel->view.Get();
            if (!settings.ready || coreModel->startupServiceMaintenanceStarted ||
                coreModel->serviceChecking.Get() || coreModel->operations.Busy() || cli::runtimeCommandMode()) return noCleanup;
            coreModel->startupServiceMaintenanceStarted = true;
            coreModel->serviceChecking = true;
            const bool startupAutoRun = settings.autoRun;
            startupTasks.Launch([=]() -> huxerui::Task<void> {
                try {
                    const auto maintenance = co_await RunOnTaskThread([] {
                        auto& core = store::coreStore();
                        core.init();
                        // 若上次异常退出把系统代理留在本应用端口上，先撤销。
                        // 仅限确实指向本应用的设置，不动其他代理软件的接管。
                        core.releaseStaleOwnedSystemProxy();
                        if (HasDesktopCoreRuntime()) {
                            // 清理上次异常退出留下的旧内核（含仍持有旧 TUN 配置
                            // 的进程）。
                            if (!core.stopCore()) {
                                return service::StartupResult{
                                    .ready = false,
                                    .error = core.snapshot().lastError};
                            }
                        }
                        const auto maintenance = core.synchronizeService();
                        if (!maintenance.ready) {
                            stream::logApplication("warning", "启动服务检查/更新失败：" + maintenance.error);
                            return maintenance;
                        }
                        if (maintenance.updated)
                            stream::logApplication("info", "已将后台网络服务更新到当前应用版本");
                        return maintenance;
                    });
                    auto next = co_await RunOnTaskThread([] { return ReadCoreView(true, false); });
                    coreModel->Update([next = std::move(next)](CoreView& view) { view = next; });
                    coreModel->serviceChecking = false;
                    coreModel->RequestRefresh();
                    if (maintenance.ready && startupAutoRun && HasDesktopCoreRuntime())
                        control.Run({.action = CoreAction::Start});
                    if (!maintenance.ready)
                        toast.Show(maintenance.error.empty() ? Localized("服务操作失败")
                                                            : huxerui::StringVariant(maintenance.error));
                } catch (const std::exception& exception) {
                    coreModel->serviceChecking = false;
                    coreModel->RequestRefresh();
                    stream::logApplication("error", std::string{"启动服务任务异常："} + exception.what());
                    toast.Show(exception.what());
                } catch (...) {
                    coreModel->serviceChecking = false;
                    coreModel->RequestRefresh();
                    stream::logApplication("error", "启动服务任务未知异常");
                    toast.Show(Localized("服务操作失败"));
                }
            });
            return noCleanup;
        },
        settingsModel->view, coreModel->serviceChecking, coreModel->operations.command);

    // 系统 VPN 的断开可能要等待 pppd/RAS 收尾，先完成清理再关闭窗口。
    const std::weak_ptr<DesktopTrayRuntime> weakTrayRuntime = trayRuntime;
    auto finishExit = [weakTrayRuntime] {
        if (const auto trayRuntime = weakTrayRuntime.lock()) trayRuntime->RequestExit();
    };

#include "ui/platform_tray_menu.inc"

    // 关闭按钮只由托盘开关控制。托盘宿主尚未就绪时仍需确认退出，
    // 避免隐藏后没有入口唤回窗口；事件中动态查询宿主可用性。
    const huxerui::Color closeHintColor = rootSpec.colors.on_surface_variant;
    auto hideWindow = [startupTasks, trayRuntime, settingsModel] {
        const bool lightweight = settingsModel->view.Get().lightweightTray;
        // Native close callbacks must finish before the host window is retired.
        startupTasks.Launch([trayRuntime, lightweight]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            trayRuntime->HideWindow(lightweight);
        });
    };
    // 最小化交给平台原生行为，保留任务栏入口；轻量托盘只用于关闭隐藏。
    window.OnCloseRequest(
        [=]() mutable -> bool {
            if (trayRuntime->exitRequested) return true;
            const SettingsView settings = settingsModel->view.Get();
            const bool trayEnabled = settings.ready
                ? settings.trayEnabled : startupTrayEnabled;
            if (trayEnabled && tray.IsAvailable() && !trayRuntime->trayRefreshFailed) {
                hideWindow();
                return true;
            }
            if (closeDialogOpen.Get()) return true;
            closeDialogOpen = true;
            dialog.Show(
                [=](huxerui::DialogContext ctx) -> huxerui::View {
                    return DialogCard(huxerui::Column{
                        huxerui::Text(Localized("关闭 Clash-Flux？"), huxerui::TextRole::Title),
                        huxerui::Text(Localized("关闭应用会停止代理，是否继续？"))
                            .Style(huxerui::TextStyle{
                                huxerui::Font::System(font_size::kCaption),
                                closeHintColor}),
                        huxerui::Row{
                            huxerui::Button(Localized("关闭")).OnClick([=] {
                                ctx.Dismiss();
                                closeDialogOpen = false;
                                finishExit();
                            }),
                            huxerui::Button(Localized("取消")).OnClick([=] {
                                ctx.Dismiss();
                                closeDialogOpen = false;
                            }),
                        }.With(huxerui::Spacing(8.0F),
                               huxerui::MainAlign(
                                   huxerui::MainAxisAlignment::SpaceBetween)),
                    }.With(huxerui::Spacing(12.0F),
                           huxerui::Frame{.width = 420.0F},
                           huxerui::CrossAlign(
                               huxerui::CrossAxisAlignment::Stretch)));
                },
                huxerui::DialogOptions{
                    .dismiss_on_outside_press = false,
                    .dismiss_on_cancel = false});
            return true;
        },
        0);

    return {};
}

[[huxerui::composable]] huxerui::View DesktopAppContent(
    huxerui::View mainRow, const huxerui::ThemeSpec& rootSpec) {
    // 每页骨架持有自己的标题栏与动作，保留页面状态及原始回调作用域。
    return huxerui::View{mainRow}.With(
        huxerui::Background(rootSpec.colors.background), huxerui::ClipChildren());
}

#endif

} // namespace clashflux::ui
