// settings_page.cpp — 设置页：内核控制（状态/启停/重启）、出站模式、混合端口、
// 局域网连接、日志级别、外观主题、关于。
//
// 状态泵：1s 一拍把 coreStore snapshot 写入本地 State（快照读是轻量锁内拷贝，
// 泵在 UI 线程直接跑，只有 PATCH/启停等阻塞操作走 RunOnTaskThread）。
#include <huxerui/huxerui.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "ui.h"
#include "task_bridge.h"

import clashflux.core;
import clashflux.service;
import clashflux.store.core;
import clashflux.store.profiles;
import clashflux.utils;

namespace clashflux::ui {
namespace {

const std::vector<std::string> kModes{"rule", "global", "direct"};
// SegmentedButton 吃 StringVariant 列表；Select 用普通 string 列表。
const std::vector<huxerui::StringVariant> kModeNames{"规则", "全局", "直连"};
const std::vector<std::string> kLogLevels{"silent", "error", "warning", "info",
                                          "debug"};
const std::vector<huxerui::StringVariant> kThemeNames{"跟随系统", "深色", "浅色"};

// 版本号编译期常量由顶层 CMake 注入（hcg 不支持 composable 内条件编译，
// 字符串在文件作用域先拼好）。
const std::string kAboutText =
    std::format("Clash-Flux v{} · mihomo 客户端", CLASHFLUX_VERSION);

[[huxerui::composable]] huxerui::View SettingRow(const std::string& label,
                                                 const std::string& hint,
                                                 huxerui::View control) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Row {
        huxerui::Column {
            huxerui::Text(label).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody), theme.colors.on_surface}),
            hint.empty()
                ? huxerui::View{huxerui::Row{}}
                : huxerui::View{huxerui::Text(hint).Style(huxerui::TextStyle{
                      huxerui::Font::System(font_size::kCaption),
                      theme.colors.on_surface_variant})},
        }.With(huxerui::Spacing(2.0F)),
        huxerui::Spacer(),
        std::move(control),
    }.With(huxerui::Spacing(12.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

[[huxerui::composable]] huxerui::View SectionTitle(const std::string& title) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Text(title).Style(huxerui::TextStyle{
        huxerui::Font::System(font_size::kChip).WithWeight(huxerui::FontWeight::Bold),
        theme.colors.primary});
}

} // namespace

[[huxerui::composable]] huxerui::View SettingsPage(huxerui::State<int> themeMode) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto dialog = huxerui::UseDialog();
    auto snap = huxerui::UseState<store::CoreSnapshot>({});
    auto portValue = huxerui::UseState(huxerui::TextEditingValue{""});
    auto busy = huxerui::UseState(false);
    auto serviceInstalled = huxerui::UseState(service::installed());

    huxerui::Lifecycle(
        [tasks, snap, portValue, serviceInstalled] {
            tasks.Launch([=]() -> huxerui::Task<void> {
                co_await PollWhile(std::chrono::duration<double>{1.0}, [=] {
                    const auto s = store::coreStore().snapshot();
                    snap = s;
                    serviceInstalled = service::installed();
                    // 端口输入框未编辑过就用当前值初始化。
                    if (portValue.Get().text.empty() && s.mixedPort > 0) {
                        portValue = huxerui::TextEditingValue{
                            std::to_string(s.mixedPort)};
                    }
                    return true;
                });
            });
            return [] {};
        },
        0);

    const store::CoreSnapshot s = snap.Get();
    const bool running = s.state == core::CoreState::Running;

    // 通用动作：阻塞活在任务线程，错误 toast，完成后快照由泵刷新。
    auto coreAction = [tasks, toast, busy](std::function<void()> job,
                                           const std::string& okMsg) {
        if (busy.Get()) return;
        busy = true;
        tasks.Launch([=]() -> huxerui::Task<void> {
            try {
                co_await RunOnTaskThread(std::move(job));
                if (!okMsg.empty()) toast.Show(okMsg);
            } catch (const std::exception& e) {
                toast.Show(e.what());
            }
            // 给内核一拍喘息再解除 busy（startCore 内已等控制器就绪）。
            busy = false;
        });
    };

    // ---- 内核控制 ----
    const std::string stateText =
        s.binaryPath.empty()
            ? "未找到 mihomo 内核 —— 请将二进制放入 engines/ 或 PATH"
            : std::format("{} · {}{}", core::stateName(s.state),
                          s.version.empty() ? "mihomo" : s.version,
                          s.lastError.empty() ? "" : " · " + s.lastError);

    // ---- 出站模式 ----
    std::size_t modeIndex = 0;
    for (std::size_t i = 0; i < kModes.size(); ++i) {
        if (s.mode == kModes[i]) modeIndex = i;
    }

    // ---- 日志级别 ----
    std::size_t levelIndex = 3;
    for (std::size_t i = 0; i < kLogLevels.size(); ++i) {
        if (s.logLevel == kLogLevels[i]) levelIndex = i;
    }

    return PageScaffold(
        "设置",
        huxerui::Row{},
        huxerui::ScrollView(
            huxerui::Column {
                Card(huxerui::Column {
                    SectionTitle("内核"),
                    huxerui::Text(stateText)
                        .Style(huxerui::TextStyle{
                            huxerui::Font::System(font_size::kBody),
                            s.state == core::CoreState::Failed
                                ? theme.colors.error
                                : theme.colors.on_surface}),
                    huxerui::Row {
                        running
                            ? huxerui::View{huxerui::Button("停止").OnClick(
                                  [coreAction] {
                                      coreAction(
                                          [] { store::coreStore().stopCore(); },
                                          "内核已停止");
                                  })}
                            : huxerui::View{huxerui::Button("启动").OnClick(
                                  [coreAction] {
                                      coreAction(
                                          [] {
                                              auto& core = store::coreStore();
                                              core.startCore(
                                                  store::profilesStore()
                                                      .selectedYaml());
                                          },
                                          "");
                                  })},
                        huxerui::Button("重启")
                            .OnClick([coreAction] {
                                coreAction(
                                    [] {
                                        auto& core = store::coreStore();
                                        core.stopCore();
                                        core.startCore(
                                            store::profilesStore().selectedYaml());
                                    },
                                    "内核已重启");
                            })
                            .With(huxerui::Enabled(running)),
                    }.With(huxerui::Spacing(8.0F)),
                }.With(huxerui::Spacing(8.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),

                Card(huxerui::Column {
                    SectionTitle("代理"),
                    SettingRow(
                        "出站模式", "规则 / 全局 / 直连",
                        huxerui::SegmentedButton(kModeNames, modeIndex)
                            .OnChanged([coreAction](std::size_t idx) {
                                coreAction(
                                    [idx] {
                                        if (!store::coreStore().applyMode(
                                                kModes[idx])) {
                                            throw std::runtime_error(
                                                "切换模式失败（内核未运行？）");
                                        }
                                    },
                                    "");
                            })),
                    SettingRow(
                        "混合端口", "HTTP/SOCKS 混合入站端口（下次启动生效）",
                        huxerui::Row {
                            huxerui::TextField(portValue.Get())
                                .OnChanged(
                                    [portValue](
                                        const huxerui::TextEditingValue& v) {
                                        portValue = v;
                                    })
                                .With(huxerui::Frame{.width = 100.0F}),
                            huxerui::Button("保存").OnClick([portValue, toast] {
                                const std::string text = portValue.Get().text;
                                try {
                                    const int port = std::stoi(text);
                                    if (port < 1 || port > 65535) throw 0;
                                    store::coreStore().setSetting(
                                        "core.mixed_port", std::to_string(port));
                                    toast.Show("端口已保存（重启内核生效）");
                                } catch (...) {
                                    toast.Show("端口无效");
                                }
                            }),
                        }.With(huxerui::Spacing(8.0F))),
                    SettingRow(
                        "局域网连接", "允许局域网设备接入（下次启动生效）",
                        huxerui::Switch(
                            store::coreStore().setting("core.allow_lan", "false") ==
                            "true")
                            .OnChanged([coreAction](bool on) {
                                coreAction(
                                    [on] {
                                        auto& core = store::coreStore();
                                        core.setSetting("core.allow_lan",
                                                        on ? "true" : "false");
                                        if (core.snapshot().state ==
                                            core::CoreState::Running) {
                                            core.api().patchConfigs(
                                                std::format(
                                                    "{{\"allow-lan\":{}}}",
                                                    on ? "true" : "false"));
                                        }
                                    },
                                    on ? "已允许局域网连接（重启内核完全生效）"
                                       : "已关闭局域网连接");
                            })),
                    SettingRow(
                        "日志级别", "内核日志详细程度",
                        huxerui::Select(
                            kLogLevels, levelIndex,
                            [](const std::string& name) {
                                return huxerui::Text(name);
                            })
                            .OnChanged([coreAction](std::size_t idx) {
                                coreAction(
                                    [idx] {
                                        auto& core = store::coreStore();
                                        core.setSetting("core.log_level",
                                                        kLogLevels[idx]);
                                        if (core.snapshot().state ==
                                            core::CoreState::Running) {
                                            core.api().patchConfigs(
                                                std::format("{{\"log-level\":\"{}\"}}",
                                                            kLogLevels[idx]));
                                        }
                                    },
                                    "");
                            })),
                }.With(huxerui::Spacing(10.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),

                Card(huxerui::Column {
                    SectionTitle("系统"),
                    SettingRow(
                        "内核服务",
                        serviceInstalled.Get()
                            ? "已安装（内核由 root 服务托管，TUN 开箱可用）"
                            : "安装 root 服务后，TUN 无需每次授权（经 pkexec "
                              "一次性提权）",
                        huxerui::Button(serviceInstalled.Get() ? "卸载服务"
                                                               : "安装服务")
                            .OnClick([coreAction,
                                      installed = serviceInstalled.Get()] {
                                coreAction(
                                    [installed] {
                                        // pkexec 弹系统授权框，以 root 重入
                                        // 本二进制的 service install/uninstall。
                                        const std::string exe =
                                            std::filesystem::read_symlink(
                                                "/proc/self/exe")
                                                .string();
                                        const int rc = std::system(
                                            std::format("pkexec \"{}\" service {}",
                                                        exe,
                                                        installed ? "uninstall"
                                                                  : "install")
                                                .c_str());
                                        if (rc != 0) {
                                            throw std::runtime_error(
                                                installed
                                                    ? "卸载被取消或失败"
                                                    : "安装被取消或失败（需要"
                                                      "授权）");
                                        }
                                    },
                                    installed ? "服务已卸载" : "服务已安装");
                            })),
                    SettingRow(
                        "系统代理",
                        store::coreStore().systemProxySupported()
                            ? std::format("写入桌面系统代理（127.0.0.1:{}）",
                                          s.mixedPort)
                            : "当前桌面环境不支持（仅 KDE / GNOME 系）",
                        huxerui::Switch(store::coreStore().systemProxyEnabled())
                            .OnChanged([coreAction](bool on) {
                                coreAction(
                                    [on] {
                                        if (!store::coreStore()
                                                 .applySystemProxy(on)) {
                                            const std::string err =
                                                store::coreStore()
                                                    .snapshot()
                                                    .lastError;
                                            throw std::runtime_error(
                                                err.empty() ? "系统代理设置失败"
                                                            : err);
                                        }
                                    },
                                    on ? "系统代理已开启" : "系统代理已关闭");
                            })
                            .With(huxerui::Enabled(
                                store::coreStore().systemProxySupported()))),
                    SettingRow(
                        "TUN 模式",
                        running
                            ? "全局透明代理（需 root/CAP_NET_ADMIN，立即生效）"
                            : "全局透明代理（下次启动生效）",
                        huxerui::Switch(s.tunEnabled)
                            .OnChanged([tasks, toast, dialog,
                                        textColor = theme.colors.on_surface,
                                        hintColor =
                                            theme.colors.on_surface_variant](bool on) {
                                tasks.Launch([=]() -> huxerui::Task<void> {
                                    if (on) {
                                        // 门禁/弹窗会卸载点击路径：先让出一拍
                                        // （约定 4/6）。
                                        co_await huxerui::Delay(
                                            std::chrono::duration<double>{0});
                                        const core::TunGate gate =
                                            co_await RunOnTaskThread([] {
                                                return core::tunGate();
                                            });
                                        if (gate == core::TunGate::Elevated) {
                                            toast.Show("已请求管理员权限重启，请在"
                                                       "新窗口开启 TUN");
                                            co_return;
                                        }
                                        if (gate == core::TunGate::Denied) {
                                            ShowTunGuideDialog(dialog, textColor,
                                                               hintColor);
                                            co_return;
                                        }
                                    }
                                    const bool ok = co_await RunOnTaskThread(
                                        [on] {
                                            return store::coreStore().applyTun(on);
                                        });
                                    if (!ok) {
                                        const std::string err =
                                            store::coreStore().snapshot().lastError;
                                        toast.Show(err.empty() ? "TUN 切换失败"
                                                               : err);
                                    } else {
                                        toast.Show(on ? "TUN 已开启"
                                                      : "TUN 已关闭");
                                    }
                                });
                            })),
                }.With(huxerui::Spacing(10.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),

                Card(huxerui::Column {
                    SectionTitle("外观"),
                    SettingRow(
                        "主题", "",
                        huxerui::SegmentedButton(kThemeNames,
                                                 static_cast<std::size_t>(
                                                     themeMode.Get()))
                            .OnChanged([themeMode](std::size_t idx) {
                                themeMode = static_cast<int>(idx);
                                store::coreStore().setSetting(
                                    "ui.theme_mode", std::to_string(idx));
                            })),
                }.With(huxerui::Spacing(10.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),

                Card(huxerui::Column {
                    SectionTitle("关于"),
                    huxerui::Text(kAboutText)
                        .Style(huxerui::TextStyle{
                            huxerui::Font::System(font_size::kChip),
                            theme.colors.on_surface_variant}),
                }.With(huxerui::Spacing(6.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),
            }.With(huxerui::Spacing(12.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
            .With(huxerui::Grow(1.0F)));
}

} // namespace clashflux::ui
