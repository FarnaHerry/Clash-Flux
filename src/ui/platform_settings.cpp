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
#include <vector>

#include "ui.h"
#include "task_bridge.h"

import clashflux.config;
import clashflux.core;
import clashflux.service;
import clashflux.stream;
import clashflux.store.core;
import clashflux.store.profiles;

namespace clashflux::ui {

namespace {

template <typename Job>
void LaunchSettingsAction(huxerui::TaskScope tasks, huxerui::ToastHandle toast,
                          huxerui::State<bool> busy, Job job,
                          std::string ok_message = {}) {
    if (busy.Get()) return;
    busy = true;
    tasks.Launch([tasks, toast, busy, job = std::move(job),
                  ok_message = std::move(ok_message)]() mutable
                     -> huxerui::Task<void> {
        try {
            co_await RunOnTaskThread(std::move(job));
            if (!ok_message.empty()) toast.Show(ok_message);
        } catch (const std::exception& error) {
            toast.Show(error.what());
        }
        busy = false;
    });
}

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
    for (const auto& option : kEnvironmentShells) labels.emplace_back(option.label);
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

[[huxerui::composable]] huxerui::View LinuxServiceRow(
    huxerui::State<bool> service_installed, huxerui::TaskScope tasks,
    huxerui::ToastHandle toast) {
    return SettingRow(
        "内核服务",
        service_installed.Get()
            ? "已安装（sing-box、PPTP 与 TUN 由 root 服务托管）"
            : "安装 root 服务后，TUN/PPTP 无需每次授权（经 pkexec 一次性提权）",
        huxerui::Button(service_installed.Get() ? "卸载服务" : "安装服务")
            .OnClick([service_installed, tasks, toast] {
                const bool installed = service_installed.Get();
                tasks.Launch([service_installed, tasks, toast,
                              installed]() -> huxerui::Task<void> {
                    try {
                        const int result = co_await RunOnTaskThread([installed] {
                            const std::string executable =
                                std::filesystem::read_symlink("/proc/self/exe")
                                    .string();
                            return std::system(
                                std::format("pkexec \"{}\" service {}", executable,
                                            installed ? "uninstall" : "install")
                                    .c_str());
                        });
                        if (result != 0) {
                            throw std::runtime_error(
                                installed ? "卸载被取消或失败"
                                           : "安装被取消或失败（需要授权）");
                        }
                        service_installed = !installed;
                        toast.Show(installed ? "服务已卸载" : "服务已安装");
                    } catch (const std::exception& error) {
                        toast.Show(error.what());
                    }
                });
            }));
}

#else

[[huxerui::composable]] huxerui::View LinuxServiceRow(
    huxerui::State<bool>, huxerui::TaskScope, huxerui::ToastHandle) {
    return {};
}

#endif

// 由宏只在桌面设置函数中选择 Linux 专属行；没有跨页面能力表。
#define CLASHFLUX_LINUX_SERVICE_ROW LinuxServiceRow

#if defined(__ANDROID__)

[[huxerui::composable]] huxerui::View DesktopSettingsSection() {
    return {};
}

[[huxerui::composable]] huxerui::View AndroidSettingsSection() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const huxerui::ApplicationHandle application = huxerui::UseApplication();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto snap = huxerui::UseState<store::CoreSnapshot>({});
    auto tun_enabled = huxerui::UseState(
        store::coreStore().setting("core.tun_enabled", "false") == "true");
    auto vpn_state = huxerui::UseState(AndroidVpnState());
    auto battery_ignored = huxerui::UseState(AndroidIsIgnoringBattery());
    auto busy = huxerui::UseState(false);

    // Android 的授权页会暂时遮住 Activity；回到前台时立即重读系统状态，
    // 不依赖用户再次点击控件或等待后台轮询恢复。
    application.OnLifecycleChange(
        [battery_ignored](huxerui::ApplicationLifecycleState state) {
            if (state == huxerui::ApplicationLifecycleState::Active) {
                battery_ignored = AndroidIsIgnoringBattery();
            }
        });

    huxerui::Lifecycle(
        [tasks, snap, tun_enabled, vpn_state, battery_ignored] {
            tasks.Launch([=]() -> huxerui::Task<void> {
                co_await PollWhile(std::chrono::duration<double>{1.0}, [=] {
                    snap = store::coreStore().snapshot();
                    tun_enabled = store::coreStore().setting(
                                      "core.tun_enabled", "false") == "true";
                    vpn_state = AndroidVpnState();
                    battery_ignored = AndroidIsIgnoringBattery();
                    return true;
                });
            });
            return [] {};
        },
        0);

    const int state = vpn_state.Get();
    const std::string status =
        state == 2 ? "已附着：sing-box 正通过 protect(fd) 使用物理网络"
        : state == 1 ? "正在建立系统 VPN 与 TUN 数据面"
        : state == 3 ? "启动失败：请查看日志页中的 Android VPN 错误"
                     : "未连接；开启后将请求系统 VPN 授权";

    const auto core_action = [tasks, toast, busy](std::function<void()> job,
                                                   std::string ok_message) {
        LaunchSettingsAction(tasks, toast, busy, std::move(job),
                             std::move(ok_message));
    };

    return Card(huxerui::Column {
        SectionTitle("Android 数据面"),
        SettingRow("隧道状态", status,
                   huxerui::Text(state == 2 ? "已连接"
                                 : state == 1 ? "连接中"
                                 : state == 3 ? "失败" : "未连接")),
        SettingRow(
            "VPN 代理",
            "系统 VPN 由此服务持有；内核出站 socket 会自动绕过 TUN",
            huxerui::Switch(tun_enabled.Get() || state == 1 || state == 2)
                .OnChanged([core_action, tun_enabled](bool on) {
                    tun_enabled = on;
                    stream::logApplication(
                        "info", on ? "用户请求开启 Android VPN"
                                   : "用户请求关闭 Android VPN");
                    core_action(
                        [on] {
                            store::coreStore().setSetting(
                                "core.tun_enabled", on ? "true" : "false");
                            if (on) {
                                store::coreStore().startCore(
                                    store::profilesStore().selectedYaml());
                                AndroidStartVpn();
                            } else {
                                AndroidStopVpn();
                            }
                        },
                        on ? "正在请求建立 VPN 隧道" : "VPN 隧道已关闭");
                })),
        SettingRow(
            "后台保活",
            "申请忽略电池优化，防止后台被杀；建议同时在系统设置中允许本应用自启动",
            huxerui::Switch(battery_ignored.Get())
                .OnChanged([battery_ignored, toast](bool on) {
                    if (on) {
                        battery_ignored = false;
                        AndroidRequestBackgroundKeepAlive();
                    } else {
                        // Android 不允许普通应用静默撤销自身的电池优化豁免，
                        // 关闭动作必须进入系统管理页完成。
                        AndroidOpenBatterySettings();
                        toast.Show("请在系统电池设置中关闭本应用的电池优化豁免");
                    }
                    battery_ignored = AndroidIsIgnoringBattery();
                })),
    }.With(huxerui::Spacing(10.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
}

#else

[[huxerui::composable]] huxerui::View AndroidSettingsSection() {
    return {};
}

[[huxerui::composable]] huxerui::View DesktopSettingsSection() {
    auto trayEnabled = huxerui::UseState(
        store::coreStore().setting("tray.enabled", "true") == "true");
    auto startMinimized = huxerui::UseState(
        store::coreStore().setting("tray.start_minimized", "false") == "true");
    std::size_t initialCloseBehavior = 0;
    const std::string savedCloseBehavior =
        store::coreStore().setting("tray.close_behavior", "0");
    if (savedCloseBehavior == "1") initialCloseBehavior = 1;
    if (savedCloseBehavior == "2") initialCloseBehavior = 2;
    auto closeBehavior = huxerui::UseState(initialCloseBehavior);
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const huxerui::ApplicationHandle application = huxerui::UseApplication();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto dialog = huxerui::UseDialog();
    auto clipboard = application.Clipboard();
    auto snap = huxerui::UseState<store::CoreSnapshot>({});
    auto service_installed = huxerui::UseState(service::installed());
    auto proxy_override = huxerui::UseState<std::optional<bool>>(std::nullopt);
    auto tun_override = huxerui::UseState<std::optional<bool>>(std::nullopt);
    const std::string detectedShell = CurrentEnvironmentShell();
    const std::string savedShell =
        store::coreStore().setting("ui.env_shell", detectedShell);
    auto envShell = huxerui::UseState(EnvironmentShellIndex(savedShell));
    const std::vector<std::string> envShellLabels =
        EnvironmentShellLabels();
    auto busy = huxerui::UseState(false);

    huxerui::Lifecycle(
        [tasks, snap, service_installed] {
            tasks.Launch([=]() -> huxerui::Task<void> {
                co_await PollWhile(std::chrono::duration<double>{1.0}, [=] {
                    snap = store::coreStore().snapshot();
                    service_installed = service::installed();
                    return true;
                });
            });
            return [] {};
        },
        0);

    const store::CoreSnapshot s = snap.Get();
    const bool running = s.state == core::CoreState::Running;
    const std::string state_text =
        s.binaryPath.empty()
            ? "未找到内核运行时"
            : std::format("{} · {}{}", core::stateName(s.state),
                          s.version.empty() ? "sing-box" : s.version,
                          s.lastError.empty() ? "" : " · " + s.lastError);

    const auto core_action = [tasks, toast, busy](std::function<void()> job,
                                                   std::string ok_message) {
        LaunchSettingsAction(tasks, toast, busy, std::move(job),
                             std::move(ok_message));
    };

    return huxerui::Column {
        Card(huxerui::Column {
            SectionTitle("内核"),
            huxerui::Text(state_text).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody),
                s.state == core::CoreState::Failed ? theme.colors.error
                                                    : theme.colors.on_surface}),
            huxerui::Row {
                running
                    ? huxerui::View{huxerui::Button("停止").OnClick(
                          [core_action] {
                              core_action(
                                  [] { store::coreStore().stopCore(); },
                                  "内核已停止");
                          })}
                    : huxerui::View{huxerui::Button("启动").OnClick(
                          [core_action] {
                              core_action(
                                  [] {
                                      store::coreStore().startCore(
                                          store::profilesStore().selectedYaml());
                                  },
                                  "内核已启动");
                          })},
                huxerui::Button("重启")
                    .OnClick([core_action] {
                        core_action(
                            [] {
                                store::coreStore().stopCore();
                                store::coreStore().startCore(
                                    store::profilesStore().selectedYaml());
                            },
                            "内核已重启");
                    })
                    .With(huxerui::Enabled(running)),
            }.With(huxerui::Spacing(8.0F)),
        }.With(huxerui::Spacing(8.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),

        Card(huxerui::Column {
            SectionTitle("系统"),
            CLASHFLUX_LINUX_SERVICE_ROW(service_installed, tasks, toast),
            SettingRow(
                "系统代理",
                std::format("写入桌面系统代理（127.0.0.1:{}）", s.mixedPort),
                huxerui::Switch(proxy_override.Get().value_or(
                                    store::coreStore().systemProxyEnabled()))
                    .OnChanged([tasks, toast, proxy_override](bool on) {
                        if (proxy_override.Get().has_value()) return;
                        proxy_override = on;
                        tasks.Launch([=]() -> huxerui::Task<void> {
                            const bool ok = co_await RunOnTaskThread(
                                [on] { return store::coreStore().applySystemProxy(on); });
                            proxy_override = std::nullopt;
                            if (!ok) {
                                const std::string error =
                                    store::coreStore().snapshot().lastError;
                                toast.Show(error.empty() ? "系统代理设置失败" : error);
                            } else {
                                toast.Show(on ? "系统代理已开启" : "系统代理已关闭");
                            }
                        });
                    })),
            SettingRow(
                "复制环境变量",
                std::format("当前检测到 {}；复制当前混合端口的代理变量",
                            detectedShell),
                huxerui::Row {
                    huxerui::Select(
                        envShellLabels,
                        envShell.Get(),
                        [](const std::string& name) { return huxerui::Text(name); })
                        .OnChanged([envShell](std::size_t index) {
                            envShell = index;
                            store::coreStore().setSetting(
                                "ui.env_shell", kEnvironmentShells[index].id);
                        })
                        .With(huxerui::Frame{.width = 170.0F}),
                    huxerui::Button("复制").OnClick(
                        [clipboard, toast, envShell, s] {
                            const std::string command = ProxyEnvironmentCommand(
                                kEnvironmentShells[envShell.Get()].id,
                                s.mixedPort);
                            if (clipboard->WriteText(command)) {
                                toast.Show("环境变量命令已复制");
                            } else {
                                toast.Show("复制失败");
                            }
                        }),
                }.With(huxerui::Spacing(8.0F))),
            SettingRow(
                "TUN 模式",
                running ? "全局透明代理（需 root/CAP_NET_ADMIN，立即生效）"
                        : "全局透明代理（下次启动生效）",
                huxerui::Switch(tun_override.Get().value_or(s.tunEnabled))
                    .OnChanged([s, tasks, toast, dialog, clipboard, proxy_override,
                               tun_override, textColor = theme.colors.on_surface,
                               hintColor = theme.colors.on_surface_variant](bool on) {
                        if (tun_override.Get().has_value()) return;
                        tun_override = on;
                        tasks.Launch([=]() -> huxerui::Task<void> {
                            if (on) {
                                co_await huxerui::Delay(
                                    std::chrono::duration<double>{0});
                                const core::TunGate gate = co_await RunOnTaskThread(
                                    [] { return core::tunGate(); });
                                if (gate == core::TunGate::Elevated) {
                                    tun_override = std::nullopt;
                                    toast.Show("已请求管理员权限重启，请在新窗口开启 TUN");
                                    co_return;
                                }
                                if (gate == core::TunGate::Denied) {
                                    tun_override = std::nullopt;
                                    ShowTunGuideDialog(dialog, clipboard, toast,
                                                       textColor, hintColor);
                                    co_return;
                                }
                            }
                            const bool ok = co_await RunOnTaskThread(
                                [on] { return store::coreStore().applyTun(on); });
                            tun_override = std::nullopt;
                            if (!ok) {
                                const std::string error =
                                    store::coreStore().snapshot().lastError;
                                toast.Show(error.empty() ? "TUN 切换失败" : error);
                            } else {
                                toast.Show(on ? "TUN 已开启" : "TUN 已关闭");
                            }
                        });
                    })),
        }.With(huxerui::Spacing(10.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),

        Card(huxerui::Column {
            SectionTitle("托盘"),
            SettingRow(
                "启用托盘图标", "关闭后托盘不可用，关闭窗口即退出",
                huxerui::Switch(trayEnabled.Get())
                    .OnChanged([trayEnabled](bool on) {
                        trayEnabled = on;
                        store::coreStore().setSetting("tray.enabled",
                                                       on ? "true" : "false");
                    })),
            SettingRow(
                "关闭窗口时",
                "托盘可用时的驻留行为（代理继续后台运行 = 最小化到托盘）",
                huxerui::SegmentedButton(
                    std::vector<huxerui::StringVariant>{"每次询问", "直接退出",
                                                        "最小化到托盘"},
                    closeBehavior.Get())
                    .OnChanged([closeBehavior](std::size_t index) {
                        closeBehavior = index;
                        store::coreStore().setSetting("tray.close_behavior",
                                                       std::to_string(index));
                    })),
            SettingRow(
                "启动时隐藏到托盘", "下次启动不显示主窗口，经托盘唤出",
                huxerui::Switch(startMinimized.Get())
                    .OnChanged([startMinimized](bool on) {
                        startMinimized = on;
                        store::coreStore().setSetting(
                            "tray.start_minimized", on ? "true" : "false");
                    })),
        }.With(huxerui::Spacing(10.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),
    }.With(huxerui::Spacing(12.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

#endif

#undef CLASHFLUX_LINUX_SERVICE_ROW

} // namespace clashflux::ui
