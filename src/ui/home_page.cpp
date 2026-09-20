// home_page.cpp — 首页（概览）：实时速率统计卡 + 60s 流量曲线（Canvas 自绘）
// + 出站模式快速切换 + 当前订阅与内核状态。
//
// 数据流：UI 泵每 500ms 从 CoreStreams 取最新流量帧追加进 60 点环形历史
// （State<vector<TrafficPoint>>），连接快照帧只取总量字段；内核状态与启用
// 订阅每拍重读。Canvas 画家捕获历史快照，重组后按最新序列重绘。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "app_resources.h"
#include "ui.h"
#include "task_bridge.h"

import nlohmann.json;
import clashflux.core;
import clashflux.stream;
import clashflux.store.core;
import clashflux.store.profiles;
import clashflux.utils;

namespace clashflux::ui {
namespace {

constexpr std::size_t kHistoryPoints = 60;  // 60 拍 ≈ 30s 窗口

const std::vector<huxerui::StringVariant> kModeNames{"规则", "全局", "直连"};
const std::vector<std::string> kModes{"rule", "global", "direct"};
#if defined(__ANDROID__)
constexpr std::string_view kDefaultCoreName = "sing-box libbox";
#define CLASHFLUX_HOME_SYSTEM_CARD AndroidHomeSystemCard
#define CLASHFLUX_HOME_ACTION(state) huxerui::Row{}
#define CLASHFLUX_HOME_FLOATING_ACTION(page, state, compact) \
    AndroidHomeFloatingAction(std::move(page), state, compact)
#else
constexpr std::string_view kDefaultCoreName = "sing-box";
#define CLASHFLUX_HOME_SYSTEM_CARD DesktopHomeSystemCard
#define CLASHFLUX_HOME_ACTION(state) DesktopHomeAction(state)
#define CLASHFLUX_HOME_FLOATING_ACTION(page, state, compact) \
    DesktopHomeFloatingAction(std::move(page), state, compact)
#endif

struct HomeState {
    stream::TrafficPoint latest;
    std::vector<stream::TrafficPoint> history;  // 旧→新
    std::int64_t totalUp = 0;
    std::int64_t totalDown = 0;
    store::CoreSnapshot core;
    std::int64_t profileId = 0;
    std::string profileName;
    std::string profileUpdated;
    std::int64_t profileUsedBytes = 0;
    std::int64_t profileTotalBytes = 0;
    std::vector<ProxyGroupSnapshot> proxyGroups;

    bool operator==(const HomeState&) const = default;
};

std::string currentProxyLine(const std::vector<ProxyGroupSnapshot>& groups) {
    for (const ProxyGroupSnapshot& group : groups) {
        if (!group.current.empty()) return group.name + " · " + group.current;
    }
    return "暂无当前线路";
}

// Kept outside the composable body: HuxerUI's code generator deliberately
// rejects conditional compilation within a composable function.
void updateRuntime(HomeState& s, store::CoreStore& core) {
    s.core = core.snapshot();
#if defined(__ANDROID__)
    // Android libbox emits its own status stream instead of clash_api's
    // /traffic and /connections WebSockets.
    stream::TrafficPoint point{s.core.uploadRate, s.core.downloadRate, 0};
    s.latest = point;
    s.totalUp = s.core.uploadTotal;
    s.totalDown = s.core.downloadTotal;
    if (s.core.state == core::CoreState::Running) {
        s.history.push_back(point);
        if (s.history.size() > kHistoryPoints) s.history.erase(s.history.begin());
    }
#else
    stream::TrafficPoint point;
    if (core.streams().takeTraffic(point)) {
        s.latest = point;
        s.history.push_back(point);
        if (s.history.size() > kHistoryPoints) s.history.erase(s.history.begin());
    }
    if (s.core.state != core::CoreState::Running && !s.history.empty()) {
        s.history.clear();
        s.latest = {};
    }
    std::string frame;
    if (core.streams().takeConnections(frame)) {
        const auto j = nlohmann::json::parse(frame, nullptr, false);
        if (j.is_object()) {
            s.totalUp = j.value("uploadTotal", std::int64_t{0});
            s.totalDown = j.value("downloadTotal", std::int64_t{0});
        }
    }
#endif
}

// 统计卡：上标签下数值。
// 统计列：统计带内的一列指标；不带卡片外壳，由外层统计带统一包卡。
[[huxerui::composable]] huxerui::View StatMetric(const std::string& label,
                                                 const std::string& value,
                                                 huxerui::Color valueColor) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Column {
        huxerui::Text(label).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kCaption),
            theme.colors.on_surface_variant}),
        huxerui::Text(value).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kTitle)
                .WithWeight(huxerui::FontWeight::Bold),
            valueColor}),
    }.With(huxerui::Spacing(4.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Start));
}

// 流量曲线：下载面积图（主色渐变填充）+ 上传折线（琥珀）。历史不足两点画平线。
huxerui::CanvasPainter TrafficPainter(const std::vector<stream::TrafficPoint>& history,
                                      huxerui::Color downColor,
                                      huxerui::Color upColor,
                                      huxerui::Color gridColor) {
    return [=](huxerui::PaintContext& paint, huxerui::Size size) {
        const float w = size.width;
        const float h = size.height;
        if (w <= 0.0F || h <= 0.0F) return;

        // 网格：三条虚线横线。
        for (int i = 1; i <= 3; ++i) {
            const float y = h * static_cast<float>(i) / 4.0F;
            paint.DrawLine({0.0F, y}, {w, y}, gridColor,
                           huxerui::StrokeStyle{.width = 1.0F,
                                                .dash_pattern = {4.0F, 4.0F}});
        }

        std::int64_t peak = 1;
        for (const auto& p : history) {
            peak = std::max({peak, p.up, p.down});
        }
        const auto yOf = [h, peak](std::int64_t v) {
            return h - (static_cast<float>(v) / static_cast<float>(peak)) *
                           (h - 8.0F) - 4.0F;  // 上下各留 4pt 呼吸
        };
        const std::size_t n = history.size();
        const float dx = n > 1 ? w / static_cast<float>(n - 1) : 0.0F;

        // 下载：路径面积填充 + 顶线描边。
        if (n >= 2) {
            huxerui::Path area;
            area.MoveTo({0.0F, yOf(history.front().down)});
            for (std::size_t i = 1; i < n; ++i) {
                area.LineTo({static_cast<float>(i) * dx, yOf(history[i].down)});
            }
            huxerui::Path line = area;  // 顶线单独描边
            area.LineTo({w, h});
            area.LineTo({0.0F, h});
            area.Close();
            huxerui::Color fill = downColor;
            fill.alpha = 0.18F;
            paint.FillPath(area, fill);
            paint.StrokePath(line, downColor,
                             huxerui::StrokeStyle{.width = 2.0F});

            huxerui::Path upLine;
            upLine.MoveTo({0.0F, yOf(history.front().up)});
            for (std::size_t i = 1; i < n; ++i) {
                upLine.LineTo({static_cast<float>(i) * dx, yOf(history[i].up)});
            }
            paint.StrokePath(upLine, upColor,
                             huxerui::StrokeStyle{.width = 1.5F});
        } else {
            paint.DrawLine({0.0F, h - 4.0F}, {w, h - 4.0F}, downColor,
                           huxerui::StrokeStyle{.width = 1.5F});
        }
    };
}

#if defined(__ANDROID__)

[[huxerui::composable]] huxerui::View AndroidHomeSystemCard(const HomeState&) {
    return {};
}

[[huxerui::composable]] huxerui::View AndroidHomeFloatingAction(
    huxerui::View page, const HomeState& state, bool compact) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto busy = huxerui::UseState(false);
    auto activeOverride = huxerui::UseState<std::optional<bool>>(std::nullopt);
    // composable 形参被 codegen 固定为 const：拷贝到局部再走右值链。
    huxerui::View base = page;
    if (!compact) return base;

    const int vpnState = AndroidVpnState();
    const bool realActive = vpnState == 1 || vpnState == 2;
    const bool active = activeOverride.Get().value_or(realActive);
    const bool canStart = state.profileId != 0;
    const bool enabled = !busy.Get() && (active || canStart);
    const auto toggle = [tasks, toast, busy, activeOverride, realActive] {
        if (busy.Get()) return;
        activeOverride = !realActive;
        busy = true;
        tasks.Launch([toast, busy, activeOverride,
                      realActive]() -> huxerui::Task<void> {
            try {
                co_await RunOnTaskThread([realActive] {
                    auto& core = store::coreStore();
                    core.setSetting("core.tun_enabled",
                                    realActive ? "false" : "true");
                    if (realActive) {
                        AndroidStopVpn();
                        WaitForAndroidVpnStopped();
                    } else {
                        core.startCore(store::profilesStore().selectedYaml());
                        AndroidStartVpn();
                    }
                });
                activeOverride = std::nullopt;
                toast.Show(realActive ? "VPN 隧道已关闭" : "正在启动 VPN 隧道");
            } catch (const std::exception& error) {
                activeOverride = std::nullopt;
                toast.Show(error.what());
            }
            busy = false;
        });
    };

    // 三角（play）启动、双竖线（pause）暂停；与代理页悬浮测速按钮共用
    // 固定悬浮层定位（主轴末端 + 交叉轴末端，避开底部悬浮导航）。
    huxerui::View floating =
        huxerui::IconButton(active ? app::images::pause : app::images::play,
                            active ? "停止 VPN" : "启动 VPN")
            .OnClick(toggle)
            .With(huxerui::Tooltip(active ? "停止 VPN" : "启动 VPN"),
                  huxerui::Frame{.width = 56.0F, .height = 56.0F},
                  huxerui::Enabled(enabled),
                  huxerui::Background(active ? theme.colors.primary_container
                                             : theme.colors.primary),
                  huxerui::Foreground(active
                                          ? theme.colors.on_primary_container
                                          : theme.colors.on_primary),
                  huxerui::CornerRadius(28.0F),
                  huxerui::Shadow{huxerui::Color::Rgb(0, 0, 0, 0.28F), {}, 14.0F,
                                  2.0F},
                  huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                     .label = active ? "停止 VPN" : "启动 VPN"});
    // 与 Android 底部悬浮导航栏相同：按钮放在独立的全屏覆盖层中，
    // 由覆盖层的 Column 在主轴末端、交叉轴末端定位，不依赖页面内容容器。
    huxerui::View dock = huxerui::Column {
        std::move(floating),
    }.With(huxerui::Padding(huxerui::EdgeInsets{
                 .right = theme.spacing.medium,
                 .bottom = kCompactFloatingNavigationInset,
                 .left = theme.spacing.medium,
             }),
             huxerui::MainAlign(huxerui::MainAxisAlignment::End),
             huxerui::CrossAlign(huxerui::CrossAxisAlignment::End));
    return huxerui::Stack {
        std::move(base),
        std::move(dock),
    }.With(huxerui::Grow(1.0F),
           huxerui::Align(huxerui::HorizontalAlignment::Stretch,
                          huxerui::VerticalAlignment::Stretch));
}

#else

[[huxerui::composable]] huxerui::View DesktopHomeAction(const HomeState&) {
    return {};
}

// 桌面首页没有 VPN 启动动作，悬浮层退回页面本身；保留同名平台函数是为了
// 让 HomePage 只经由宏选择一个完整函数，而不在通用页面里写平台分支。
[[huxerui::composable]] huxerui::View DesktopHomeFloatingAction(
    huxerui::View page, const HomeState&, bool) {
    huxerui::View result = page;
    return result;
}

// 桌面快捷开关自洽管理自己的任务、权限引导和乐观状态；Android 不会进入
// 这个函数，所以不会从首页漏出系统代理/TUN 控件。
[[huxerui::composable]] huxerui::View DesktopHomeSystemCard(
    const HomeState& state) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const huxerui::ApplicationHandle application = huxerui::UseApplication();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto dialog = huxerui::UseDialog();
    auto clipboard = application.Clipboard();
    auto proxy_override = huxerui::UseState<std::optional<bool>>(std::nullopt);
    auto tun_override = huxerui::UseState<std::optional<bool>>(std::nullopt);
    const huxerui::Color text_color = theme.colors.on_surface;
    const huxerui::Color hint_color = theme.colors.on_surface_variant;

    return Card(huxerui::Column {
        huxerui::Text("系统").Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kBody)
                .WithWeight(huxerui::FontWeight::SemiBold),
            text_color}),
        SettingSwitchRow(
            "系统代理", "为桌面应用设置系统代理",
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
                        }
                    });
                })),
        SettingSwitchRow(
            "TUN 模式", "全局透明代理（需管理员权限）",
            huxerui::Switch(tun_override.Get().value_or(state.core.tunEnabled))
                .OnChanged([state, tasks, toast, dialog, clipboard, text_color,
                            hint_color, tun_override](bool on) {
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
                                                   text_color, hint_color);
                                co_return;
                            }
                        }
                        const bool ok = co_await RunOnTaskThread(
                            [on] { return store::coreStore().applyTun(on); });
                        tun_override = std::nullopt;
                        if (!ok) {
                            const std::string error =
                                store::coreStore().snapshot().lastError;
                            toast.Show(error.empty() ? "TUN 设置失败" : error);
                        }
                    });
                })),
    }.With(huxerui::Spacing(10.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
}

#endif

} // namespace

[[huxerui::composable]] huxerui::View HomePage(
    huxerui::State<std::size_t> navPage) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto state = huxerui::UseState<HomeState>({});
    // 乐观开关：点击立即翻转显示，后台完成后清除覆盖（真实状态接管），
    // 失败自动回弹并提示。覆盖值非空即“进行中”，期间忽略再次点击，
    // 避免 TUN 重启内核期间的并发 stop/start。
    auto modeOverride = huxerui::UseState<std::optional<std::size_t>>(std::nullopt);

    huxerui::Lifecycle(
        [tasks, state] {
            tasks.Launch([state]() -> huxerui::Task<void> {
                co_await PollWhile(std::chrono::duration<double>{0.5}, [state] {
                    auto& core = store::coreStore();
                    HomeState s = state.Get();
                    updateRuntime(s, core);

                    if (const auto p = store::profilesStore().selected()) {
                        s.profileId = p->id;
                        s.profileName = p->name;
                        s.profileUsedBytes = p->usedBytes;
                        s.profileTotalBytes = p->totalBytes;
                        s.profileUpdated = p->updatedAt > 0
                                               ? "更新于 " + formatTime(p->updatedAt)
                                               : "未拉取";
                        if (!p->error.empty()) s.profileUpdated = "拉取失败";
                    } else {
                        s.profileId = 0;
                        s.profileName = "未启用订阅";
                        s.profileUpdated = "";
                        s.profileUsedBytes = 0;
                        s.profileTotalBytes = 0;
                        s.proxyGroups.clear();
                    }

                    state = s;
                    return true;
                });
            });
            tasks.Launch([state]() -> huxerui::Task<void> {
                for (;;) {
                    const std::string body =
                        co_await RunOnTaskThread([] { return ProxyGroupsSnapshot(); });
                    HomeState s = state.Get();
                    s.proxyGroups = ParseProxyGroups(body);
                    state = s;
                    co_await huxerui::Delay(std::chrono::duration<double>{2.0});
                }
            });
            return [] {};
        },
        0);

    const HomeState s = state.Get();
    const bool running = s.core.state == core::CoreState::Running;
    const huxerui::Color upColor = huxerui::Color::Rgb(245, 158, 11);   // 琥珀
    const huxerui::Color downColor = theme.colors.primary;
    // 流量主岛顶部品牌蓝光晕的起止色（primary 8% → 全透明）。
    huxerui::Color heroGlowTop = theme.colors.primary;
    heroGlowTop.alpha = 0.08F;
    huxerui::Color heroGlowBottom = theme.colors.primary;
    heroGlowBottom.alpha = 0.0F;

    std::size_t modeIndex = 0;
    for (std::size_t i = 0; i < kModes.size(); ++i) {
        if (s.core.mode == kModes[i]) modeIndex = i;
    }

    // 响应式：Compact 视口的统计并入启动卡片（metric 列）；桌面端是一条
    // 统计带——一张卡内 4 列指标，列间竖向 Divider 分隔，不再并排多张小卡。
    huxerui::View statCards = compact
        ? huxerui::View{}
        : huxerui::View{Card(huxerui::Row {
                  StatMetric("下载速率", formatRate(s.latest.down), downColor)
                      .With(huxerui::Grow(1.0F)),
                  huxerui::Divider(huxerui::Axis::Vertical),
                  StatMetric("上传速率", formatRate(s.latest.up), upColor)
                      .With(huxerui::Grow(1.0F)),
                  huxerui::Divider(huxerui::Axis::Vertical),
                  StatMetric("总下载", formatBytes(s.totalDown),
                             theme.colors.on_surface)
                      .With(huxerui::Grow(1.0F)),
                  huxerui::Divider(huxerui::Axis::Vertical),
                  StatMetric("总上传", formatBytes(s.totalUp),
                             theme.colors.on_surface)
                      .With(huxerui::Grow(1.0F)),
              }.With(huxerui::Spacing(16.0F),
                     huxerui::CrossAlign(
                         huxerui::CrossAxisAlignment::Stretch)))};

    // 出站模式 / 当前订阅两卡（Compact 视口竖排，见下方布局分支）。
    const std::size_t displayedModeIndex = modeOverride.Get().value_or(modeIndex);
    huxerui::View modeCard = Card(huxerui::Column {
        huxerui::Text("出站模式").Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kBody)
                .WithWeight(huxerui::FontWeight::SemiBold),
            theme.colors.on_surface}),
        huxerui::SegmentedButton(kModeNames, displayedModeIndex)
            .OnChanged([tasks, toast, modeOverride](std::size_t idx) {
                if (!BeginOptimistic(modeOverride, idx)) return;
                tasks.Launch([=]() -> huxerui::Task<void> {
                    const bool ok = co_await RunOnTaskThread(
                        [idx] {
                            return store::coreStore().applyMode(kModes[idx]);
                        });
                    EndOptimistic(modeOverride);
                    if (!ok) toast.Show("切换失败（内核未运行？）");
                });
            }),
    }.With(huxerui::Spacing(10.0F)))
                                 .With(huxerui::Grow(1.0F));

    huxerui::View profileBody = huxerui::Column {
        huxerui::Text("当前订阅").Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kBody)
                .WithWeight(huxerui::FontWeight::SemiBold),
            theme.colors.on_surface}),
        huxerui::Text(s.profileName).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kBody), theme.colors.on_surface}),
        s.profileUpdated.empty()
            ? huxerui::View{huxerui::Row{}}
            : huxerui::View{
                  huxerui::Text(s.profileUpdated)
                      .Style(huxerui::TextStyle{
                          huxerui::Font::System(font_size::kCaption),
                          theme.colors.on_surface_variant})},
    }.With(huxerui::Spacing(4.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Start));
    huxerui::View profileCard = Card(huxerui::Stack{
        std::move(profileBody),
        s.profileId != 0
            ? huxerui::View{huxerui::ProgressBar(s.profileTotalBytes > 0
                                                     ? std::clamp(
                                                           static_cast<float>(s.profileUsedBytes) /
                                                               static_cast<float>(s.profileTotalBytes),
                                                           0.0F, 1.0F)
                                                     : 1.0F)
                                .With(huxerui::Frame{.height = 3.0F},
                                      // Stack 用 Align 定位子项：进度条贴住
                                      // 卡片下边缘（原来贴在顶部）。
                                      huxerui::Align(
                                          huxerui::HorizontalAlignment::Stretch,
                                          huxerui::VerticalAlignment::End))}
            : huxerui::View{huxerui::Row{}},
        // 桌面端同一行的卡片会被拉伸到等高，撑满卡片高度后进度条才真正
        // 落在下边缘而不是内容中部。
    }.With(huxerui::Grow(1.0F)));
    profileCard = std::move(profileCard).With(huxerui::Grow(1.0F));
    huxerui::View systemCard = CLASHFLUX_HOME_SYSTEM_CARD(s).With(
        huxerui::Grow(1.0F));

    huxerui::View page = PageScaffold(
        "首页",
        CLASHFLUX_HOME_ACTION(s),
        huxerui::ScrollView(
            huxerui::Column {
                // 速率统计卡
                std::move(statCards),

                // 流量曲线：自上而下叠一层品牌蓝光晕，强调色落在主岛
                // 而不是铺满背景。
                Card(huxerui::Column {
                    huxerui::Row {
                        huxerui::Text("流量").Style(huxerui::TextStyle{
                            huxerui::Font::System(font_size::kBody)
                                .WithWeight(huxerui::FontWeight::SemiBold),
                            theme.colors.on_surface}),
                        huxerui::Spacer(),
                        huxerui::Text("— 下载").Style(huxerui::TextStyle{
                            huxerui::Font::System(font_size::kCaption),
                            downColor}),
                        huxerui::Text("— 上传").Style(huxerui::TextStyle{
                            huxerui::Font::System(font_size::kCaption),
                            upColor}),
                    }.With(huxerui::Spacing(12.0F)),
                    huxerui::Canvas(TrafficPainter(
                                        s.history, downColor, upColor,
                                        theme.colors.outline))
                        .With(huxerui::Frame{.height = 160.0F}),
                }.With(huxerui::Spacing(10.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                       huxerui::Background(huxerui::LinearGradient{
                           .start = {0.0F, 0.0F},
                           .end = {0.0F, 1.0F},
                           .stops = {{0.0F, heroGlowTop},
                                     {1.0F, heroGlowBottom}},
                       }))),

                // 出站模式 + 当前订阅 + 系统开关（Compact 竖排）
                compact
                    ? huxerui::View{huxerui::Column {
                          std::move(modeCard),
                          std::move(profileCard),
                          std::move(systemCard),
                      }.With(huxerui::Spacing(10.0F),
                             huxerui::CrossAlign(
                                 huxerui::CrossAxisAlignment::Stretch))}
                    : huxerui::View{huxerui::Row {
                          std::move(modeCard),
                          std::move(profileCard),
                          std::move(systemCard),
                      }.With(huxerui::Spacing(10.0F),
                             huxerui::CrossAlign(
                                 huxerui::CrossAxisAlignment::Stretch))},

                // 内核状态
                Card(huxerui::Row {
                    huxerui::Text("内核").Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kBody)
                            .WithWeight(huxerui::FontWeight::SemiBold),
                        theme.colors.on_surface}),
                    huxerui::Spacer(),
                    huxerui::Text(
                        running
                            ? "运行中 · " +
                                  (s.core.version.empty() ? std::string{kDefaultCoreName}
                                                          : s.core.version)
                            : core::stateName(s.core.state))
                        .Style(huxerui::TextStyle{
                            huxerui::Font::System(font_size::kChip),
                            running ? downColor
                                    : theme.colors.on_surface_variant}),
                }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))),
                compact ? CompactFloatingNavigationFooter() : huxerui::View{},
            }.With(huxerui::Spacing(12.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
            .With(huxerui::Grow(1.0F)));

    // 移动端启动/停止按钮脱离滚动内容，固定在底部悬浮导航之上的位置。
    return CLASHFLUX_HOME_FLOATING_ACTION(std::move(page), s, compact);
}

#undef CLASHFLUX_HOME_SYSTEM_CARD
#undef CLASHFLUX_HOME_ACTION
#undef CLASHFLUX_HOME_FLOATING_ACTION

} // namespace clashflux::ui
