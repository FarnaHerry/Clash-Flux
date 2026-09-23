// platform_app.cpp — 平台专属应用壳层与订阅刷新泵。
//
// AppRoot 只负责组装通用页面；窗口/托盘生命周期、Android 数据目录和
// 平台网络刷新通道在这里按平台函数整体实现，再由调用点宏选择。
#include <huxerui/huxerui.h>

#include <chrono>
#include <cstdlib>
#include <format>
#include <string>
#include <thread>
#include <vector>

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
    auto http = huxerui::UseService<huxerui::HttpClient>();
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

[[huxerui::composable]] huxerui::View DesktopApplicationEffects(
    const huxerui::ApplicationHandle& application,
    const huxerui::ThemeSpec& rootSpec) {
    const huxerui::WindowHandle window = huxerui::UseWindow();
    const huxerui::SystemTrayHandle tray = application.SystemTray();
    const bool trayAvailable = tray.IsAvailable();
    auto tasks = huxerui::UseTaskScope();
    auto trayCoreRunning = huxerui::UseState(false);
    auto traySysProxy = huxerui::UseState(false);
    auto trayTun = huxerui::UseState(false);
    auto trayProfiles = huxerui::UseState<std::vector<db::Profile>>({});
    auto trayProxyGroups = huxerui::UseState<std::vector<ProxyGroupSnapshot>>({});
    auto trayEnabled = huxerui::UseState(
        store::coreStore().setting("tray.enabled", "true") == "true");
    auto closeDialogOpen = huxerui::UseState(false);
    auto exitRequested = huxerui::UseState(false);
    auto dialog = huxerui::UseDialog();
    auto clipboard = application.Clipboard();
    auto toast = huxerui::UseToast();

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
        [tasks, trayCoreRunning, traySysProxy, trayTun, trayEnabled] {
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
                co_await PollWhile(std::chrono::duration<double>{0.5}, [=] {
                    auto& core = store::coreStore();
                    core.checkAlive();
                    trayCoreRunning =
                        core.snapshot().state == core::CoreState::Running;
                    traySysProxy = core.systemProxyEnabled();
                    trayTun = core.snapshot().tunEnabled;
                    trayEnabled = core.setting("tray.enabled", "true") == "true";
                    return true;
                });
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
            [tray, window, application, tasks, trayCoreRunning, traySysProxy,
             trayTun, dialog, clipboard, toast, trayProfiles, trayProxyGroups,
             trayEnabled, finishExit,
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
                            trayCoreRunning.Get() ? "停止内核" : "启动内核",
                            [tasks, toast, trayCoreRunning] {
                                tasks.Launch([=]() -> huxerui::Task<void> {
                                    const bool next = !trayCoreRunning.Get();
                                    const bool ok = co_await RunOnTaskThread([next] {
                                        auto& core = store::coreStore();
                                        if (!next) return core.stopCore();
                                        const bool resumeSysProxy =
                                            core.systemProxyEnabled();
                                        const bool resumeTun =
                                            core.setting("core.tun_enabled",
                                                         "false") == "true";
                                        core.startCore(
                                            store::profilesStore().selectedYaml(),
                                            false, resumeTun, resumeSysProxy);
                                        return core.snapshot().state ==
                                               core::CoreState::Running;
                                    });
                                    if (ok) {
                                        trayCoreRunning = next;
                                        co_return;
                                    }
                                    const std::string error =
                                        store::coreStore().snapshot().lastError;
                                    toast.Show(error.empty()
                                                   ? (next ? "启动内核失败"
                                                           : "停止内核失败")
                                                   : error);
                                });
                            }));
                    menuEntries.push_back(
                        huxerui::MenuItem("系统代理", [tasks, traySysProxy] {
                            tasks.Launch([=]() -> huxerui::Task<void> {
                                const bool next = !traySysProxy.Get();
                                const bool ok = co_await RunOnTaskThread([next] {
                                    return store::coreStore().applySystemProxy(next);
                                });
                                if (ok) traySysProxy = next;
                            });
                        }).Checked(traySysProxy.Get()));
                    menuEntries.push_back(
                        huxerui::MenuItem(
                            "TUN 模式",
                            [tasks, trayTun, window, dialog, clipboard, toast,
                             textColor, hintColor] {
                                tasks.Launch([=]() -> huxerui::Task<void> {
                                    const bool next = !trayTun.Get();
                                    if (next) {
                                        co_await huxerui::Delay(
                                            std::chrono::duration<double>{0});
                                        const core::TunGate gate =
                                            co_await RunOnTaskThread(
                                                [] { return core::tunGate(); });
                                        if (gate == core::TunGate::Elevated) co_return;
                                        if (gate == core::TunGate::Denied) {
                                            window.Activate();
                                            ShowTunGuideDialog(
                                                dialog, clipboard, toast, textColor,
                                                hintColor);
                                            co_return;
                                        }
                                    }
                                    const bool ok = co_await RunOnTaskThread([next] {
                                        return store::coreStore().applyTun(next);
                                    });
                                    if (ok) trayTun = next;
                                });
                            })
                            .Checked(trayTun.Get()));
                    menuEntries.push_back(huxerui::MenuSection{});
                    menuEntries.push_back(
                        huxerui::MenuItem("退出", [finishExit] { finishExit(); }));
                    tray.Show(app::images::tray,
                              huxerui::SystemTrayOptions{
                                  .tooltip = "Clash-Flux",
                                  .menu = std::move(menuEntries)});
                }
                return [tray] { tray.Hide(); };
            },
            trayCoreRunning, traySysProxy, trayTun, trayEnabled);
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
                huxerui::Image(app::images::mascot_logo)
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
