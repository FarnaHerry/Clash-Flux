// platform_app.cpp — 平台专属应用壳层与订阅刷新泵。
//
// AppRoot 只负责组装通用页面；窗口/托盘生命周期、Android 数据目录和
// 平台网络刷新通道在这里按平台函数整体实现，再由调用点宏选择。
#include <huxerui/huxerui.h>

#include <chrono>
#include <atomic>
#include <cstdint>
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
    const huxerui::ApplicationHandle&, const huxerui::ThemeSpec&,
    bool, bool, bool) {
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
    std::optional<huxerui::ApplicationHandle> application;
    std::optional<huxerui::WindowHandle> window;
    std::optional<CoreControl> control;
    std::function<void()> refresh;
    bool started = false;
    bool startupVisibilityHandled = false;
    bool retired = false;
    bool exitRequested = false;
    std::uint64_t windowGeneration = 0;
    std::string presentationKey;

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
        bool opening = retired;
        try {
            if (retired) {
                ++windowGeneration;
                retired = false;
                window = application->OpenWindow(huxerui::WindowOptions{
                    .title = "Clash-Flux",
                    .initial_size = {1080.0F, 720.0F},
                    .minimum_size = huxerui::Size{320.0F, 480.0F},
                    .chrome_mode = huxerui::WindowChromeMode::Custom,
                    .title_bar_height = kDesktopTitleBarHeight,
                });
                opening = false;
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
    context.Provide(std::make_shared<DesktopTrayRuntime>());
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
    bool startupTrayEnabled, bool startupMinimized, bool startupLightweight) {
    const huxerui::WindowHandle window = huxerui::UseWindow();
    // 策略组快照 / 内核与接管状态的全局唯一来源（见 *_model.h）：托盘菜单只读。
    const auto proxiesModel = huxerui::UseService<ProxiesModel>();
    const auto coreModel = huxerui::UseService<CoreModel>();
    const auto profilesModel = huxerui::UseService<ProfilesModel>();
    const auto settingsModel = huxerui::UseService<SettingsModel>();
    const auto trayRuntime = huxerui::UseService<DesktopTrayRuntime>();
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
    auto finishExit = [startupTasks, application, tray, weakTrayRuntime]() {
        const auto trayRuntime = weakTrayRuntime.lock();
        if (!trayRuntime) return;
        if (trayRuntime->exitRequested) return;
        trayRuntime->exitRequested = true;
        trayRuntime->refresh = {};
        clashflux::instance::markClosing();
        tray.Hide();
        if (trayRuntime->window) trayRuntime->window->Hide();
        auto coreStopped = std::make_shared<std::atomic_bool>(false);
        auto cleanupFinished = std::make_shared<std::atomic_bool>(false);
        startupTasks.Launch([application, coreStopped, cleanupFinished]() -> huxerui::Task<void> {
            bool stopped = co_await RunOnTaskThread([] {
                store::vpnStore().shutdown();
                return store::coreStore().stopCore();
            });
            if (!stopped) {
                stopped = co_await RunOnTaskThread([] {
                    return store::coreStore().stopCore();
                });
                if (!stopped)
                    stream::logApplication("error", "退出时停止内核失败：" +
                        store::coreStore().snapshot().lastError);
            }
            coreStopped->store(stopped);
            // 写后缓存模型：退出前把 settings/profiles 的脏数据落库。失败只能
            // 记日志（退出路径没有界面），但绝不能再静默——那等于用户改动凭空消失。
            auto& store = clashflux::persistence::persistence();
            const bool settingsSaved = co_await store.flushSettings();
            const bool profilesSaved = co_await store.flushProfiles();
            if (!settingsSaved || !profilesSaved) {
                stream::logApplication(
                    "error",
                    "退出前落库失败：" + (store.lastError().empty()
                                             ? std::string{"未知原因"}
                                             : store.lastError()));
            }
            cleanupFinished->store(true);
            application.Quit();
        });
        std::thread([coreStopped, cleanupFinished] {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            if (!coreStopped->load()) store::coreStore().stopCore();
            while (!cleanupFinished->load())
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::this_thread::sleep_for(std::chrono::seconds(3));
            std::_Exit(0);
        }).detach();
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
    window.OnMinimizeRequest([=]() -> bool {
        const SettingsView settings = settingsModel->view.Get();
        if (settings.lightweightTray &&
            (settings.ready ? settings.trayEnabled : startupTrayEnabled) &&
            tray.IsAvailable()) {
            hideWindow();
            return true;
        }
        return false;
    }, 0);
    window.OnCloseRequest(
        [=]() mutable -> bool {
            if (trayRuntime->exitRequested) return true;
            const SettingsView settings = settingsModel->view.Get();
            const bool trayEnabled = settings.ready
                ? settings.trayEnabled : startupTrayEnabled;
            if (trayEnabled && tray.IsAvailable()) {
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

    // 用首帧快照决定初始窗口可见性，不等设置 hydrate 后再隐藏造成窗口闪现。
    // 只在壳层首次挂载时处理一次；伪 CLI 模式同样不渲染窗口。
    huxerui::Lifecycle(
        [window, tray, startupTasks, trayRuntime, startupTrayEnabled,
         startupMinimized, startupLightweight] {
            const bool commandMode = cli::runtimeCommandMode();
            if (!trayRuntime->startupVisibilityHandled) {
                trayRuntime->startupVisibilityHandled = true;
                if (commandMode) {
                    window.Hide();
                } else if (tray.IsAvailable() && startupTrayEnabled && startupMinimized) {
                    if (startupLightweight) {
                        startupTasks.Launch([trayRuntime]() -> huxerui::Task<void> {
                            co_await huxerui::Delay(std::chrono::duration<double>{0});
                            if (trayRuntime->refresh) trayRuntime->refresh();
                            trayRuntime->HideWindow(true);
                        });
                    } else {
                        window.Hide();
                    }
                }
            }
            return [] {};
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
