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
#define CLASHFLUX_HOME_INLINE_ACTION(state) AndroidHomeAction(state),
#define CLASHFLUX_HOME_NAV_ACTIONS(state) AndroidHomeNavigationActions(state),
#else
constexpr std::string_view kDefaultCoreName = "sing-box";
#define CLASHFLUX_HOME_SYSTEM_CARD DesktopHomeSystemCard
#define CLASHFLUX_HOME_ACTION(state) DesktopHomeAction(state)
#define CLASHFLUX_HOME_INLINE_ACTION(state)
#define CLASHFLUX_HOME_NAV_ACTIONS(state)
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
[[huxerui::composable]] huxerui::View StatCard(const std::string& label,
                                               const std::string& value,
                                               huxerui::Color valueColor) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return Card(huxerui::Column {
        huxerui::Text(label).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kCaption),
            theme.colors.on_surface_variant}),
        huxerui::Text(value).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kTitle)
                .WithWeight(huxerui::FontWeight::Bold),
            valueColor}),
    }.With(huxerui::Spacing(4.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Start)));
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

[[huxerui::composable]] huxerui::View AndroidHomeNavigationActions(
    huxerui::State<std::size_t> navPage) {
    return huxerui::Column {
        Card(huxerui::Row {
                 huxerui::Text("选择订阅"),
                 huxerui::Spacer(),
                 huxerui::Text("›"),
             }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)))
            .OnClick([navPage] { navPage = 1; })
            .With(huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                     .label = "选择订阅"},
                  huxerui::Focusable(true), huxerui::Enabled(true)),
        Card(huxerui::Row {
                 huxerui::Text("选择节点"),
                 huxerui::Spacer(),
                 huxerui::Text("›"),
             }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)))
            .OnClick([navPage] { navPage = 2; })
            .With(huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                     .label = "选择节点"},
                  huxerui::Focusable(true), huxerui::Enabled(true)),
    }.With(huxerui::Spacing(10.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View AndroidHomeAction(const HomeState& state) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto busy = huxerui::UseState(false);
    const int vpnState = AndroidVpnState();
    const bool active = vpnState == 1 || vpnState == 2;
    const bool canStart = state.profileId != 0;
    const bool enabled = !busy.Get() && (active || canStart);
    const auto toggle = [tasks, toast, busy, active] {
            if (busy.Get()) return;
            busy = true;
            tasks.Launch([toast, busy, active]() -> huxerui::Task<void> {
                try {
                    co_await RunOnTaskThread([active] {
                        auto& core = store::coreStore();
                        core.setSetting("core.tun_enabled", active ? "false" : "true");
                        if (active) {
                            AndroidStopVpn();
                        } else {
                            core.startCore(store::profilesStore().selectedYaml());
                            AndroidStartVpn();
                        }
                    });
                    toast.Show(active ? "VPN 隧道已关闭" : "正在启动 VPN 隧道");
                } catch (const std::exception& error) {
                    toast.Show(error.what());
                }
                busy = false;
            });
        };

    // 移动端操作区随首页内容一起滚动，不再占据页面顶部的固定操作栏。
    // 启动后以主题主色填充图标按钮，提供清晰的隧道已启动反馈。
    huxerui::View center = huxerui::IconButton(
                              active ? app::images::speed : app::images::bolt,
                              active ? "停止 VPN" : "启动 VPN")
        .OnClick(toggle)
        .With(huxerui::Frame{.width = 76.0F, .height = 76.0F},
              huxerui::Enabled(enabled),
              huxerui::Background(active ? theme.colors.primary
                                         : huxerui::Color::Transparent()),
              huxerui::Foreground(active ? theme.colors.on_primary
                                         : theme.colors.primary),
              huxerui::CornerRadius(38.0F),
              huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                 .label = active ? "停止 VPN" : "启动 VPN"});
    if (!active) {
        return Card(huxerui::Row { std::move(center) }
                        .With(huxerui::Frame{.height = 92.0F},
                              huxerui::MainAlign(
                                  huxerui::MainAxisAlignment::Center),
                              huxerui::CrossAlign(
                                  huxerui::CrossAxisAlignment::Center)));
    }

    const huxerui::Color downColor = theme.colors.primary;
    const huxerui::Color upColor = huxerui::Color::Rgb(245, 158, 11);
    auto metric = [&](std::string_view title, const std::string& value,
                      huxerui::Color color) {
        return huxerui::Column{
            huxerui::Text(std::string(title)).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption),
                active ? theme.colors.on_primary_container
                       : theme.colors.on_surface_variant}),
            huxerui::Text(value).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kChip)
                    .WithWeight(huxerui::FontWeight::Bold),
                active ? theme.colors.on_primary_container : color}),
        }.With(huxerui::Spacing(3.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    };
    const auto left = huxerui::Column{
        metric("下载速率", formatRate(state.latest.down), downColor),
        metric("总下载", formatBytes(state.totalDown), theme.colors.on_surface),
    }.With(huxerui::Spacing(14.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
                       .With(huxerui::Grow(1.0F));
    const auto right = huxerui::Column{
        metric("上传速率", formatRate(state.latest.up), upColor),
        metric("总上传", formatBytes(state.totalUp), theme.colors.on_surface),
    }.With(huxerui::Spacing(14.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
                        .With(huxerui::Grow(1.0F));
    return Card(huxerui::Row { left, std::move(center), right }
                    .With(huxerui::Frame{.height = 92.0F},
                          huxerui::Spacing(10.0F),
                          huxerui::CrossAlign(
                              huxerui::CrossAxisAlignment::Center)))
        .With(huxerui::Background(active ? theme.colors.primary_container
                                         : ResolveIslandTheme(theme).raised),
              huxerui::Border{active ? theme.colors.primary
                                     : huxerui::Color::Transparent(),
                               active ? 1.5F : 0.0F},
              huxerui::Shadow{.color = active
                                           ? huxerui::Color{
                                                 theme.colors.primary.red,
                                                 theme.colors.primary.green,
                                                 theme.colors.primary.blue, 0.55F}
                                           : huxerui::Color::Transparent(),
                               .blur_radius = active ? 18.0F : 0.0F,
                               .spread = active ? 2.0F : 0.0F});
}

#else

[[huxerui::composable]] huxerui::View DesktopHomeAction(const HomeState&) {
    return {};
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

    std::size_t modeIndex = 0;
    for (std::size_t i = 0; i < kModes.size(); ++i) {
        if (s.core.mode == kModes[i]) modeIndex = i;
    }

    // 响应式：Compact 视口统计卡 2×2 网格、模式/订阅双卡竖排。
    huxerui::View statCards = compact
        ? huxerui::View{}
        : huxerui::View{huxerui::Row {
                  StatCard("下载速率", formatRate(s.latest.down), downColor)
                      .With(huxerui::Grow(1.0F)),
                  StatCard("上传速率", formatRate(s.latest.up), upColor)
                      .With(huxerui::Grow(1.0F)),
                  StatCard("总下载", formatBytes(s.totalDown),
                           theme.colors.on_surface)
                      .With(huxerui::Grow(1.0F)),
                  StatCard("总上传", formatBytes(s.totalUp),
                           theme.colors.on_surface)
                      .With(huxerui::Grow(1.0F)),
              }.With(huxerui::Spacing(10.0F),
                     huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))};

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
                                      huxerui::MainAlign(huxerui::MainAxisAlignment::End),
                                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))}
            : huxerui::View{huxerui::Row{}}});
    profileCard = std::move(profileCard).With(huxerui::Grow(1.0F));
    huxerui::View systemCard = CLASHFLUX_HOME_SYSTEM_CARD(s).With(
        huxerui::Grow(1.0F));

    return PageScaffold(
        "首页",
        CLASHFLUX_HOME_ACTION(s),
        huxerui::ScrollView(
            huxerui::Column {
                // 速率统计卡
                std::move(statCards),

                // Android 启动控件与页面内容同处滚动流，不单独冻结在标题下方。
                CLASHFLUX_HOME_INLINE_ACTION(s)

                // 移动端导航动作独立成卡片，紧跟启动卡片。
                CLASHFLUX_HOME_NAV_ACTIONS(navPage)

                // 流量曲线
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
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),

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
}

#undef CLASHFLUX_HOME_SYSTEM_CARD
#undef CLASHFLUX_HOME_ACTION
#undef CLASHFLUX_HOME_INLINE_ACTION
#undef CLASHFLUX_HOME_NAV_ACTIONS

} // namespace clashflux::ui
