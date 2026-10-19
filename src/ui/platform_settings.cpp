// platform_settings.cpp — 平台专属设置区。
//
// 每个函数只负责一个平台形态：状态、任务、权限提示和控件一起维护，
// 通用 SettingsPage 不再知道 Android/桌面能力矩阵，也不再用控件级过滤器。
#include <huxerui/huxerui.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ui.h"
#include "app_resources.h"
#include "task_bridge.h"

import clashflux.config;
import clashflux.core;
import clashflux.service;
import clashflux.stream;
import clashflux.store.core;
import clashflux.store.profiles;
import clashflux.store.vpn;

#include "core_model.h"
#include "core_control.h"
#include "settings_model.h"

namespace clashflux::ui {

namespace {

#if !defined(__ANDROID__)

struct EnvironmentShell {
    const char* id;
    const char* label;
};

#if defined(_WIN32)
constexpr EnvironmentShell kEnvironmentShells[] = {
    {"powershell", "PowerShell"},
    {"cmd", "命令提示符（cmd）"},
};
#else
constexpr EnvironmentShell kEnvironmentShells[] = {
    {"bash", "Bash"},
    {"zsh", "Zsh"},
    {"fish", "Fish"},
    {"sh", "POSIX sh"},
};
#endif

std::string ShellBasename(const char* value) {
    if (value == nullptr || *value == '\0') return {};
    std::string path = value;
    const std::size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos) path.erase(0, slash + 1);
    if (path.size() > 4 && path.ends_with(".exe")) path.resize(path.size() - 4);
    for (char& c : path) {
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    }
    return path;
}

std::string CurrentEnvironmentShell() {
#if defined(_WIN32)
    const std::string shell = ShellBasename(std::getenv("COMSPEC"));
    if (shell == "cmd") return "cmd";
    return "powershell";
#else
    const std::string shell = ShellBasename(std::getenv("SHELL"));
    for (const auto& option : kEnvironmentShells) {
        if (shell == option.id) return option.id;
    }
    return "sh";
#endif
}

std::size_t EnvironmentShellIndex(const std::string& id) {
    for (std::size_t index = 0; index < std::size(kEnvironmentShells); ++index) {
        if (id == kEnvironmentShells[index].id) return index;
    }
    return 0;
}

std::vector<std::string> EnvironmentShellLabels() {
    std::vector<std::string> labels;
    for (const auto& option : kEnvironmentShells) {
        labels.emplace_back(option.label);
    }
    return labels;
}

std::string ProxyEnvironmentCommand(const std::string& shell, int port) {
    const int safePort = port > 0 && port <= 65535 ? port : 7899;
    const std::string endpoint = std::format("http://127.0.0.1:{}", safePort);
#if defined(_WIN32)
    if (shell == "cmd") {
        return std::format(
            "set \"HTTP_PROXY={}\"\n"
            "set \"HTTPS_PROXY={}\"\n"
            "set \"ALL_PROXY={}\"\n"
            "set \"NO_PROXY=localhost,127.0.0.1\"",
            endpoint, endpoint, endpoint);
    }
    return std::format(
        "$env:HTTP_PROXY = '{}'\n"
        "$env:HTTPS_PROXY = '{}'\n"
        "$env:ALL_PROXY = '{}'\n"
        "$env:NO_PROXY = 'localhost,127.0.0.1'",
        endpoint, endpoint, endpoint);
#else
    if (shell == "fish") {
        return std::format(
            "set -gx HTTP_PROXY {}\n"
            "set -gx HTTPS_PROXY {}\n"
            "set -gx ALL_PROXY {}\n"
            "set -gx NO_PROXY localhost,127.0.0.1",
            endpoint, endpoint, endpoint);
    }
    return std::format(
        "export HTTP_PROXY={}\n"
        "export HTTPS_PROXY={}\n"
        "export ALL_PROXY={}\n"
        "export NO_PROXY=localhost,127.0.0.1\n"
        "export http_proxy=\"$HTTP_PROXY\"\n"
        "export https_proxy=\"$HTTPS_PROXY\"\n"
        "export all_proxy=\"$ALL_PROXY\"\n"
        "export no_proxy=\"$NO_PROXY\"",
        endpoint, endpoint, endpoint);
#endif
}

#endif

} // namespace

#if defined(__linux__)

[[huxerui::composable]] huxerui::View DesktopServiceRow(
    bool service_installed, huxerui::TaskScope tasks,
    huxerui::ToastHandle toast) {
    const auto model = huxerui::UseService<CoreModel>();
    const auto serviceTasks = huxerui::UseApplicationTaskScope();
    return SettingRow(
        Localized("内核服务"),
        service_installed
            ? Localized("已安装（sing-box、PPTP 与 TUN 由 root 服务托管）")
            : Localized("安装 root 服务后，TUN/PPTP 无需每次授权（经 pkexec 一次性提权）"),
        huxerui::Button(Localized(service_installed ? "卸载服务" : "安装服务"))
            .With(huxerui::Enabled(!model->serviceChecking.Get() && !model->operations.Busy()))
            .OnClick([service_installed, tasks = serviceTasks, toast, model] {
                if (model->serviceChecking.Get() || model->operations.Busy()) return;
                model->serviceChecking = true;
                const bool installed = service_installed;
                tasks.Launch([service_installed, tasks, toast, model,
                              installed]() -> huxerui::Task<void> {
                    try {
                        const auto result = co_await RunOnTaskThread([installed] {
                            const std::string executable =
                                std::filesystem::read_symlink("/proc/self/exe")
                                    .string();
                            const int code = std::system(
                                std::format("pkexec \"{}\" service {}", executable,
                                            installed ? "uninstall" : "install")
                                    .c_str());
                            return std::pair{code, service::installed()};
                        });
                        model->Update([installed = result.second](CoreView& view) {
                            view.serviceInstalled = installed;
                        });
                        model->RequestRefresh();
                        model->serviceChecking = false;
                        if (result.first != 0) {
                            toast.Show(Localized(
                                installed ? "卸载被取消或失败"
                                          : "安装被取消或失败（需要授权）"));
                            co_return;
                        }
                        toast.Show(Localized(installed ? "服务已卸载" : "服务已安装"));
                    } catch (const std::exception& error) {
                        model->serviceChecking = false;
                        toast.Show(error.what());
                    } catch (...) {
                        model->serviceChecking = false;
                        toast.Show(Localized("服务操作失败"));
                    }
                });
            }), app::images::gear);
}

#elif defined(_WIN32)

[[huxerui::composable]] huxerui::View DesktopServiceRow(
    bool service_installed, huxerui::TaskScope tasks,
    huxerui::ToastHandle toast) {
    auto busy = huxerui::UseState(false);
    const auto model = huxerui::UseService<CoreModel>();
    const auto serviceTasks = huxerui::UseApplicationTaskScope();
    const auto manage = [service_installed, tasks = serviceTasks, toast, busy, model](bool remove) {
        if (busy.Get() || model->serviceChecking.Get() || model->operations.Busy()) return;
        busy = true;
        model->serviceChecking = true;
        tasks.Launch([=]() -> huxerui::Task<void> {
            std::string error;
            bool ok = false;
            try {
                const auto result = co_await RunOnTaskThread([remove] {
                    std::string failure;
                    const bool success = service::manageElevated(remove, failure);
                    // UAC cancellation leaves the current data plane untouched.
                    // Once SCM changes succeed, retire the client's old session
                    // state and per-user proxy settings on the task thread.
                    if (success) {
                        store::vpnStore().shutdown();
                        store::coreStore().stopCore();
                    }
                    return std::tuple{success, std::move(failure), service::installed()};
                });
                ok = std::get<0>(result);
                error = std::get<1>(result);
                const bool installed = std::get<2>(result);
                model->Update([installed](CoreView& view) { view.serviceInstalled = installed; });
            } catch (const std::exception& exception) { error = exception.what(); }
              catch (...) { error = "服务操作失败"; }
            busy = false;
            model->serviceChecking = false;
            model->RequestRefresh();
            if (ok) toast.Show(Localized(remove ? "服务已卸载" : "服务已安装"));
            else toast.Show(error.empty() ? Localized("服务操作失败") : huxerui::StringVariant(error));
        });
    };
    return SettingRow(
        Localized("内核服务"),
        Localized("Windows 系统服务托管内核、TUN 与 PPTP；启动后自动检查更新，手动安装或更新需要管理员授权"),
        huxerui::Row {
            huxerui::Button(Localized(service_installed ? "更新服务" : "安装服务"))
                .OnClick([manage] { manage(false); }).With(huxerui::Enabled(!busy.Get() && !model->serviceChecking.Get() && !model->operations.Busy())),
            service_installed
                ? huxerui::View{huxerui::Button(Localized("卸载服务"))
                    .OnClick([manage] { manage(true); }).With(huxerui::Enabled(!busy.Get() && !model->serviceChecking.Get() && !model->operations.Busy()))}
                : huxerui::View{},
        }.With(huxerui::Spacing(8.0F)), app::images::gear);
}

#else

[[huxerui::composable]] huxerui::View DesktopServiceRow(
    bool, huxerui::TaskScope, huxerui::ToastHandle) {
    return {};
}

#endif

// Platform choice stays outside composable bodies.
#define CLASHFLUX_DESKTOP_SERVICE_ROW DesktopServiceRow

#if defined(__ANDROID__)

[[huxerui::composable]] huxerui::View AndroidGeneralSettings() {
    return {};
}

[[huxerui::composable]] huxerui::View DesktopGeneralSettings() {
    return {};
}

[[huxerui::composable]] huxerui::View AndroidNetworkSettings() { return {}; }

[[huxerui::composable]] huxerui::View DesktopNetworkSettings() { return {}; }

[[huxerui::composable]] huxerui::View AndroidKernelSettings() {
    const huxerui::ApplicationHandle application = huxerui::UseApplication();
    auto toast = huxerui::UseToast();
    const auto control = UseCoreControl();
    const auto model = control.model;
    const CoreView view = model->view.Get();
    application.OnLifecycleChanged([model](huxerui::ApplicationLifecycleState state) {
        if (state == huxerui::ApplicationLifecycleState::Active) model->RequestRefresh();
    });
    return huxerui::Column {
        SectionTitle(Localized("后台运行")),
        SettingSwitchRow(
            Localized("后台保活"),
            Localized("申请忽略电池优化，防止后台被杀；建议同时在系统设置中允许本应用自启动"),
            huxerui::Switch(view.ignoringBattery)
                .OnChanged([model, toast](bool on) {
                    if (on) {
                        AndroidRequestBackgroundKeepAlive();
                    } else {
                        // Android 不允许普通应用静默撤销自身的电池优化豁免，
                        // 关闭动作必须进入系统管理页完成。
                        AndroidOpenBatterySettings();
                        toast.Show(Localized("请在系统电池设置中关闭本应用的电池优化豁免"));
                    }
                    model->RequestRefresh();
                }), false, app::images::battery),
    }.With(huxerui::Spacing(10.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

#else

[[huxerui::composable]] huxerui::View AndroidNetworkSettings() { return {}; }

[[huxerui::composable]] huxerui::View AndroidGeneralSettings() {
    return {};
}

[[huxerui::composable]] huxerui::View DesktopGeneralSettings() {
    // 设置项一律从 SettingsModel 读（见 settings_model.h）：组合期直接
    // setting(...) 会读到 hydrate 之前的默认值，且之后无人再同步——「关闭窗口时」
    // 曾经因此一直高亮"每次询问"。
    //
    // 写入走「写透」：setSetting 同步写内存缓存，紧接着把同一个值 Update 进模型，
    // 于是 UI 真值与缓存真值同一时刻成立——KV 不会失败，既不需要 pending/乐观
    // 回落，也不需要 RequestSync 去等下一个泵节拍（那个 tick 唤醒不了正在 Delay
    // 的泵，会让开关看起来"点了不动"）。
    const auto settingsModel = huxerui::UseService<SettingsModel>();
    const SettingsView settings = settingsModel->view.Get();

    return huxerui::Column {
        SettingSwitchRow(
            Localized("开机自启动"), Localized("登录系统后自动启动 Clash-Flux（桌面端）"),
            huxerui::Switch(settings.autoStart)
                .OnChanged([settingsModel](bool on) {
                    store::coreStore().setSetting(
                        "app.autostart", on ? "true" : "false");
                    settingsModel->Update(
                        [on](SettingsView& view) { view.autoStart = on; });
                }), false, app::images::power),
        // 内核启停与流量接管解耦后的「启动入口」之一：默认不随应用启动内核
        // （内核只是本地端口 + 控制接口），需要时用首页右下角悬浮按钮启动。
        SettingSwitchRow(
            Localized("启动时自动运行内核"), Localized("打开应用就拉起 sing-box，并按已记录的系统代理/TUN 恢复接管"),
            huxerui::Switch(settings.autoRun)
                .OnChanged([settingsModel](bool on) {
                    store::coreStore().setSetting("app.auto_run",
                                                  on ? "true" : "false");
                    settingsModel->Update(
                        [on](SettingsView& view) { view.autoRun = on; });
                }), false, app::images::play),
        SettingSwitchRow(
            Localized("启用托盘"), Localized("启用后关闭窗口隐藏到托盘；停用后关闭窗口询问是否退出"),
            huxerui::Switch(settings.trayEnabled)
                .OnChanged([settingsModel](bool on) {
                    store::coreStore().setSetting("tray.enabled",
                                                  on ? "true" : "false");
                    settingsModel->Update(
                        [on](SettingsView& view) { view.trayEnabled = on; });
                }), false, app::images::tray),
        SettingSwitchRow(
            Localized("启动时隐藏到托盘"), Localized("下次启动不显示主窗口，经托盘唤出"),
            huxerui::Switch(settings.startMinimized)
                .OnChanged([settingsModel](bool on) {
                    store::coreStore().setSetting(
                        "tray.start_minimized", on ? "true" : "false");
                    settingsModel->Update(
                        [on](SettingsView& view) { view.startMinimized = on; });
                }), false, app::images::visibility_off),
    }.With(huxerui::Spacing(10.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View DesktopNetworkSettings() {
    const auto application = huxerui::UseApplication();
    const auto clipboard = application.Clipboard();
    const auto toast = huxerui::UseToast();
    const auto coreModel = huxerui::UseService<CoreModel>();
    const auto settingsModel = huxerui::UseService<SettingsModel>();
    const SettingsView settings = settingsModel->view.Get();
    const std::string detectedShell = CurrentEnvironmentShell();
    const std::size_t envShell = EnvironmentShellIndex(
        settings.envShell.empty() ? detectedShell : settings.envShell);
    const std::vector<std::string> envShellLabels = EnvironmentShellLabels();
    return huxerui::Column {
        SettingRow(
            Localized("复制环境变量"),
            LocalizedFormat("当前检测到 {}；复制当前混合端口的代理变量",
                            detectedShell),
            huxerui::Row {
                huxerui::Select(
                    envShellLabels,
                    envShell,
                    [](const std::string& name) {
                        return huxerui::Text(name == "命令提示符（cmd）"
                                                 ? Localized("命令提示符（cmd）")
                                                 : huxerui::StringVariant(name));
                    })
                    .OnChanged([settingsModel](std::size_t index) {
                        if (index < std::size(kEnvironmentShells))
                            settingsModel->SetEnvShell(kEnvironmentShells[index].id);
                    })
                    .With(huxerui::Grow(1.0F), huxerui::Frame{.max_width = 170.0F},
                          huxerui::Enabled(settings.ready)),
                huxerui::Button(Localized("复制")).With(huxerui::Enabled(settings.ready)).OnClick(
                    [clipboard, toast, settingsModel, detectedShell, coreModel] {
                        const SettingsView current = settingsModel->view.Get();
                        if (!current.ready) return;
                        const CoreView runtime = coreModel->view.Get();
                        const int port = runtime.core.state == core::CoreState::Running
                            ? runtime.core.mixedPort : std::atoi(current.mixedPort.c_str());
                        const std::string command = ProxyEnvironmentCommand(
                            current.envShell.empty() ? detectedShell : current.envShell, port);
                        if (clipboard->WriteText(command)) {
                            toast.Show(Localized("环境变量命令已复制"));
                        } else {
                            toast.Show(Localized("复制失败"));
                        }
                    }),
            }.With(huxerui::Spacing(8.0F)), app::images::terminal),
    }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View DesktopKernelSettings() {
    const auto control = UseCoreControl();
    const auto view = control.model->view.Get();
    const auto tasks = huxerui::UseApplicationTaskScope();
    const auto toast = huxerui::UseToast();
    const bool running = view.core.state == core::CoreState::Running;
    const auto version = view.core.version.empty() ? std::string("sing-box") : view.core.version;
    return huxerui::Column {
        SectionTitle(Localized("运行与维护")),
        SettingRow(Localized("内核"), version,
            huxerui::Button(Localized("重启"))
                .With(huxerui::Enabled(running && !control.Busy()))
                .OnClick([control] { control.Run({.action = CoreAction::Restart}); }),
            app::images::bolt),
        CLASHFLUX_DESKTOP_SERVICE_ROW(view.serviceInstalled, tasks, toast),
    }.With(huxerui::Spacing(10.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

#endif

#undef CLASHFLUX_DESKTOP_SERVICE_ROW

} // namespace clashflux::ui
