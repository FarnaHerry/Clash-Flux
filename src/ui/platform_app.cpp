// platform_app.cpp — 平台专属应用壳层与订阅刷新泵。
//
// AppRoot 只负责组装通用页面；窗口/托盘生命周期、Android 数据目录和
// 平台网络刷新通道在这里按平台函数整体实现，再由调用点宏选择。
#include <huxerui/huxerui.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <mutex>
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
    bool, bool) {
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

// 托盘激活处理器是整个 Runtime 的一次性连接，不是组合生命周期订阅：HuxerUI
// 固定 revision 的 SystemTrayHandle::OnActivate 直接连接服务，第二次调用抛
// std::logic_error("... already connected")。它写在组合函数体里，任何一次
// 外层重组都会重复连接，未捕获异常冒泡到 LinuxUiWindow::Run 后 abort，表现
// 为「打开 TUN / 系统代理等任意开关就闪退」。按框架约定用 call_once 保证
// 进程内只连接一次；handler 常驻到 Runtime 结束，与托盘宿主是否就绪无关。
std::once_flag g_trayActivationOnce;

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
    std::string startCore;
    std::string stopCore;
    std::string systemProxy;
    std::string tun;
    std::string quit;
};

} // namespace

[[huxerui::composable]] huxerui::View DesktopApplicationEffects(
    const huxerui::ApplicationHandle& application,
    const huxerui::ThemeSpec& rootSpec,
    bool startupTrayEnabled, bool startupMinimized) {
    const huxerui::WindowHandle window = huxerui::UseWindow();
    // 策略组快照 / 内核与接管状态的全局唯一来源（见 *_model.h）：托盘菜单只读。
    const auto proxiesModel = huxerui::UseService<ProxiesModel>();
    const auto coreModel = huxerui::UseService<CoreModel>();
    const auto profilesModel = huxerui::UseService<ProfilesModel>();
    const auto settingsModel = huxerui::UseService<SettingsModel>();
    const huxerui::SystemTrayHandle tray = application.SystemTray();
    const bool trayAvailable = tray.IsAvailable();
    auto tasks = huxerui::UseTaskScope();
    const auto control = UseCoreControl();
    // Native menu/icon projections are read-only mirrors of the application model.
    auto trayCoreRunning = huxerui::UseState(false);
    auto trayCoreMenuRunning = huxerui::UseState(false);
    auto traySysProxy = huxerui::UseState(false);
    auto trayTun = huxerui::UseState(false);
    auto traySysProxyActive = huxerui::UseState(false);
    auto trayTunActive = huxerui::UseState(false);
    auto trayProfiles = huxerui::UseState<std::vector<db::Profile>>({});
    auto trayProxyGroups = huxerui::UseState<std::vector<ProxyGroupSnapshot>>({});
    auto startupVisibilityHandled = huxerui::UseState(false);
    auto startupRuntimeHandled = huxerui::UseState(false);
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

    // 第二次启动通过单实例通道只发一个唤醒事件；这里在 UI 线程轻量轮询
    // （0.25s：唤醒延迟肉眼无差，但不参与内核/REST 工作），确保隐藏到托盘后
    // 也能被再次打开。改成阻塞等待需要信号掩码/命名事件层面的改动，收益不足。
    huxerui::Lifecycle(
        [tasks, window] {
            tasks.Launch([window]() -> huxerui::Task<void> {
                co_await PollWhile(std::chrono::duration<double>{0.25}, [window] {
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

    // 每次 GUI 启动都检查已安装服务，随后按设置自启内核；任务线程执行
    // 阻塞 IPC/升级，应用级保留状态避免重组重复检查；不主动安装或触发 UAC。
    huxerui::Lifecycle(
        [tasks, trayCoreRunning, trayCoreMenuRunning, traySysProxy, trayTun,
         traySysProxyActive, trayTunActive, coreModel, settingsModel,
         startupRuntimeHandled, toast, control] {
            const auto noCleanup = [] {};
            const SettingsView settings = settingsModel->view.Get();
            if (!settings.ready || startupRuntimeHandled.Get() ||
                coreModel->serviceChecking.Get() || coreModel->operations.Busy() || cli::runtimeCommandMode()) return noCleanup;
            startupRuntimeHandled = true;
            coreModel->serviceChecking = true;
            const bool startupAutoRun = settings.autoRun;
            tasks.Launch([=]() -> huxerui::Task<void> {
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
                            core.stopCore();
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

    // Rendering projections share the same command target and authoritative result.
    huxerui::Lifecycle(
        [trayCoreRunning, trayCoreMenuRunning, traySysProxy, traySysProxyActive,
         trayTun, trayTunActive, coreModel, control] {
            const CoreView view = coreModel->view.Get();
            const bool running = view.core.state == core::CoreState::Running;
            trayCoreRunning = running;
            trayCoreMenuRunning = control.Running();
            traySysProxy = control.SystemProxyEnabled();
            traySysProxyActive = running && view.systemProxyActive;
            trayTun = control.TunEnabled();
            trayTunActive = view.TunActive();
            return [] {};
        },
        coreModel->view, coreModel->operations.command);


    // 托盘菜单需要的是当前可选订阅和运行中的策略组快照。数据库/API 读取
    // 全部放到任务线程，菜单本身只消费最近一次轻量快照。
    // 托盘菜单的运行态/列表镜像：都以模型 State 为依赖，模型一变就同步一次，
    // 不再有 1s 轮询，也不再自己读存储层（第三份订阅拷贝就此消失）。
    huxerui::Lifecycle(
        [trayProfiles, profilesModel] {
            trayProfiles = profilesModel->list.Get();
            return [] {};
        },
        profilesModel->list);
    huxerui::Lifecycle(
        [trayProxyGroups, proxiesModel] {
            trayProxyGroups = proxiesModel->snapshot.Get().groups;
            return [] {};
        },
        proxiesModel->snapshot);

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
            application.Quit();
        });
        // 退出保底看门狗：避免任何底层阻塞（网络断开超时、平台事件循环等）导致后台残留僵尸进程
        std::thread([] {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            std::_Exit(0);
        }).detach();
    };

    std::call_once(g_trayActivationOnce, [tray, window] {
        tray.OnActivate([window] {
            window.Show();
            window.Activate();
        });
    });

    if (trayAvailable) {
        const huxerui::Locale trayLocale = huxerui::UseEnvironment<huxerui::Locale>();
        const auto resolveLabel = [](huxerui::StringVariant label) {
            return huxerui::UseString(label);
        };
        const TrayMenuText menuText{
            .showWindow = resolveLabel(Localized("显示主窗口")),
            .noProfiles = resolveLabel(Localized("暂无可用订阅")),
            .selectProfile = resolveLabel(Localized("选择订阅")),
            .selectLine = resolveLabel(Localized("切换当前订阅线路")),
            .startCore = resolveLabel(Localized("启动")),
            .stopCore = resolveLabel(Localized("停止内核")),
            .systemProxy = resolveLabel(Localized("系统代理")),
            .tun = resolveLabel(Localized("TUN 模式")),
            .quit = resolveLabel(Localized("退出")),
        };
        const auto proxyLineMenu = BuildProxyLineMenu(
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
                        toast.Show(error.empty()
                                       ? Localized("线路切换失败")
                                       : huxerui::StringVariant(error));
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
            }, resolveLabel);
        huxerui::Lifecycle(
            [tray, window, application, tasks, trayCoreRunning,
             trayCoreMenuRunning, traySysProxy, trayTun, traySysProxyActive,
             trayTunActive, control, dialog, clipboard, toast,
             trayProfiles, trayProxyGroups, settingsModel, proxiesModel,
             coreModel, profilesModel,
             finishExit, menuText, proxyLineMenu,
             startupTrayEnabled,
             textColor = rootSpec.colors.on_surface,
             hintColor = rootSpec.colors.on_surface_variant] {
                const SettingsView settings = settingsModel->view.Get();
                const bool trayEnabled = settings.ready
                    ? settings.trayEnabled : startupTrayEnabled;
                if (trayEnabled) {
                    std::vector<huxerui::MenuEntry> menuEntries;
                    menuEntries.push_back(
                        huxerui::MenuItem(menuText.showWindow, [window] {
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
                                [tasks, toast, trayProfiles, trayProxyGroups,
                                 proxiesModel, profilesModel, id = profile.id] {
                                    tasks.Launch(
                                        [tasks, toast, trayProfiles,
                                         trayProxyGroups, proxiesModel,
                                         profilesModel, id]()
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
                                            // 切换订阅会改选中态与策略组，两个模型都
                                            // 立刻补一拍（镜像会跟着更新）。
                                            profilesModel->RequestSync();
                                            if (error.empty()) {
                                                // 切换订阅会换掉整组策略：请共享模型
                                                // 立即补一拍，托盘与各页面同一份数据。
                                                proxiesModel->RequestRefresh();
                                            }
                                        });
                                })
                                .Checked(profile.selected));
                    }
                    if (profileEntries.empty()) {
                        profileEntries.push_back(
                            huxerui::MenuItem(menuText.noProfiles, [] {}).Enabled(false));
                    }
                    menuEntries.push_back(huxerui::MenuItem(
                        menuText.selectProfile, std::move(profileEntries)));
                    menuEntries.push_back(huxerui::MenuItem(
                        menuText.selectLine, proxyLineMenu));
                    menuEntries.push_back(
                        huxerui::MenuItem(control.Running() ? menuText.stopCore : menuText.startCore,
                            [control] {
                                control.Run({.action = control.Running() ? CoreAction::Stop : CoreAction::Start});
                            })
                            .Checked(control.Running()).Enabled(!control.Busy()));
                    menuEntries.push_back(
                        huxerui::MenuItem(menuText.systemProxy, [control] {
                            control.Run({.action = CoreAction::SystemProxy,
                                         .enabled = !control.SystemProxyEnabled()});
                        }).Checked(control.SystemProxyEnabled()).Enabled(!control.Busy()));
                    menuEntries.push_back(
                        huxerui::MenuItem(menuText.tun, [control, window] {
                            control.Run({.action = CoreAction::Tun, .enabled = !control.TunEnabled()});
                        }).Checked(control.TunEnabled()).Enabled(!control.Busy()));
                    menuEntries.push_back(huxerui::MenuSection{});
                    menuEntries.push_back(
                        huxerui::MenuItem(menuText.quit, [finishExit] { finishExit(); }));
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
                } else {
                    // 关掉托盘图标后不能只停在「不再 Show」：托盘项是上一次
                    // Show 注册的，必须显式撤掉，否则图标会一直留在托盘里。
                    tray.Hide();
                }
                return [tray] { tray.Hide(); };
            },
            // 依赖必须覆盖菜单真正读到的每一份状态：订阅列表与线路快照是
            // **镜像 State**（由上面两个 Lifecycle 在模型变化时写入），漏掉它们
            // 会出现「菜单在镜像填充之前就建好、之后再也不会重建」——表现为
            // 缩到托盘后菜单里只剩「暂无可用订阅 / 暂无可切换线路」，而窗口可见时
            // 因为内核/接管状态变化顺带重建才看起来正常。
            // 有效 Locale 也必须作为依赖：语言切换及系统语言变化都刷新托盘文案。
            trayCoreRunning, trayCoreMenuRunning, traySysProxy, trayTun,
            traySysProxyActive, trayTunActive, coreModel->operations.command,
            coreModel->view, coreModel->serviceChecking, trayProfiles,
            trayProxyGroups, settingsModel->view, trayLocale);

    }

    // 关闭按钮只由托盘开关控制。托盘宿主尚未就绪时仍需确认退出，
    // 避免隐藏后没有入口唤回窗口；事件中动态查询宿主可用性。
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
        [window, tray, startupVisibilityHandled,
         startupTrayEnabled, startupMinimized] {
            const bool commandMode = cli::runtimeCommandMode();
            if (!startupVisibilityHandled.Get()) {
                startupVisibilityHandled = true;
                if (commandMode ||
                    (tray.IsAvailable() && startupTrayEnabled && startupMinimized)) {
                    window.Hide();
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
