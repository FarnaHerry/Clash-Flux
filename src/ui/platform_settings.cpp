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
#include <type_traits>
#include <utility>
#include <vector>

#include "ui.h"
#include "task_bridge.h"

import clashflux.config;
import clashflux.core;
import clashflux.service;
import clashflux.stream;
import clashflux.store.core;
import clashflux.store.profiles;

#include "core_model.h"
#include "settings_model.h"

namespace clashflux::ui {

namespace {

template <typename Job>
void LaunchSettingsAction(huxerui::TaskScope tasks, huxerui::ToastHandle toast,
                          huxerui::State<bool> busy, Job job,
                          std::string ok_message = {},
                          std::function<void(bool)> finished = {}) {
    if (busy.Get()) return;
    busy = true;
    tasks.Launch([tasks, toast, busy, job = std::move(job),
                  ok_message = std::move(ok_message),
                  finished = std::move(finished)]() mutable
                     -> huxerui::Task<void> {
        bool ok = false;
        try {
            using Result = std::invoke_result_t<Job&>;
            if constexpr (std::is_same_v<Result, bool>) {
                ok = co_await RunOnTaskThread(std::move(job));
            } else {
                co_await RunOnTaskThread(std::move(job));
                ok = true;
            }
            if (ok && !ok_message.empty()) toast.Show(ok_message);
        } catch (const std::exception& error) {
            toast.Show(error.what());
        }
        if (finished) finished(ok);
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

// 由宏只在桌面内核模块中选择 Linux 专属行；没有跨页面能力表。
#define CLASHFLUX_LINUX_SERVICE_ROW LinuxServiceRow

#if defined(__ANDROID__)

[[huxerui::composable]] huxerui::View AndroidGeneralSettings() {
    return {};
}

[[huxerui::composable]] huxerui::View DesktopGeneralSettings() {
    return {};
}

[[huxerui::composable]] huxerui::View AndroidKernelSettings() {
    const huxerui::ApplicationHandle application = huxerui::UseApplication();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto tun_enabled = huxerui::UseState(
        store::coreStore().setting("core.tun_enabled", "false") == "true");
    auto vpn_state = huxerui::UseState(AndroidVpnState());
    auto vpn_enabled = huxerui::UseState(
        tun_enabled.Get() || vpn_state.Get() == 1 || vpn_state.Get() == 2);
    auto vpn_pending = huxerui::UseState(false);
    auto battery_ignored = huxerui::UseState(AndroidIsIgnoringBattery());
    auto busy = huxerui::UseState(false);

    // Android 的授权页会暂时遮住 Activity；回到前台时立即重读系统状态，
    // 不依赖用户再次点击控件或等待后台轮询恢复。
    application.OnLifecycleChanged(
        [battery_ignored](huxerui::ApplicationLifecycleState state) {
            if (state == huxerui::ApplicationLifecycleState::Active) {
                battery_ignored = AndroidIsIgnoringBattery();
            }
        });

    huxerui::Lifecycle(
        [tasks, tun_enabled, vpn_state, vpn_enabled, vpn_pending,
         battery_ignored] {
            tasks.Launch([=]() -> huxerui::Task<void> {
                co_await PollWhile(std::chrono::duration<double>{1.0}, [=] {
                    tun_enabled = store::coreStore().setting(
                                      "core.tun_enabled", "false") == "true";
                    vpn_state = AndroidVpnState();
                    battery_ignored = AndroidIsIgnoringBattery();
                    if (!vpn_pending.Get()) {
                        vpn_enabled = tun_enabled.Get() ||
                                      vpn_state.Get() == 1 ||
                                      vpn_state.Get() == 2;
                    }
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
    return huxerui::Column {
        SettingRow("隧道状态", status,
                   huxerui::Text(state == 2 ? "已连接"
                                 : state == 1 ? "连接中"
                                 : state == 3 ? "失败" : "未连接")),
        SettingSwitchRow(
            "VPN 代理",
            "系统 VPN 由此服务持有；内核出站 socket 会自动绕过 TUN",
            huxerui::Switch(vpn_enabled.Get())
                .OnChanged([tasks, toast, busy, tun_enabled, vpn_state,
                            vpn_enabled, vpn_pending](bool on) {
                    if (busy.Get() || vpn_pending.Get()) return;
                    const bool previous = vpn_enabled.Get();
                    vpn_enabled = on;
                    vpn_pending = true;
                    busy = true;
                    stream::logApplication(
                        "info", on ? "用户请求开启 Android VPN"
                                   : "用户请求关闭 Android VPN");
                    tasks.Launch([toast, busy, tun_enabled, vpn_state,
                                  vpn_enabled, vpn_pending, on,
                                  previous]() -> huxerui::Task<void> {
                        bool ok = false;
                        std::string error;
                        try {
                            co_await RunOnTaskThread([on] {
                                store::coreStore().setSetting(
                                    "core.tun_enabled", on ? "true" : "false");
                                if (on) {
                                    store::coreStore().startCore(
                                        store::profilesStore().selectedYaml(),
                                        false, true);
                                    AndroidStartVpn();
                                } else {
                                    AndroidStopVpn();
                                    WaitForAndroidVpnStopped();
                                    store::coreStore().startCore(
                                        store::profilesStore().selectedYaml(),
                                        false, false);
                                }
                            });

                            if (on) {
                                for (int attempt = 0; attempt < 300; ++attempt) {
                                    const auto [current, requested] =
                                        co_await RunOnTaskThread([] {
                                            return std::pair{
                                                AndroidVpnState(),
                                                store::coreStore().setting(
                                                    "core.tun_enabled", "false") == "true"};
                                        });
                                    if (current == 1 || current == 2) {
                                        ok = true;
                                        break;
                                    }
                                    if (current == 3 || !requested) break;
                                    co_await huxerui::Delay(
                                        std::chrono::duration<double>{0.2});
                                }
                            } else {
                                for (int attempt = 0; attempt < 300; ++attempt) {
                                    const auto [current, requested] =
                                        co_await RunOnTaskThread([] {
                                            return std::pair{
                                                AndroidVpnState(),
                                                store::coreStore().setting(
                                                    "core.tun_enabled", "false") == "true"};
                                        });
                                    if (current != 1 && current != 2 && !requested) {
                                        ok = true;
                                        break;
                                    }
                                    co_await huxerui::Delay(
                                        std::chrono::duration<double>{0.2});
                                }
                            }
                        } catch (const std::exception& exception) {
                            error = exception.what();
                        }

                        if (!ok) {
                            try {
                                co_await RunOnTaskThread([previous] {
                                    store::coreStore().setSetting(
                                        "core.tun_enabled",
                                        previous ? "true" : "false");
                                });
                            } catch (const std::exception& exception) {
                                if (error.empty()) error = exception.what();
                            }
                            if (error.empty()) error = store::coreStore().snapshot().lastError;
                            if (error.empty()) {
                                error = on ? "VPN 启动已取消或失败" : "VPN 隧道未能关闭";
                            }
                        }

                        try {
                            const auto [current, requested] =
                                co_await RunOnTaskThread([] {
                                    return std::pair{
                                        AndroidVpnState(),
                                        store::coreStore().setting(
                                            "core.tun_enabled", "false") == "true"};
                                });
                            vpn_state = current;
                            tun_enabled = requested;
                        } catch (const std::exception& exception) {
                            if (error.empty()) error = exception.what();
                        }

                        vpn_pending = false;
                        busy = false;
                        if (!ok) {
                            vpn_enabled = previous;
                            if (!error.empty()) toast.Show(error);
                        } else {
                            toast.Show(on ? "正在请求建立 VPN 隧道" : "VPN 隧道已关闭");
                        }
                    });
                })),
        SettingSwitchRow(
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
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

#else

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
            "开机自启动", "登录系统后自动启动 Clash-Flux（桌面端）",
            huxerui::Switch(settings.autoStart)
                .OnChanged([settingsModel](bool on) {
                    store::coreStore().setSetting(
                        "app.autostart", on ? "true" : "false");
                    settingsModel->Update(
                        [on](SettingsView& view) { view.autoStart = on; });
                })),
        // 内核启停与流量接管解耦后的「启动入口」之一：默认不随应用启动内核
        // （内核只是本地端口 + 控制接口），需要时用首页右下角悬浮按钮启动。
        SettingSwitchRow(
            "启动时自动运行内核", "打开应用就拉起 sing-box，并按已记录的系统代理/TUN 恢复接管",
            huxerui::Switch(settings.autoRun)
                .OnChanged([settingsModel](bool on) {
                    store::coreStore().setSetting("app.auto_run",
                                                  on ? "true" : "false");
                    settingsModel->Update(
                        [on](SettingsView& view) { view.autoRun = on; });
                })),
        SettingSwitchRow(
            "启用托盘图标", "关闭后托盘不可用，关闭窗口即退出",
            huxerui::Switch(settings.trayEnabled)
                .OnChanged([settingsModel](bool on) {
                    store::coreStore().setSetting("tray.enabled",
                                                  on ? "true" : "false");
                    settingsModel->Update(
                        [on](SettingsView& view) { view.trayEnabled = on; });
                })),
        SettingSwitchRow(
            "启动时隐藏到托盘", "下次启动不显示主窗口，经托盘唤出",
            huxerui::Switch(settings.startMinimized)
                .OnChanged([settingsModel](bool on) {
                    store::coreStore().setSetting(
                        "tray.start_minimized", on ? "true" : "false");
                    settingsModel->Update(
                        [on](SettingsView& view) { view.startMinimized = on; });
                })),
        SettingRow(
            "关闭窗口时",
            "托盘可用时的驻留行为（代理继续后台运行 = 最小化到托盘）",
            huxerui::SegmentedButton(
                std::vector<huxerui::StringVariant>{"每次询问", "直接退出",
                                                    "最小化到托盘"},
                static_cast<std::size_t>(settings.closeBehavior))
                .OnChanged([settingsModel](std::size_t index) {
                    store::coreStore().setSetting("tray.close_behavior",
                                                  std::to_string(index));
                    settingsModel->Update([index](SettingsView& view) {
                        view.closeBehavior = static_cast<int>(index);
                    });
                })),
    }.With(huxerui::Spacing(10.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View DesktopKernelSettings() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const huxerui::ApplicationHandle application = huxerui::UseApplication();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto dialog = huxerui::UseDialog();
    auto clipboard = application.Clipboard();
    auto snap = huxerui::UseState<store::CoreSnapshot>({});
    auto service_installed = huxerui::UseState(service::installed());
    auto proxyEnabled =
        huxerui::UseState(store::coreStore().systemProxyEnabled());
    auto tunEnabled = huxerui::UseState(store::coreStore().snapshot().tunEnabled);
    auto proxyPending = huxerui::UseState(false);
    auto tunPending = huxerui::UseState(false);
    const std::string detectedShell = CurrentEnvironmentShell();
    const std::string savedShell =
        store::coreStore().setting("ui.env_shell", detectedShell);
    auto envShell = huxerui::UseState(EnvironmentShellIndex(savedShell));
    // 首帧组合早于 hydrate，上面拿到的可能是探测值；模型报 ready 后补一次库里
    // 保存的 shell（见 settings_model.h）。
    const auto settingsModel = huxerui::UseService<SettingsModel>();
    auto envShellHydrated = huxerui::UseState(false);
    huxerui::Lifecycle(
        [envShell, envShellHydrated, settingsModel, detectedShell] {
            const SettingsView settings = settingsModel->view.Get();
            if (!envShellHydrated.Get() && settings.ready) {
                envShellHydrated = true;
                envShell = EnvironmentShellIndex(
                    settings.envShell.empty() ? detectedShell
                                              : settings.envShell);
            }
            return [] {};
        },
        settingsModel->view);
    const std::vector<std::string> envShellLabels =
        EnvironmentShellLabels();
    auto busy = huxerui::UseState(false);

    // 内核/接管状态来自唯一来源 CoreModel（见 core_model.h）：以模型的 State 作
    // 依赖镜像到本段的受控值——模型一变就同步一次，不再是 1s 定时器。
    const auto coreModel = huxerui::UseService<CoreModel>();
    huxerui::Lifecycle(
        [snap, service_installed, proxyEnabled, tunEnabled, proxyPending,
         tunPending, coreModel] {
            const CoreView view = coreModel->view.Get();
            snap = view.core;
            service_installed = view.serviceInstalled;
            if (!proxyPending.Get()) proxyEnabled = view.systemProxyIntent;
            if (!tunPending.Get()) tunEnabled = view.core.tunEnabled;
            return [] {};
        },
        coreModel->view);

    const store::CoreSnapshot s = snap.Get();
    const bool running = s.state == core::CoreState::Running;
    const std::string state_text =
        s.binaryPath.empty()
            ? "未找到内核运行时"
            : std::format("{} · {}{}", core::stateName(s.state),
                          s.version.empty() ? "sing-box" : s.version,
                          s.lastError.empty() ? "" : " · " + s.lastError);

    const auto core_action = [tasks, toast, busy,
                              coreModel](std::function<void()> job,
                                         std::string ok_message) {
        // finished 回调在 UI 线程执行：动作一完成就请模型重读，权威值不必等下一拍。
        LaunchSettingsAction(tasks, toast, busy, std::move(job),
                             std::move(ok_message),
                             [coreModel](bool) { coreModel->RequestRefresh(); });
    };

    return huxerui::Column {
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

        CLASHFLUX_LINUX_SERVICE_ROW(service_installed, tasks, toast),
        SettingSwitchRow(
            "系统代理",
            std::format("写入桌面系统代理（127.0.0.1:{}）", s.mixedPort),
            huxerui::Switch(proxyPending.Get()
                                ? proxyEnabled.Get()
                                : coreModel->view.Get().systemProxyIntent)
                .OnChanged([tasks, toast, proxyEnabled, proxyPending,
                            coreModel](bool on) {
                    if (proxyPending.Get()) return;
                    const bool previous = proxyEnabled.Get();
                    proxyEnabled = on;
                    proxyPending = true;
                    tasks.Launch([toast, proxyEnabled, proxyPending, previous, on,
                                  coreModel]() -> huxerui::Task<void> {
                        DesktopModeApplyResult result;
                        try {
                            result = co_await RunOnTaskThread([on] {
                                return ApplyDesktopSystemProxy(on);
                            });
                        } catch (const std::exception& exception) {
                            result.error = exception.what();
                        }
                        proxyPending = false;
                        if (result.status != DesktopModeApplyStatus::Applied) {
                            // 失败回落要做目标值校验：只有界面仍停在本任务的意图
                            // 值时才回退；用户若已经点到别处，说明有更新的意图，
                            // 不覆盖它（避免"失败回落把新状态打回去"）。
                            if (proxyEnabled.Get() == on) proxyEnabled = previous;
                            toast.Show(result.error.empty() ? "系统代理设置失败"
                                                            : result.error);
                        } else {
                            // 成功不回写本地 State（它已经等于 on）：把权威值写透
                            // 进模型即可，首页卡/托盘下一帧跟随，也不会二次闪烁。
                            coreModel->Update([on](CoreView& view) {
                                view.systemProxyIntent = on;
                                view.systemProxyActive = on;
                            });
                            toast.Show(on ? "系统代理已开启" : "系统代理已关闭");
                        }
                    });
                })),
        SettingSwitchRow(
            "TUN 模式",
            running ? "全局透明代理（需 root/CAP_NET_ADMIN，立即生效）"
                    : "全局透明代理（下次启动生效）",
            huxerui::Switch(tunPending.Get()
                                ? tunEnabled.Get()
                                : coreModel->view.Get().core.tunEnabled)
                .OnChanged([s, tasks, toast, dialog, clipboard, tunEnabled,
                           tunPending, coreModel,
                           textColor = theme.colors.on_surface,
                           hintColor = theme.colors.on_surface_variant](bool on) {
                    if (tunPending.Get()) return;
                    const bool previous = tunEnabled.Get();
                    tunEnabled = on;
                    tunPending = true;
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        DesktopModeApplyResult result;
                        try {
                            result = co_await RunOnTaskThread(
                                [on] { return ApplyDesktopTun(on); });
                        } catch (const std::exception& exception) {
                            result.error = exception.what();
                        }
                        tunPending = false;
                        if (result.status == DesktopModeApplyStatus::Applied) {
                            // 成功：权威值写透模型，不再用 RequestRefresh 赌下一拍。
                            coreModel->Update([on](CoreView& view) {
                                view.core.tunEnabled = on;
                            });
                            toast.Show(on ? "TUN 已开启" : "TUN 已关闭");
                            co_return;
                        }
                        // 失败回落同样做目标值校验（见系统代理处的说明）。
                        if (tunEnabled.Get() == on) tunEnabled = previous;
                        if (result.status == DesktopModeApplyStatus::ElevationRequested) {
                            toast.Show("已请求管理员权限重启，请在新窗口开启 TUN");
                            co_return;
                        }
                        if (result.status == DesktopModeApplyStatus::PermissionDenied) {
                            ShowTunGuideDialog(dialog, clipboard, toast,
                                               textColor, hintColor);
                            co_return;
                        }
                        toast.Show(result.error.empty() ? "TUN 切换失败"
                                                        : result.error);
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
    }.With(huxerui::Spacing(12.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

#endif

#undef CLASHFLUX_LINUX_SERVICE_ROW

} // namespace clashflux::ui
