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

struct HomeState {
    stream::TrafficPoint latest;
    std::vector<stream::TrafficPoint> history;  // 旧→新
    std::int64_t totalUp = 0;
    std::int64_t totalDown = 0;
    store::CoreSnapshot core;
    std::string profileName;
    std::string profileUpdated;

    bool operator==(const HomeState&) const = default;
};

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

} // namespace

[[huxerui::composable]] huxerui::View HomePage() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto dialog = huxerui::UseDialog();
    auto state = huxerui::UseState<HomeState>({});

    huxerui::Lifecycle(
        [tasks, state] {
            tasks.Launch([state]() -> huxerui::Task<void> {
                co_await PollWhile(std::chrono::duration<double>{0.5}, [state] {
                    auto& core = store::coreStore();
                    HomeState s = state.Get();
                    s.core = core.snapshot();

                    stream::TrafficPoint point;
                    if (core.streams().takeTraffic(point)) {
                        s.latest = point;
                        s.history.push_back(point);
                        if (s.history.size() > kHistoryPoints) {
                            s.history.erase(s.history.begin());
                        }
                    }
                    if (s.core.state != core::CoreState::Running &&
                        !s.history.empty()) {
                        s.history.clear();
                        s.latest = {};
                    }

                    std::string frame;
                    if (core.streams().takeConnections(frame)) {
                        const auto j = nlohmann::json::parse(frame, nullptr,
                                                             false);
                        if (j.is_object()) {
                            s.totalUp = j.value("uploadTotal", std::int64_t{0});
                            s.totalDown = j.value("downloadTotal",
                                                  std::int64_t{0});
                        }
                    }

                    if (const auto p = store::profilesStore().selected()) {
                        s.profileName = p->name;
                        s.profileUpdated = p->updatedAt > 0
                                               ? "更新于 " + formatTime(p->updatedAt)
                                               : "未拉取";
                        if (!p->error.empty()) s.profileUpdated = "拉取失败";
                    } else {
                        s.profileName = "未启用订阅";
                        s.profileUpdated = "";
                    }

                    state = s;
                    return true;
                });
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
    huxerui::View statCards =
        compact
            ? huxerui::View{huxerui::Column {
                  huxerui::Row {
                      StatCard("下载速率", formatRate(s.latest.down), downColor)
                          .With(huxerui::Grow(1.0F)),
                      StatCard("上传速率", formatRate(s.latest.up), upColor)
                          .With(huxerui::Grow(1.0F)),
                  }.With(huxerui::Spacing(10.0F),
                         huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
                  huxerui::Row {
                      StatCard("总下载", formatBytes(s.totalDown),
                               theme.colors.on_surface)
                          .With(huxerui::Grow(1.0F)),
                      StatCard("总上传", formatBytes(s.totalUp),
                               theme.colors.on_surface)
                          .With(huxerui::Grow(1.0F)),
                  }.With(huxerui::Spacing(10.0F),
                         huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
              }.With(huxerui::Spacing(10.0F),
                     huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))}
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
    huxerui::View modeCard = Card(huxerui::Column {
        huxerui::Text("出站模式").Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kBody)
                .WithWeight(huxerui::FontWeight::SemiBold),
            theme.colors.on_surface}),
        huxerui::SegmentedButton(kModeNames, modeIndex)
            .OnChanged([tasks, toast](std::size_t idx) {
                tasks.Launch([=]() -> huxerui::Task<void> {
                    const bool ok = co_await RunOnTaskThread(
                        [idx] {
                            return store::coreStore().applyMode(kModes[idx]);
                        });
                    if (!ok) toast.Show("切换失败（内核未运行？）");
                });
            }),
    }.With(huxerui::Spacing(10.0F)))
                                 .With(huxerui::Grow(1.0F));
    huxerui::View profileCard = Card(huxerui::Column {
        huxerui::Text("当前订阅").Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kBody)
                .WithWeight(huxerui::FontWeight::SemiBold),
            theme.colors.on_surface}),
        huxerui::Text(s.profileName)
            .Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody),
                theme.colors.on_surface}),
        s.profileUpdated.empty()
            ? huxerui::View{huxerui::Row{}}
            : huxerui::View{
                  huxerui::Text(s.profileUpdated)
                      .Style(huxerui::TextStyle{
                          huxerui::Font::System(font_size::kCaption),
                          theme.colors.on_surface_variant})},
    }.With(huxerui::Spacing(4.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Start)))
                                    .With(huxerui::Grow(1.0F));
    // 系统快捷开关卡：系统代理 / TUN 模式（与设置页「系统」卡同一 store 通道）。
    // 系统代理开关按桌面环境支持性禁用；写失败由 0.5s 泵带回真实状态。
    huxerui::View systemCard = Card(huxerui::Column {
        huxerui::Text("系统").Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kBody)
                .WithWeight(huxerui::FontWeight::SemiBold),
            theme.colors.on_surface}),
        huxerui::Row {
            huxerui::Text("系统代理").Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody),
                theme.colors.on_surface}),
            huxerui::Spacer(),
            huxerui::Switch(store::coreStore().systemProxyEnabled())
                .OnChanged([tasks, toast](bool on) {
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        const bool ok = co_await RunOnTaskThread(
                            [on] { return store::coreStore().applySystemProxy(on); });
                        if (!ok) {
                            const std::string err =
                                store::coreStore().snapshot().lastError;
                            toast.Show(err.empty() ? "系统代理设置失败" : err);
                        }
                    });
                })
                .With(huxerui::Enabled(store::coreStore().systemProxySupported())),
        }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Row {
            huxerui::Text("TUN 模式").Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody),
                theme.colors.on_surface}),
            huxerui::Spacer(),
            huxerui::Switch(s.core.tunEnabled)
                .OnChanged([tasks, toast, dialog,
                            textColor = theme.colors.on_surface,
                            hintColor =
                                theme.colors.on_surface_variant](bool on) {
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        if (on) {
                            // 门禁/弹窗会卸载点击路径：先让出一拍（约定 4/6）。
                            co_await huxerui::Delay(
                                std::chrono::duration<double>{0});
                            const core::TunGate gate = co_await RunOnTaskThread(
                                [] { return core::tunGate(); });
                            if (gate == core::TunGate::Elevated) {
                                toast.Show("已请求管理员权限重启，请在新窗口开启"
                                           " TUN");
                                co_return;
                            }
                            if (gate == core::TunGate::Denied) {
                                ShowTunGuideDialog(dialog, textColor, hintColor);
                                co_return;
                            }
                        }
                        const bool ok = co_await RunOnTaskThread(
                            [on] { return store::coreStore().applyTun(on); });
                        if (!ok) {
                            const std::string err =
                                store::coreStore().snapshot().lastError;
                            toast.Show(err.empty() ? "TUN 设置失败" : err);
                        }
                    });
                }),
        }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
    }.With(huxerui::Spacing(10.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
                                    .With(huxerui::Grow(1.0F));

    return PageScaffold(
        "首页",
        huxerui::Row{},
        huxerui::ScrollView(
            huxerui::Column {
                // 速率统计卡
                std::move(statCards),

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
                                  (s.core.version.empty() ? "mihomo"
                                                          : s.core.version)
                            : core::stateName(s.core.state))
                        .Style(huxerui::TextStyle{
                            huxerui::Font::System(font_size::kChip),
                            running ? downColor
                                    : theme.colors.on_surface_variant}),
                }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))),
            }.With(huxerui::Spacing(12.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
            .With(huxerui::Grow(1.0F)));
}

} // namespace clashflux::ui
