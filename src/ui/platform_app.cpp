// platform_app.cpp — 平台专属应用壳层与订阅刷新泵。
//
// AppRoot 只负责组装通用页面；窗口/托盘生命周期、Android 数据目录和
// 平台网络刷新通道在这里按平台函数整体实现，再由调用点宏选择。
#include <huxerui/huxerui.h>

#include <chrono>
#include <format>
#include <string>
#include <vector>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

#include "ui.h"
#include "app_resources.h"
#include "task_bridge.h"

import clashflux.config;
import clashflux.core;
import clashflux.service;
import clashflux.store.core;
import clashflux.store.profiles;
import clashflux.store.vpn;

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
    auto traySysProxy = huxerui::UseState(false);
    auto trayTun = huxerui::UseState(false);
    auto trayEnabled = huxerui::UseState(
        store::coreStore().setting("tray.enabled", "true") == "true");
    auto closeDialogOpen = huxerui::UseState(false);
    auto exitRequested = huxerui::UseState(false);
    auto dialog = huxerui::UseDialog();
    auto clipboard = application.Clipboard();
    auto toast = huxerui::UseToast();

    // 内核自启 + 崩溃检测泵：启动和存活检查全部在任务线程执行。
    huxerui::Lifecycle(
        [tasks, traySysProxy, trayTun, trayEnabled] {
            tasks.Launch([=]() -> huxerui::Task<void> {
                co_await RunOnTaskThread([] {
                    auto& core = store::coreStore();
                    core.init();
                    if (!cfg::singboxBinary().empty()) {
                        core.startCore(store::profilesStore().selectedYaml());
                    }
                });
                co_await PollWhile(std::chrono::duration<double>{0.5}, [=] {
                    auto& core = store::coreStore();
                    core.checkAlive();
                    traySysProxy = core.systemProxyEnabled();
                    trayTun = core.snapshot().tunEnabled;
                    trayEnabled = core.setting("tray.enabled", "true") == "true";
                    return true;
                });
            });
            return [] {};
        },
        0);

    // 系统 VPN 的断开可能要等待 pppd/RAS 收尾，先完成清理再关闭窗口。
    auto finishExit = [tasks, application, exitRequested]() {
        if (exitRequested.Get()) return;
        exitRequested = true;
        tasks.Launch([application]() -> huxerui::Task<void> {
            co_await RunOnTaskThread([] {
                store::vpnStore().shutdown();
                store::coreStore().stopCore();
            });
            application.Quit();
        });
    };

    if (trayAvailable) {
        tray.OnActivate([window] { window.Activate(); });
        huxerui::Lifecycle(
            [tray, window, application, tasks, traySysProxy, trayTun, dialog,
             clipboard, toast, trayEnabled, finishExit,
             textColor = rootSpec.colors.on_surface,
             hintColor = rootSpec.colors.on_surface_variant] {
                if (trayEnabled.Get()) {
                    std::vector<huxerui::MenuEntry> menuEntries;
                    menuEntries.push_back(
                        huxerui::MenuItem("显示主窗口", [window] {
                            window.Activate();
                        }));
                    menuEntries.push_back(huxerui::MenuSection{});
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
            traySysProxy, trayTun, trayEnabled);
    }

    // 关闭窗口行为：托盘可用时按设置询问/退出/最小化到托盘。
    const huxerui::Color closeHintColor = rootSpec.colors.on_surface_variant;
    auto hideToTray = [window] { window.Hide(); };
    window.OnCloseRequest(
        [=]() mutable -> bool {
            if (exitRequested.Get()) return false;
            if (!trayAvailable || !trayEnabled.Get()) {
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
                hideToTray();
                return true;
            }
            if (closeDialogOpen.Get()) return true;
            closeDialogOpen = true;
            dialog.Show(
                [=](huxerui::DialogContext ctx) -> huxerui::View {
                    return DialogCard(huxerui::Column {
                        huxerui::Text("关闭 Clash-Flux？", huxerui::TextRole::Title),
                        huxerui::Text("直接退出将停止代理；最小化到托盘后代理继续在后台运行。")
                            .Style(huxerui::TextStyle{
                                huxerui::Font::System(font_size::kCaption),
                                closeHintColor}),
                        huxerui::Row {
                            huxerui::Button("直接关闭").OnClick([=] {
                                ctx.Dismiss();
                                closeDialogOpen = false;
                                finishExit();
                            }),
                            huxerui::Button("最小化到托盘").OnClick([=] {
                                ctx.Dismiss();
                                closeDialogOpen = false;
                                hideToTray();
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
                huxerui::DialogOptions{});
            return true;
        },
        0);

    if (trayAvailable && trayEnabled.Get() &&
        store::coreStore().setting("tray.start_minimized", "false") == "true") {
        window.Hide();
    }
    return {};
}

[[huxerui::composable]] huxerui::View DesktopAppContent(
    huxerui::View mainRow, const huxerui::ThemeSpec& rootSpec) {
    // 桌面标题栏和拖拽区只存在于桌面壳函数，Android 不会组合这些节点。
    huxerui::View content = mainRow;
    return huxerui::Column {
        huxerui::WindowTitleBar {
            huxerui::Text("Clash-Flux")
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kChip)
                        .WithWeight(huxerui::FontWeight::Bold),
                    rootSpec.colors.on_surface})
                .With(huxerui::WindowDragRegion{}),
            huxerui::Spacer{}.With(huxerui::Grow(1.0F),
                                   huxerui::WindowDragRegion{}),
        }
            .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(
                      rootSpec.spacing.small, 0.0F)),
                  huxerui::Spacing(rootSpec.spacing.small)),
        std::move(content),
    }
        .With(huxerui::Spacing(rootSpec.spacing.extra_small),
              huxerui::Background(rootSpec.colors.background),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

#endif

} // namespace clashflux::ui
