// settings_page.cpp — 设置页的三个固定模块：通用 / 内核 / 关于。
//
// 平台专属内容由模块入口处的编译宏选择，平台函数内部自洽管理状态、
// 任务、权限和控件；这里不维护 Android/桌面能力矩阵。
#include <huxerui/huxerui.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app_resources.h"
#include "ui.h"
#include "task_bridge.h"

import clashflux.config;
import clashflux.stream;
import clashflux.store.core;
import clashflux.store.profiles;

namespace clashflux::ui {
namespace {

const std::vector<std::string> kModes{"rule", "global", "direct"};
const std::vector<huxerui::StringVariant> kModeNames{"规则", "全局", "直连"};
const std::vector<huxerui::StringVariant> kThemeNames{"跟随系统", "深色", "浅色"};

// 宏只选择模块级平台函数，不把平台能力拆成控件级过滤条件。
#if defined(__ANDROID__)
#define CLASHFLUX_GENERAL_PLATFORM_SECTION AndroidGeneralSettings
#define CLASHFLUX_KERNEL_PLATFORM_SECTION AndroidKernelSettings
#define CLASHFLUX_MORE_SETTINGS AndroidMoreSettings
#else
#define CLASHFLUX_GENERAL_PLATFORM_SECTION DesktopGeneralSettings
#define CLASHFLUX_KERNEL_PLATFORM_SECTION DesktopKernelSettings
#define CLASHFLUX_MORE_SETTINGS DesktopMoreSettings
#endif

const std::string kAboutText = std::format(
    "Clash-Flux v{} · sing-box 内核（桌面 spawn / Android libbox）",
    CLASHFLUX_VERSION);

} // namespace

[[huxerui::composable]] huxerui::View AndroidMoreSettings(
    huxerui::State<std::size_t> navPage) {
    return huxerui::Column {
        SectionTitle("更多"),
        Card(huxerui::Row {
                 huxerui::Text("连接"), huxerui::Spacer(), huxerui::Text("›"),
             }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)))
            .OnClick([navPage] { navPage = 4; })
            .With(huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                     .label = "连接"},
                  huxerui::Focusable(true), huxerui::Enabled(true)),
        Card(huxerui::Row {
                 huxerui::Text("日志"), huxerui::Spacer(), huxerui::Text("›"),
             }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)))
            .OnClick([navPage] { navPage = 5; })
            .With(huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                     .label = "日志"},
                  huxerui::Focusable(true), huxerui::Enabled(true)),
        Card(huxerui::Row {
                 huxerui::Text("规则"), huxerui::Spacer(), huxerui::Text("›"),
             }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)))
            .OnClick([navPage] { navPage = 3; })
            .With(huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                     .label = "规则"},
                  huxerui::Focusable(true), huxerui::Enabled(true)),
    }.With(huxerui::Spacing(10.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View DesktopMoreSettings(
    huxerui::State<std::size_t>) {
    return huxerui::View{};
}

[[huxerui::composable]] huxerui::View SettingsPage(
    huxerui::State<int> themeMode, huxerui::State<std::size_t> navPage) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    auto tasks = huxerui::UseTaskScope();
    auto transition = huxerui::UseSceneTransition();
    struct ThemeAnimationFlag {
        bool animating = false;
    };
    auto animating =
        huxerui::UseState(std::make_shared<ThemeAnimationFlag>());
    auto toast = huxerui::UseToast();
    auto snap = huxerui::UseState<store::CoreSnapshot>({});
    auto portValue = huxerui::UseState(huxerui::TextEditingValue{""});
    auto allowLan = huxerui::UseState(
        store::coreStore().setting("core.allow_lan", "false") == "true");
    auto ipv6Enabled = huxerui::UseState(
        store::coreStore().setting("core.ipv6_enabled", "false") == "true");
    auto busy = huxerui::UseState(false);

    huxerui::Lifecycle(
        [tasks, snap, portValue, allowLan, ipv6Enabled] {
            tasks.Launch([=]() -> huxerui::Task<void> {
                co_await PollWhile(std::chrono::duration<double>{1.0}, [=] {
                    const auto current = store::coreStore().snapshot();
                    snap = current;
                    allowLan = store::coreStore().setting(
                                   "core.allow_lan", "false") == "true";
                    ipv6Enabled = store::coreStore().setting(
                                      "core.ipv6_enabled", "false") == "true";
                    if (portValue.Get().text.empty() && current.mixedPort > 0) {
                        portValue = huxerui::TextEditingValue{
                            std::to_string(current.mixedPort)};
                    }
                    return true;
                });
            });
            return [] {};
        },
        0);

    // 主题模式：0=跟随系统，1=深色，2=浅色。
    const auto applyTheme = [themeMode, transition, tasks, animating](int mode) {
        if (animating.Get()->animating) return;
        const bool currentDark =
            themeMode.Get() == 1 ||
            (themeMode.Get() == 0 && cfg::systemPrefersDark());
        const bool targetDark =
            mode == 1 || (mode == 0 && cfg::systemPrefersDark());
        const auto mutation = [themeMode, mode] {
            themeMode = mode;
            store::coreStore().setSetting("ui.theme_mode", std::to_string(mode));
        };
        if (currentDark == targetDark) {
            mutation();
            return;
        }

        animating.Get()->animating = true;
        tasks.Launch([animating]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0.5});
            animating.Get()->animating = false;
        });
        const huxerui::TransitionSpec reveal{
            huxerui::CircularRevealTransition{}, huxerui::TweenSpec{0.36}};
        transition.RunFromCurrentInteraction(
            currentDark ? reveal.Reversed() : reveal, std::move(mutation));
    };

    // 通用设置动作：阻塞操作统一移到任务线程，完成后由快照泵更新页面。
    const auto coreAction = [tasks, toast, busy](std::function<void()> job,
                                                  const std::string& okMessage) {
        if (busy.Get()) return;
        busy = true;
        tasks.Launch([=]() -> huxerui::Task<void> {
            try {
                co_await RunOnTaskThread(std::move(job));
                if (!okMessage.empty()) toast.Show(okMessage);
            } catch (const std::exception& error) {
                stream::logApplication("error",
                                       std::format("设置操作失败：{}", error.what()));
                toast.Show(error.what());
            }
            busy = false;
        });
    };

    const store::CoreSnapshot s = snap.Get();
    std::size_t modeIndex = 0;
    for (std::size_t i = 0; i < kModes.size(); ++i) {
        if (s.mode == kModes[i]) modeIndex = i;
    }
    return PageScaffold(
        "设置", huxerui::Row{},
        huxerui::ScrollView(
            huxerui::Column {
                CLASHFLUX_MORE_SETTINGS(navPage),
                Card(huxerui::Column {
                    SectionTitle("通用"),
                    SettingRow(
                        "主题", "",
                        huxerui::SegmentedButton(
                            kThemeNames, static_cast<std::size_t>(themeMode.Get()))
                            .OnChanged([applyTheme](std::size_t index) {
                                applyTheme(static_cast<int>(index));
                            })),
                    CLASHFLUX_GENERAL_PLATFORM_SECTION(),
                }.With(huxerui::Spacing(10.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),

                Card(huxerui::Column {
                    SectionTitle("内核"),
                    SettingRow(
                        "出站模式", "规则 / 全局 / 直连",
                        huxerui::SegmentedButton(kModeNames, modeIndex)
                            .OnChanged([coreAction](std::size_t index) {
                                coreAction(
                                    [index] {
                                        if (!store::coreStore().applyMode(
                                                kModes[index])) {
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
                                .OnChanged([portValue](
                                               const huxerui::TextEditingValue& value) {
                                    portValue = value;
                                })
                                .With(huxerui::Frame{.width = 100.0F}),
                            huxerui::IconButton(app::images::save, "保存设置")
                                .With(huxerui::Tooltip("保存设置"))
                                .OnClick([portValue, toast] {
                                try {
                                    const int port = std::stoi(portValue.Get().text);
                                    if (port < 1 || port > 65535) throw 0;
                                    store::coreStore().setSetting(
                                        "core.mixed_port", std::to_string(port));
                                    toast.Show("端口已保存（重启内核生效）");
                                } catch (...) {
                                    toast.Show("端口无效");
                                }
                            }),
                        }.With(huxerui::Spacing(8.0F))),
                    CLASHFLUX_KERNEL_PLATFORM_SECTION(),
                    SettingSwitchRow(
                        "局域网连接", "允许局域网设备接入（下次启动生效）",
                        huxerui::Switch(allowLan.Get())
                            .OnChanged([coreAction, allowLan, busy](bool on) {
                                if (busy.Get()) return;
                                allowLan = on;
                                coreAction(
                                    [on] {
                                        store::coreStore().setSetting(
                                            "core.allow_lan", on ? "true" : "false");
                                    },
                                    on ? "已允许局域网连接（重启内核生效）"
                                       : "已关闭局域网连接");
                            })),
                    SettingSwitchRow(
                        "IPv6", "重启内核生效",
                        huxerui::Switch(ipv6Enabled.Get())
                            .OnChanged([coreAction, ipv6Enabled, busy](bool on) {
                                if (busy.Get()) return;
                                ipv6Enabled = on;
                                coreAction(
                                    [on] {
                                        store::coreStore().setSetting(
                                            "core.ipv6_enabled",
                                            on ? "true" : "false");
                                    },
                                    on ? "已启用 IPv6（重启内核生效）"
                                       : "已关闭 IPv6（重启内核生效）");
                            })),
                }.With(huxerui::Spacing(10.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),

                Card(huxerui::Column {
                    SectionTitle("关于"),
                    huxerui::Text(kAboutText).Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kChip),
                        theme.colors.on_surface_variant}),
                }.With(huxerui::Spacing(6.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),
                compact ? CompactFloatingNavigationFooter() : huxerui::View{},
            }.With(huxerui::Spacing(12.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
            .With(huxerui::Grow(1.0F)));
}

#undef CLASHFLUX_GENERAL_PLATFORM_SECTION
#undef CLASHFLUX_KERNEL_PLATFORM_SECTION
#undef CLASHFLUX_MORE_SETTINGS

} // namespace clashflux::ui
