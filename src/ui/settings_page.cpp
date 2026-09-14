// settings_page.cpp — 设置页：内核控制（状态/启停/重启）、出站模式、混合端口、
// 局域网连接、日志级别、外观主题、关于。
//
// 状态泵：1s 一拍把 coreStore snapshot 写入本地 State（快照读是轻量锁内拷贝，
// 泵在 UI 线程直接跑，只有 PATCH/启停等阻塞操作走 RunOnTaskThread）。
#include <huxerui/huxerui.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "ui.h"
#include "task_bridge.h"

import clashflux.core;
import clashflux.config;
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
    std::format("Clash-Flux v{} · sing-box 内核（桌面 spawn / Android libbox）", CLASHFLUX_VERSION);
#if defined(__ANDROID__)
const std::string kDefaultCoreName = "sing-box libbox";
#else
const std::string kDefaultCoreName = "sing-box";
#endif

[[huxerui::composable]] huxerui::View SettingRow(const std::string& label,
                                                 const std::string& hint,
                                                 huxerui::View control) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    huxerui::View description = huxerui::Column {
        huxerui::Text(label).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kBody), theme.colors.on_surface}),
        hint.empty()
            ? huxerui::View{huxerui::Row{}}
            : huxerui::View{huxerui::Text(hint).Style(huxerui::TextStyle{
                  huxerui::Font::System(font_size::kCaption),
                  theme.colors.on_surface_variant})},
    }.With(huxerui::Spacing(2.0F));

    if (compact) {
        return huxerui::Column {
            std::move(description),
            std::move(control),
        }.With(huxerui::Spacing(8.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }

    return huxerui::Row {
        std::move(description),
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
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    const huxerui::ApplicationHandle application = huxerui::UseApplication();
    auto tasks = huxerui::UseTaskScope();
    // 与 apitab 的主题切换保持一致：整棵主题树用圆形揭示过渡；reduced
    // motion 由 HuxerUI 自动降级，但状态更新和配置落盘仍然必须执行。
    auto transition = huxerui::UseSceneTransition();
    // HuxerUI 的场景过渡没有公开“运行中”查询或完成回调，重复 Run 会替换
    // 当前过渡。按 apitab 的限制加冷却标记，动画期间再次点击直接忽略。
    struct ThemeAnimationFlag {
        bool animating = false;
    };
    auto animating = huxerui::UseState(std::make_shared<ThemeAnimationFlag>());
    auto toast = huxerui::UseToast();
    auto dialog = huxerui::UseDialog();
    auto clipboard = application.Clipboard();
    auto snap = huxerui::UseState<store::CoreSnapshot>({});
    auto portValue = huxerui::UseState(huxerui::TextEditingValue{""});
    auto busy = huxerui::UseState(false);
    auto serviceInstalled = huxerui::UseState(service::installed());
    // Android VPN 授权/服务回调发生在平台侧，不能把 Switch 的值直接绑在
    // 一次性的 setting() 读取上；用受控 State 立即响应点击，并由页面泵
    // 校准取消授权、系统撤销等异步结果。
    auto tunEnabled = huxerui::UseState(
        store::coreStore().setting("core.tun_enabled", "false") == "true");
    // 乐观开关：点击立即翻转显示，后台完成后清除覆盖（真实状态接管），
    // 失败自动回弹并提示。覆盖值非空即“进行中”，期间忽略再次点击，
    // 避免 TUN 重启内核期间的并发 stop/start。
    auto proxyOverride = huxerui::UseState<std::optional<bool>>(std::nullopt);
    auto tunOverride = huxerui::UseState<std::optional<bool>>(std::nullopt);
    auto vpnState = huxerui::UseState(AndroidVpnState());
    auto trayCloseBehavior = huxerui::UseState<std::size_t>([] {
        const std::string value =
            store::coreStore().setting("tray.close_behavior", "0");
        if (value == "1") return std::size_t{1};
        if (value == "2") return std::size_t{2};
        return std::size_t{0};
    }());

    huxerui::Lifecycle(
        [tasks, snap, portValue, serviceInstalled, tunEnabled, vpnState] {
            tasks.Launch([=]() -> huxerui::Task<void> {
                co_await PollWhile(std::chrono::duration<double>{1.0}, [=] {
                    const auto s = store::coreStore().snapshot();
                    snap = s;
                    serviceInstalled = service::installed();
                    tunEnabled = store::coreStore().setting("core.tun_enabled",
                                                            "false") == "true";
                    vpnState = AndroidVpnState();
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

    // 主题模式：0=跟随系统，1=深色，2=浅色。目标与当前有效深浅一致时
    // 只更新偏好，不播放动画。圆形揭示原点取同步事件的精确位置；键盘/无
    // 指针激活时由 HuxerUI 回落到激活控件中心。浅→深使用从内向外的揭示，
    // 深→浅使用上游的反向 TransitionSpec，让深色层从外向内收缩。
    auto applyTheme = [themeMode, transition, tasks, animating](int mode) {
        if (animating.Get()->animating) return;

        const bool currentDark =
            themeMode.Get() == 1 || (themeMode.Get() == 0 && cfg::systemPrefersDark());
        const bool targetDark = mode == 1 || (mode == 0 && cfg::systemPrefersDark());
        auto mutation = [themeMode, mode] {
            themeMode = mode;
            store::coreStore().setSetting("ui.theme_mode", std::to_string(mode));
        };
        if (currentDark == targetDark) {
            mutation();
            return;
        }

        animating.Get()->animating = true;
        // 冷却时间略长于 CircularRevealTransition 默认 0.36s，避免
        // 第二次同步触发打断首个场景过渡；完成后只解除门禁，不改主题状态。
        tasks.Launch([animating]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0.5});
            animating.Get()->animating = false;
        });

        // 必须在同步事件回调中调用；异步代码若已有窗口坐标，按 HuxerUI
        // 约定应使用 RunAt，而不能在这里延迟调用 RunFromCurrentInteraction。
        const huxerui::TransitionSpec reveal{
            huxerui::CircularRevealTransition{}, huxerui::TweenSpec{0.36}};
        const huxerui::TransitionSpec transitionSpec =
            currentDark ? reveal.Reversed() : reveal;
        transition.RunFromCurrentInteraction(
            transitionSpec, std::move(mutation));
    };

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
    const std::string stateText = s.binaryPath.empty()
        ? "未找到内核运行时"
        : std::format("{} · {}{}", core::stateName(s.state),
                      s.version.empty() ? kDefaultCoreName : s.version,
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
                // Android's data plane is the VpnService below.  The desktop
                // core buttons only generate YAML on Android and therefore
                // looked successful while never starting sing-box.
                [stateText, s, running, coreAction, theme]() -> huxerui::View {
                    if constexpr (CompileTimePlatform() == PlatformKind::Android) {
                        return {};
                    }
                    return Card(huxerui::Column {
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
                           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
                }(),

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
                                        // sing-box 的 clash_api 不支持热更
                                        // allow-lan：持久化后下次启动生效。
                                        store::coreStore().setSetting(
                                            "core.allow_lan",
                                            on ? "true" : "false");
                                    },
                                    on ? "已允许局域网连接（重启内核生效）"
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
                                        // sing-box 的 clash_api 不支持热更日志
                                        // 级别：持久化后下次启动生效（词表在
                                        // 配置生成与 WS 订阅层做映射）。
                                        store::coreStore().setSetting(
                                            "core.log_level", kLogLevels[idx]);
                                    },
                                    "");
                            })
                            .With(huxerui::Frame{.width = 180.0F})),
                }.With(huxerui::Spacing(10.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),

                PlatformControl(
                    {PlatformCode::CoreService, PlatformCode::SystemProxy,
                     PlatformCode::CoreTun},
                    [s, running, serviceInstalled, coreAction, tasks, toast,
                     dialog, clipboard, proxyOverride, tunOverride,
                     textColor = theme.colors.on_surface,
                     hintColor = theme.colors.on_surface_variant] {
                        return Card(huxerui::Column {
                            SectionTitle("系统"),
                            PlatformControl(
                                {PlatformCode::CoreService},
                                [serviceInstalled, coreAction] {
                                    return SettingRow(
                                        "内核服务",
                                        serviceInstalled.Get()
                                            ? "已安装（sing-box、PPTP 与 TUN 由 root 服务托管）"
                                            : "安装 root 服务后，TUN/PPTP 无需每次授权（经 pkexec "
                                              "一次性提权）",
                                        huxerui::Button(serviceInstalled.Get()
                                                            ? "卸载服务"
                                                            : "安装服务")
                                            .OnClick([
                                                coreAction,
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
                                                            std::format(
                                                                "pkexec \"{}\" service {}",
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
                                                    installed ? "服务已卸载"
                                                              : "服务已安装");
                                            }));
                                }),
                            PlatformControl(
                                {PlatformCode::SystemProxy},
                                [s, tasks, toast, proxyOverride] {
                                    return SettingRow(
                                        "系统代理",
                                        std::format(
                                            "写入桌面系统代理（127.0.0.1:{}）",
                                            s.mixedPort),
                                        huxerui::Switch(
                                            proxyOverride.Get().value_or(
                                                store::coreStore().systemProxyEnabled()))
                                            .OnChanged([tasks, toast,
                                                        proxyOverride](bool on) {
                                                // 乐观切换：先翻转，失败再回弹。
                                                if (proxyOverride.Get().has_value()) return;
                                                proxyOverride = on;
                                                tasks.Launch([=]() -> huxerui::Task<void> {
                                                    const bool ok = co_await RunOnTaskThread(
                                                        [on] {
                                                            return store::coreStore()
                                                                .applySystemProxy(on);
                                                        });
                                                    proxyOverride = std::nullopt;
                                                    if (!ok) {
                                                        const std::string err =
                                                            store::coreStore()
                                                                .snapshot()
                                                                .lastError;
                                                        toast.Show(err.empty()
                                                                       ? "系统代理设置失败"
                                                                       : err);
                                                    } else {
                                                        toast.Show(on ? "系统代理已开启"
                                                                      : "系统代理已关闭");
                                                    }
                                                });
                                            }));
                                }),
                            PlatformControl(
                                {PlatformCode::CoreTun},
                                [s, running, tasks, toast, dialog, clipboard,
                                 textColor, hintColor, tunOverride] {
                                    return SettingRow(
                                        "TUN 模式",
                                        running
                                            ? "全局透明代理（需 root/CAP_NET_ADMIN，立即生效）"
                                            : "全局透明代理（下次启动生效）",
                                        huxerui::Switch(
                                            tunOverride.Get().value_or(s.tunEnabled))
                                            .OnChanged(
                                                [s, tasks, toast, dialog, clipboard,
                                                 textColor, hintColor,
                                                 tunOverride](bool on) {
                                                    // 乐观切换：TUN 重启内核耗时数秒，
                                                    // 先翻转显示，失败/门禁拦截再回弹。
                                                    if (tunOverride.Get().has_value()) return;
                                                    tunOverride = on;
                                                    tasks.Launch(
                                                        [=]() -> huxerui::Task<void> {
                                                            if (on) {
                                                                // 门禁/弹窗会卸载点击路径：先让出一拍
                                                                // （约定 4/6）。
                                                                co_await huxerui::Delay(
                                                                    std::chrono::duration<
                                                                        double>{0});
                                                                const core::TunGate gate =
                                                                    co_await RunOnTaskThread(
                                                                        [] {
                                                                            return core::tunGate();
                                                                        });
                                                                if (gate ==
                                                                    core::TunGate::Elevated) {
                                                                    tunOverride = std::nullopt;
                                                                    toast.Show(
                                                                        "已请求管理员权限重启，请在新窗口开启 TUN");
                                                                    co_return;
                                                                }
                                                                if (gate ==
                                                                    core::TunGate::Denied) {
                                                                    tunOverride = std::nullopt;
                                                                    ShowTunGuideDialog(
                                                                        dialog, clipboard,
                                                                        toast, textColor,
                                                                        hintColor);
                                                                    co_return;
                                                                }
                                                            }
                                                            const bool ok =
                                                                co_await RunOnTaskThread(
                                                                    [on] {
                                                                        return store::coreStore()
                                                                            .applyTun(on);
                                                                    });
                                                            tunOverride = std::nullopt;
                                                            if (!ok) {
                                                                const std::string err =
                                                                    store::coreStore()
                                                                        .snapshot()
                                                                        .lastError;
                                                                toast.Show(
                                                                    err.empty()
                                                                        ? "TUN 切换失败"
                                                                        : err);
                                                            } else {
                                                                toast.Show(on ? "TUN 已开启"
                                                                              : "TUN 已关闭");
                                                            }
                                                        });
                                                }));
                                }),
                        }.With(huxerui::Spacing(10.0F),
                               huxerui::CrossAlign(
                                   huxerui::CrossAxisAlignment::Stretch)));
                    }),

                // Android VPN 隧道：这里不能复用 PlatformControl。真机上其
                // Scope 子组合曾错误丢弃 Android 专属卡，导致系统 VPN 无从
                // 开启、所有透明代理流量和统计都保持为 0。使用编译期平台分支
                // 直接声明卡片，桌面构建仍生成空 View。
                [coreAction, tunEnabled, vpnState]() -> huxerui::View {
                    if constexpr (CompileTimePlatform() != PlatformKind::Android) {
                        return {};
                    }
                    const int state = vpnState.Get();
                    const std::string status = state == 2
                        ? "已附着：sing-box 正通过 protect(fd) 使用物理网络"
                        : state == 1 ? "正在建立系统 VPN 与 TUN 数据面"
                        : state == 3 ? "启动失败：请查看日志页中的 Android VPN 错误"
                                     : "未连接；开启后将请求系统 VPN 授权";
                    return Card(huxerui::Column {
                        SectionTitle("Android 数据面"),
                        SettingRow("隧道状态", status,
                                   huxerui::Text(state == 2 ? "已连接" :
                                                 state == 1 ? "连接中" :
                                                 state == 3 ? "失败" : "未连接")),
                        SettingRow(
                            "VPN 代理",
                            "系统 VPN 由此服务持有；内核出站 socket 会自动绕过 TUN",
                            huxerui::Switch(tunEnabled.Get() || state == 1 || state == 2)
                                .OnChanged([coreAction, tunEnabled](bool on) {
                                    // 先写受控状态，避免必须切走页面再回来才看到开关变化。
                                    tunEnabled = on;
                                    coreAction(
                                        [on] {
                                            store::coreStore().setSetting(
                                                "core.tun_enabled",
                                                on ? "true" : "false");
                                            if (on) {
                                                // Rebuild the managed DNS block before
                                                // VpnService attaches the TUN. Android
                                                // TUN itself is started by Java, not by
                                                // this core restart.
                                                store::coreStore().startCore(
                                                    store::profilesStore().selectedYaml());
                                                AndroidStartVpn();
                                            } else {
                                                AndroidStopVpn();
                                            }
                                        },
                                        on ? "正在请求建立 VPN 隧道"
                                           : "VPN 隧道已关闭");
                                })),
                        SettingRow(
                            "后台保活",
                            "申请忽略电池优化，防止后台被杀；建议同时在系统设"
                            "置中允许本应用自启动（MIUI 等系统需要手动放行），"
                            "并在系统 VPN 设置里开启「始终开启」以便重启后自动"
                            "恢复隧道",
                            huxerui::Button("忽略电池优化").OnClick(
                                [] { AndroidRequestIgnoreBattery(); })),
                    }
                        .With(huxerui::Spacing(10.0F),
                              huxerui::CrossAlign(
                                  huxerui::CrossAxisAlignment::Stretch)));
                }(),

                PlatformControl({PlatformCode::SystemTray},
                                [trayCloseBehavior] {
                                    // 编译期第二道闸（kSystemTrayUi）：平台代
                                    // 码表是运行时判定，历史构建出现过 Android
                                    // 上托盘卡漏出的报告，这里兜底保证手机上
                                    // 不渲染托盘/关窗驻留选项。
                                    if (!kSystemTrayUi) return huxerui::View{};
                                    return Card(huxerui::Column {
                                        SectionTitle("托盘"),
                                        SettingRow(
                                            "启用托盘图标",
                                            "关闭后托盘不可用，关闭窗口即退出",
                                            huxerui::Switch(store::coreStore().setting(
                                                                "tray.enabled",
                                                                "true") == "true")
                                                .OnChanged([](bool on) {
                                                    store::coreStore().setSetting(
                                                        "tray.enabled",
                                                        on ? "true" : "false");
                                                })),
                                        SettingRow(
                                            "关闭窗口时",
                                            "托盘可用时的驻留行为（代理继续后台运行 = "
                                            "最小化到托盘）",
                                            huxerui::SegmentedButton(
                                                std::vector<huxerui::StringVariant>{
                                                    "每次询问", "直接退出", "最小化到托盘"},
                                                trayCloseBehavior.Get())
                                                .OnChanged(
                                                    [trayCloseBehavior](std::size_t idx) {
                                                        trayCloseBehavior = idx;
                                                        store::coreStore().setSetting(
                                                            "tray.close_behavior",
                                                            std::to_string(idx));
                                                    })),
                                        SettingRow(
                                            "启动时隐藏到托盘",
                                            "下次启动不显示主窗口，经托盘唤出",
                                            huxerui::Switch(store::coreStore().setting(
                                                                "tray.start_minimized",
                                                                "false") == "true")
                                                .OnChanged([](bool on) {
                                                    store::coreStore().setSetting(
                                                        "tray.start_minimized",
                                                        on ? "true" : "false");
                                                })),
                                    }
                                        .With(huxerui::Spacing(10.0F),
                                              huxerui::CrossAlign(
                                                  huxerui::CrossAxisAlignment::Stretch)));
                                }),

                Card(huxerui::Column {
                    SectionTitle("外观"),
                    SettingRow(
                        "主题", "",
                        huxerui::SegmentedButton(kThemeNames,
                                                 static_cast<std::size_t>(
                                                     themeMode.Get()))
                            .OnChanged([applyTheme](std::size_t idx) {
                                applyTheme(static_cast<int>(idx));
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
                compact ? CompactFloatingNavigationFooter() : huxerui::View{},
            }.With(huxerui::Spacing(12.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
            .With(huxerui::Grow(1.0F)));
}

} // namespace clashflux::ui
