// platform_app.cpp — 平台专属应用壳层与订阅刷新泵。
//
// AppRoot 只负责组装通用页面；窗口/托盘生命周期、Android 数据目录和
// 平台网络刷新通道在这里按平台函数整体实现，再由调用点宏选择。
#include <huxerui/huxerui.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
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
#include "app_resources.h"
#include "task_bridge.h"

import clashflux.config;
import clashflux.core;
import clashflux.db;
import clashflux.service;
import clashflux.store.core;
import clashflux.store.profiles;
import clashflux.store.vpn;
#if !defined(__ANDROID__)
import clashflux.instance;
#endif

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
    const huxerui::ApplicationHandle&, const huxerui::ThemeSpec&) {
    return {};
}

[[huxerui::composable]] huxerui::View AndroidAppContent(
    huxerui::View mainRow, const huxerui::ThemeSpec& rootSpec) {
    huxerui::View content = mainRow;
    return std::move(content).With(
        huxerui::Background(rootSpec.colors.background),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

#else

void DesktopPreparePlatformDataDirectory(
    const huxerui::ApplicationHandle& application) {
    static_cast<void>(application);
}

[[huxerui::composable]] huxerui::View DesktopProfileRefreshPump() {
    auto tasks = huxerui::UseTaskScope();
    huxerui::Lifecycle(
        [tasks] {
            tasks.Launch([]() -> huxerui::Task<void> {
                for (;;) {
                    co_await huxerui::Delay(std::chrono::duration<double>{30.0});
                    co_await RunOnTaskThread([] {
                        store::profilesStore().refreshDue();
                    });
                }
            });
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

struct TrayRuntimeSnapshot {
    bool coreRunning = false;
    bool systemProxyIntent = false;
    bool tunIntent = false;
    bool systemProxyActive = false;
    bool tunActive = false;
    bool trayEnabled = true;
};

struct TrayOperationResult {
    bool ok = false;
    std::string error;
};

TrayRuntimeSnapshot ReadTrayRuntimeSnapshot() {
    auto& core = store::coreStore();
    core.checkAlive();
    const store::CoreSnapshot snapshot = core.snapshot();
    const bool running = snapshot.state == core::CoreState::Running;
    return TrayRuntimeSnapshot{
        .coreRunning = running,
        .systemProxyIntent = core.systemProxyEnabled(),
        .tunIntent = snapshot.tunEnabled,
        .systemProxyActive = running && core.systemProxyActive(),
        .tunActive = running && snapshot.tunEnabled,
        .trayEnabled = core.setting("tray.enabled", "true") == "true",
    };
}

} // namespace

[[huxerui::composable]] huxerui::View DesktopApplicationEffects(
    const huxerui::ApplicationHandle& application,
    const huxerui::ThemeSpec& rootSpec) {
    const huxerui::WindowHandle window = huxerui::UseWindow();
    const huxerui::SystemTrayHandle tray = application.SystemTray();
    const bool trayAvailable = tray.IsAvailable();
    auto tasks = huxerui::UseTaskScope();
    // 运行态只负责图标配色；菜单的乐观态独立保存，这样启动/停止耗时期间
    // 菜单立即响应，但状态图标仍等到后台确认内核运行后才改变。
    auto trayCoreRunning = huxerui::UseState(false);
    auto trayCoreMenuRunning = huxerui::UseState(false);
    auto traySysProxy = huxerui::UseState(false);
    auto trayTun = huxerui::UseState(false);
    auto traySysProxyActive = huxerui::UseState(false);
    auto trayTunActive = huxerui::UseState(false);
    auto trayCorePending = huxerui::UseState(false);
    auto traySysProxyPending = huxerui::UseState(false);
    auto trayTunPending = huxerui::UseState(false);
    auto trayPollRevision = huxerui::UseState(std::uint64_t{0});
    auto trayProfiles = huxerui::UseState<std::vector<db::Profile>>({});
    auto trayProxyGroups = huxerui::UseState<std::vector<ProxyGroupSnapshot>>({});
    auto trayEnabled = huxerui::UseState(
        store::coreStore().setting("tray.enabled", "true") == "true");
    auto closeDialogOpen = huxerui::UseState(false);
    auto exitRequested = huxerui::UseState(false);
    auto dialog = huxerui::UseDialog();
    auto clipboard = application.Clipboard();
    auto toast = huxerui::UseToast();
    const bool darkTheme = IsDarkTheme(rootSpec);

    huxerui::Lifecycle(
        [darkTheme] {
            ApplyWindowsNativeMenuTheme(darkTheme);
            return [] {};
        },
        darkTheme);

    // 第二次启动通过单实例通道只发一个唤醒事件；这里在 UI 线程轻量轮询，
    // 不参与内核/REST 工作，确保隐藏到托盘后也能被再次打开。
    huxerui::Lifecycle(
        [tasks, window] {
            tasks.Launch([window]() -> huxerui::Task<void> {
                co_await PollWhile(std::chrono::duration<double>{0.1}, [window] {
                    if (instance::consumeActivation()) {
                        window.Show();
                        window.Activate();
                    }
                    return true;
                });
            });
            return [] {};
        },
        0);

    // 内核自启 + 崩溃检测泵：启动和存活检查全部在任务线程执行。
    huxerui::Lifecycle(
        [tasks, trayCoreRunning, trayCoreMenuRunning, traySysProxy, trayTun,
         traySysProxyActive, trayTunActive, trayCorePending,
         traySysProxyPending, trayTunPending, trayEnabled, trayPollRevision] {
            tasks.Launch([=]() -> huxerui::Task<void> {
                co_await RunOnTaskThread([] {
                    auto& core = store::coreStore();
                    core.init();
                    // 若上次异常退出把系统代理留在本应用端口上，先撤销。
                    // 仅限确实指向本应用的设置，不动其他代理软件的接管。
                    core.releaseStaleOwnedSystemProxy();
                    if (!cfg::singboxBinary().empty()) {
                        // 清理上次异常退出留下的旧内核（含仍持有旧 TUN 配置
                        // 的进程）。
                        core.stopCore();
                        // 内核启停与流量接管解耦：默认不在启动应用时拉起内核
                        // （内核只是本地混合端口 + 控制接口，需要时由首页右下角
                        // 悬浮按钮显式启动）；只有用户打开「启动时自动运行内核」
                        // 才在这里拉起，并按已记录的 TUN / 系统代理意图恢复接管。
                        if (core.setting("app.auto_run", "false") == "true") {
                            const bool resumeSysProxy = core.systemProxyEnabled();
                            const bool resumeTun =
                                core.setting("core.tun_enabled", "false") == "true";
                            core.startCore(
                                store::profilesStore().selectedYaml(), false,
                                resumeTun, resumeSysProxy);
                        }
                    }
                });
                for (;;) {
                    const std::uint64_t revision = trayPollRevision.Get();
                    const TrayRuntimeSnapshot snapshot = co_await RunOnTaskThread(
                        [] { return ReadTrayRuntimeSnapshot(); });
                    if (revision == trayPollRevision.Get()) {
                        // applyTun 在内核运行时需要重启。保留本次乐观模式更新
                        // 的运行态，等操作完成后再读回确认，避免状态图标闪回默认色。
                        if (!trayTunPending.Get()) {
                            trayCoreRunning = snapshot.coreRunning;
                        }
                        if (!trayCorePending.Get()) {
                            trayCoreMenuRunning = snapshot.coreRunning;
                        }
                        if (!trayTunPending.Get() &&
                            !traySysProxyPending.Get()) {
                            traySysProxy = snapshot.systemProxyIntent;
                            traySysProxyActive = snapshot.systemProxyActive;
                        }
                        if (!trayTunPending.Get()) {
                            trayTun = snapshot.tunIntent;
                            trayTunActive = snapshot.tunActive;
                        }
                        trayEnabled = snapshot.trayEnabled;
                    }
                    co_await huxerui::Delay(
                        std::chrono::duration<double>{0.5});
                }
            });
            return [] {};
        },
        0);

    // 托盘菜单需要的是当前可选订阅和运行中的策略组快照。数据库/API 读取
    // 全部放到任务线程，菜单本身只消费最近一次轻量快照。
    huxerui::Lifecycle(
        [tasks, trayProfiles, trayProxyGroups] {
            tasks.Launch([trayProfiles, trayProxyGroups]() -> huxerui::Task<void> {
                for (;;) {
                    const auto profiles = co_await RunOnTaskThread(
                        [] { return store::profilesStore().list(); });
                    trayProfiles = profiles;
                    const std::string body = co_await RunOnTaskThread([] {
                        return store::coreStore().snapshot().state ==
                                       core::CoreState::Running
                                   ? ProxyGroupsSnapshot()
                                   : std::string{};
                    });
                    trayProxyGroups = ParseProxyGroups(body);
                    co_await huxerui::Delay(std::chrono::duration<double>{1.0});
                }
            });
            return [] {};
        },
        0);

    // 系统 VPN 的断开可能要等待 pppd/RAS 收尾，先完成清理再关闭窗口。
    auto finishExit = [tasks, application, tray, window, exitRequested]() {
        if (exitRequested.Get()) return;
        exitRequested = true;
        clashflux::instance::markClosing();
        tray.Hide();
        window.Hide();
        tasks.Launch([application]() -> huxerui::Task<void> {
            co_await RunOnTaskThread([] {
                store::vpnStore().shutdown();
                store::coreStore().stopCore();
            });
            application.Quit();
        });
        // 退出保底看门狗：避免任何底层阻塞（网络断开超时、平台事件循环等）导致后台残留僵尸进程
        std::thread([] {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            std::_Exit(0);
        }).detach();
    };

    if (trayAvailable) {
        tray.OnActivate([window] {
            window.Show();
            window.Activate();
        });
        huxerui::Lifecycle(
            [tray, window, application, tasks, trayCoreRunning,
             trayCoreMenuRunning, traySysProxy, trayTun, traySysProxyActive,
             trayTunActive, trayCorePending, traySysProxyPending,
             trayTunPending, trayPollRevision, dialog, clipboard, toast,
             trayProfiles, trayProxyGroups, trayEnabled, finishExit,
             textColor = rootSpec.colors.on_surface,
             hintColor = rootSpec.colors.on_surface_variant] {
                if (trayEnabled.Get()) {
                    std::vector<huxerui::MenuEntry> menuEntries;
                    menuEntries.push_back(
                        huxerui::MenuItem("显示主窗口", [window] {
                            window.Show();
                            window.Activate();
                        }));
                    menuEntries.push_back(huxerui::MenuSection{});
                    std::vector<huxerui::MenuEntry> profileEntries;
                    for (const db::Profile& profile : trayProfiles.Get()) {
                        if (profile.type == "pptp" || profile.type == "openvpn") {
                            continue;
                        }
                        profileEntries.push_back(
                            huxerui::MenuItem(
                                profile.name,
                                [tasks, toast, trayProfiles, id = profile.id] {
                                    tasks.Launch(
                                        [tasks, toast, trayProfiles, id]()
                                            -> huxerui::Task<void> {
                                            const std::string error =
                                                co_await RunOnTaskThread([id] {
                                                    auto& profiles =
                                                        store::profilesStore();
                                                    return profiles.activate(id)
                                                               ? std::string{}
                                                               : profiles.lastError();
                                                });
                                            if (!error.empty()) toast.Show(error);
                                            trayProfiles = co_await RunOnTaskThread(
                                                [] { return store::profilesStore().list(); });
                                        });
                                })
                                .Checked(profile.selected));
                    }
                    if (profileEntries.empty()) {
                        profileEntries.push_back(
                            huxerui::MenuItem("暂无可用订阅", [] {}).Enabled(false));
                    }
                    menuEntries.push_back(huxerui::MenuItem(
                        "选择订阅", std::move(profileEntries)));
                    menuEntries.push_back(huxerui::MenuItem(
                        "切换当前订阅线路",
                        BuildProxyLineMenu(
                            trayProxyGroups.Get(),
                            [tasks, toast, trayProxyGroups](
                                const std::string& group, const std::string& name) {
                                tasks.Launch([toast, trayProxyGroups, group, name]()
                                                 -> huxerui::Task<void> {
                                    const bool ok = co_await RunOnTaskThread(
                                        [group, name] {
                                            return SelectProxyLine(group, name);
                                        });
                                    if (!ok) {
                                        const std::string error =
                                            store::coreStore().snapshot().lastError;
                                        toast.Show(error.empty() ? "线路切换失败" : error);
                                        co_return;
                                    }
                                    auto groups = trayProxyGroups.Get();
                                    for (ProxyGroupSnapshot& proxyGroup : groups) {
                                        if (proxyGroup.name == group) {
                                            proxyGroup.current = name;
                                        }
                                    }
                                    trayProxyGroups = std::move(groups);
                                });
                            })));
                    // 内核启停是独立动作（与系统代理/TUN 解耦）：启动时按已
                    // 记录的 TUN / 系统代理意图恢复接管。
                    menuEntries.push_back(
                        huxerui::MenuItem(
                            trayCoreMenuRunning.Get() ? "停止内核" : "启动",
                            [tasks, toast, trayCoreMenuRunning, trayCorePending,
                             trayPollRevision] {
                                if (trayCorePending.Get()) return;
                                const bool previous = trayCoreMenuRunning.Get();
                                const bool next = !previous;
                                trayCorePending = true;
                                trayCoreMenuRunning = next;
                                trayPollRevision = trayPollRevision.Get() + 1;
                                tasks.Launch([=]() -> huxerui::Task<void> {
                                    TrayOperationResult result;
                                    try {
                                        result = co_await RunOnTaskThread([next] {
                                            auto& core = store::coreStore();
                                            if (!next) {
                                                const bool ok = core.stopCore();
                                                return TrayOperationResult{
                                                    .ok = ok,
                                                    .error = core.snapshot().lastError};
                                            }
                                            const bool resumeSysProxy =
                                                core.systemProxyEnabled();
                                            const bool resumeTun =
                                                core.setting("core.tun_enabled",
                                                             "false") == "true";
                                            core.startCore(
                                                store::profilesStore().selectedYaml(),
                                                false, resumeTun, resumeSysProxy);
                                            const auto snapshot = core.snapshot();
                                            return TrayOperationResult{
                                                .ok = snapshot.state ==
                                                      core::CoreState::Running,
                                                .error = snapshot.lastError};
                                        });
                                    } catch (const std::exception& error) {
                                        result.error = error.what();
                                    }
                                    trayCorePending = false;
                                    trayPollRevision = trayPollRevision.Get() + 1;
                                    if (!result.ok) {
                                        trayCoreMenuRunning = previous;
                                        toast.Show(
                                            result.error.empty()
                                                ? (next ? "启动内核失败" : "停止内核失败")
                                                : result.error);
                                    }
                                });
                            })
                            .Checked(trayCoreMenuRunning.Get())
                            .Enabled(!trayCorePending.Get()));
                    menuEntries.push_back(
                        huxerui::MenuItem(
                            "系统代理", [tasks, traySysProxy,
                                         traySysProxyActive, traySysProxyPending,
                                         trayCoreRunning, trayPollRevision,
                                         toast] {
                                if (traySysProxyPending.Get()) return;
                                const bool previous = traySysProxy.Get();
                                const bool previousActive =
                                    traySysProxyActive.Get();
                                const bool next = !previous;
                                traySysProxyPending = true;
                                traySysProxy = next;
                                if (trayCoreRunning.Get()) {
                                    traySysProxyActive = next;
                                }
                                trayPollRevision = trayPollRevision.Get() + 1;
                                tasks.Launch([=]() -> huxerui::Task<void> {
                                    TrayOperationResult result;
                                    try {
                                        const DesktopModeApplyResult applyResult =
                                            co_await RunOnTaskThread([next] {
                                                return ApplyDesktopSystemProxy(next);
                                            });
                                        result = TrayOperationResult{
                                            .ok = applyResult.status ==
                                                  DesktopModeApplyStatus::Applied,
                                            .error = applyResult.error};
                                    } catch (const std::exception& error) {
                                        result.error = error.what();
                                    }
                                    traySysProxyPending = false;
                                    trayPollRevision =
                                        trayPollRevision.Get() + 1;
                                    if (!result.ok) {
                                        traySysProxy = previous;
                                        traySysProxyActive = previousActive;
                                        toast.Show(result.error.empty()
                                                       ? "系统代理切换失败"
                                                       : result.error);
                                    }
                                });
                            })
                            .Checked(traySysProxy.Get())
                            .Enabled(!traySysProxyPending.Get()));
                    menuEntries.push_back(
                        huxerui::MenuItem(
                            "TUN 模式",
                            [tasks, trayTun, trayTunActive, trayTunPending,
                             trayCoreRunning, trayPollRevision, window, dialog,
                             clipboard, toast, textColor, hintColor] {
                                if (trayTunPending.Get()) return;
                                const bool previous = trayTun.Get();
                                const bool previousActive = trayTunActive.Get();
                                const bool next = !previous;
                                trayTunPending = true;
                                trayTun = next;
                                if (trayCoreRunning.Get()) {
                                    trayTunActive = next;
                                }
                                trayPollRevision = trayPollRevision.Get() + 1;
                                tasks.Launch([=]() -> huxerui::Task<void> {
                                    DesktopModeApplyResult result;
                                    try {
                                        result = co_await RunOnTaskThread([next] {
                                            return ApplyDesktopTun(next);
                                        });
                                    } catch (const std::exception& error) {
                                        result.error = error.what();
                                    }
                                    trayTunPending = false;
                                    trayPollRevision =
                                        trayPollRevision.Get() + 1;
                                    if (result.status !=
                                        DesktopModeApplyStatus::Applied) {
                                        trayTun = previous;
                                        trayTunActive = previousActive;
                                        if (result.status ==
                                            DesktopModeApplyStatus::ElevationRequested) {
                                            toast.Show("已请求管理员权限重启，请在新窗口开启 TUN");
                                        } else if (result.status ==
                                                   DesktopModeApplyStatus::PermissionDenied) {
                                            window.Activate();
                                            ShowTunGuideDialog(
                                                dialog, clipboard, toast,
                                                textColor, hintColor);
                                        } else {
                                            toast.Show(result.error.empty()
                                                           ? "TUN 模式切换失败"
                                                           : result.error);
                                        }
                                    }
                                });
                            })
                            .Checked(trayTun.Get())
                            .Enabled(!trayTunPending.Get()));
                    menuEntries.push_back(huxerui::MenuSection{});
                    menuEntries.push_back(
                        huxerui::MenuItem("退出", [finishExit] { finishExit(); }));
                    huxerui::ImageVariant trayIcon = app::images::tray_default;
                    if (trayCoreRunning.Get()) {
                        if (trayTunActive.Get()) {
                            trayIcon = app::images::tray_tun;
                        } else if (traySysProxyActive.Get()) {
                            trayIcon = app::images::tray_system_proxy;
                        }
                    }
                    tray.Show(std::move(trayIcon),
                              huxerui::SystemTrayOptions{
                                  .tooltip = "Clash-Flux",
                                  .menu = std::move(menuEntries)});
                }
                return [tray] { tray.Hide(); };
            },
            trayCoreRunning, trayCoreMenuRunning, traySysProxy, trayTun,
            traySysProxyActive, trayTunActive, trayCorePending,
            traySysProxyPending, trayTunPending, trayEnabled);
    }

    // 关闭窗口行为：托盘可用时按设置询问/退出/最小化到托盘。
    // trayAvailable 只是当前帧的快照；关闭事件可能在托盘宿主晚就绪后
    // 才发生，因此事件处理必须动态查询 tray.IsAvailable()。
    const huxerui::Color closeHintColor = rootSpec.colors.on_surface_variant;
    auto hideWindow = [tasks, window] {
        // GTK/Win32/macOS 的关闭回调都在平台事件栈中执行。把 Hide 推迟
        // 到下一帧，避免隐藏最后一个窗口时让运行时在回调中途拆毁自己。
        tasks.Launch([window]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            window.Hide();
        });
    };
    window.OnCloseRequest(
        [=]() mutable -> bool {
            if (exitRequested.Get()) return true;
            if (!tray.IsAvailable() || !trayEnabled.Get()) {
                finishExit();
                return true;
            }
            const std::string behavior =
                store::coreStore().setting("tray.close_behavior", "0");
            if (behavior == "1") {
                finishExit();
                return true;
            }
            if (behavior == "2") {
                hideWindow();
                return true;
            }
            if (closeDialogOpen.Get()) return true;
            closeDialogOpen = true;
            dialog.Show(
                [=](huxerui::DialogContext ctx) -> huxerui::View {
                    return DialogCard(huxerui::Column{
                        huxerui::Text("关闭 Clash-Flux？", huxerui::TextRole::Title),
                        huxerui::Text("直接关闭会停止代理；最小化到托盘后代理继续运行。")
                            .Style(huxerui::TextStyle{
                                huxerui::Font::System(font_size::kCaption),
                                closeHintColor}),
                        huxerui::Row{
                            huxerui::Button("直接关闭").OnClick([=] {
                                ctx.Dismiss();
                                closeDialogOpen = false;
                                finishExit();
                            }),
                            huxerui::Button("最小化到托盘").OnClick([=] {
                                ctx.Dismiss();
                                closeDialogOpen = false;
                                hideWindow();
                            }),
                            huxerui::Button("取消").OnClick([=] {
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

    // 只在这个壳层生命周期首次挂载时执行一次。此前直接写在组合函数末尾，
    // 页面切换造成重组后会再次 Hide，表现为“切换页面就缩到托盘”。
    const bool hideOnStartup =
        trayAvailable && trayEnabled.Get() &&
        store::coreStore().setting("tray.start_minimized", "false") == "true";
    huxerui::Lifecycle(
        [window, hideOnStartup] {
            if (hideOnStartup) window.Hide();
            return [] {};
        },
        0);
    return {};
}

[[huxerui::composable]] huxerui::View DesktopAppContent(
    huxerui::View mainRow, const huxerui::ThemeSpec& rootSpec) {
    // 桌面标题栏和拖拽区只存在于桌面壳函数，Android 不会组合这些节点。
    huxerui::View content = mainRow;
    return huxerui::Column {
        huxerui::WindowTitleBar {
            huxerui::Row {
                huxerui::Image(app::images::mascot_logo_dark)
                    .Fit(huxerui::ImageFit::Contain)
                    .With(huxerui::Frame{.width = 20.0F, .height = 20.0F}),
            }
                .With(huxerui::Frame{.width = 72.0F, .height = 20.0F},
                      huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                      huxerui::WindowDragRegion{}),
            huxerui::Spacer{}.With(huxerui::Grow(1.0F),
                                   huxerui::WindowDragRegion{}),
        }
            .With(huxerui::Spacing(rootSpec.spacing.small)),
        std::move(content),
    }
        .With(huxerui::Spacing(rootSpec.spacing.extra_small),
              huxerui::Background(rootSpec.colors.background),
              huxerui::ClipChildren(),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

#endif

} // namespace clashflux::ui
