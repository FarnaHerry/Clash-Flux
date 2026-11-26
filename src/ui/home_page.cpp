// home_page.cpp — 固定网格首页：卡片目录 + 网格摆放（宽/高各 1..4 单位）。
//
// 结构：
//   kHomeCards   文件作用域 #if 选出的平台基础卡片表（composable 体内禁止条件编译，
//                平台差异只出现在这里和平台函数选择宏上）；运行中的桌面控制卡追加在尾部；
//   HomeGrid     把「页面逻辑宽度」分成 2..5 列，按卡片声明的宽高（HomeCardSpan）
//                做左上紧凑的二维打包；
//   HomeCardContent 按种类组装卡片内容，平台专属卡片经宏选择完整函数。
// 基础布局是编译期常量：没有编辑态、拖动排序、用户增删卡片或 home.layout.* 持久化。
// 桌面控制卡由实际内核状态决定是否在尾部出现。
//
// 数据流：UI 泵每 500ms 从 CoreStreams 取最新流量帧追加进 60 点环形历史
// （State<vector<TrafficPoint>>），连接快照帧只取总量字段；内核状态与启用
// 订阅每拍重读。Canvas 画家捕获历史快照，重组后按最新序列重绘。
#include <huxerui/huxerui.h>
#include <huxerui/charts.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app_resources.h"
#include "ui.h"
#include "../network_settings.h"
#include "app_http_client.h"
#include "box_cat.h"
#include "home_control_motion.h"
#include "home_header_control.h"
#include "page_layout.h"
#include "country_flag.h"
#include "home_grid.h"
#include "home_card.h"
#include "home_easter_eggs.h"
#include "home_mode_choices.h"
#include "empty_state.h"
#include "home_network_model.h"
#include "home_proxy_picker.h"
#include "profile_card_layout.h"
#include "public_ip_lookup.h"
#include "task_bridge.h"

import clashflux.api;
import clashflux.core;
import clashflux.data;
import clashflux.model;
import clashflux.netinfo;
import clashflux.stream;
import clashflux.store.core;
import clashflux.store.profiles;
import clashflux.utils;

// CoreView 含 store::CoreSnapshot、ProfilesModel 含 model::Profile，必须在模块导入
// 之后（同 profiles_cache.h）。
#include "core_model.h"
#include "core_control.h"
#include "home_vpn_status.h"
#include "profiles_model.h"
#include "proxies_model.h"
#include "settings_model.h"
#include "stream_updates.h"
#include "wire_codec.h"

namespace clashflux::ui {
namespace {

constexpr std::size_t kHistoryPoints = 60;  // 60 拍 ≈ 30s 窗口

const std::vector<std::string> kModeLabels{"规则", "全局", "直连"};
const std::vector<std::string> kModes{"rule", "global", "direct"};

std::size_t HomeModeIndex(const std::string& mode) {
    for (std::size_t index = 0; index < kModes.size(); ++index) {
        if (mode == kModes[index]) return index;
    }
    return 0;
}

// ---- 卡片种类与平台目录 ----------------------------------------------------

#if defined(__ANDROID__)
// 移动端的 VPN 猫咪在启动后并入固定页内标题栏。
// 手机屏是 2 列：需要整宽的（图表）给 2 格，信息/单选卡 1 格；
// 高度仍按 FlClash 跨度（图表/信息卡/出站模式 2 行，开关与网络信息卡 1 行）。
constexpr auto& kHomeCards = kMobileHomeCards;
constexpr bool kDesktopHomeControl = false;
#define CLASHFLUX_HOME_PLATFORM_CARD(homeState, state, kind) \
    huxerui::Row{}
#define CLASHFLUX_HOME_CONTROL_CARD(shown, motion, transition) huxerui::Row{}
#define CLASHFLUX_HOME_FLOATING_ACTION(page, state, compact, shown, motion, ready, transition) \
    page
#else
// 桌面可选卡片：系统代理/TUN 开关是桌面专有能力。
//
// 初始宽高按 FlClash 仪表盘实测跨度（其 3 列网格：单元 264x79.5、间距 14 逻辑 px）：
//   下载和上传各 1x2               流量统计 1x2
//   系统代理 1x1 → 系统代理 1x1   TUN 1x1      → 虚拟网卡 1x1
//   出站模式(竖排) 1x2 → 出站模式取 1x2（纵向单选列表）
//   其余信息类卡片（当前订阅）与 FlClash 的信息卡同级，取 1x2。
constexpr auto& kHomeCards = kDesktopHomeCards;
constexpr bool kDesktopHomeControl = true;
#define CLASHFLUX_HOME_PLATFORM_CARD(homeState, state, kind) \
    DesktopHomePlatformCard(kind)
#define CLASHFLUX_HOME_CONTROL_CARD(shown, motion, transition) \
    DesktopHomeStatusCard(shown, motion, transition)
#define CLASHFLUX_HOME_FLOATING_ACTION(page, state, compact, shown, motion, ready, transition) \
    DesktopHomeFloatingAction(std::move(page), shown, motion, ready, transition)
#endif

// ---- 卡片网格尺寸模型 ------------------------------------------------------

// 卡片声明的宽高（单位格，各 1..4）。宽会被页面当前列数收窄，高不随页面变化。
struct HomeCardEntry {
    HomeCardKind kind = HomeCardKind::Download;
    HomeCardSize size;

    bool operator==(const HomeCardEntry&) const = default;
};

using HomeLayout = std::vector<HomeCardEntry>;

// 首页右下角悬浮启动按钮的占位高度（滚动内容尾部留白）。
constexpr float kHomeFloatingButtonInset = 72.0F;

struct HomeState {
    bool coreReady = false;
    bool profilesReady = false;
    stream::TrafficPoint latest;
    std::vector<stream::TrafficPoint> history;  // 旧→新
    std::int64_t totalUp = 0;
    std::int64_t totalDown = 0;
    store::CoreSnapshot core;
    std::int64_t profileId = 0;
    std::string profileName;
    std::int64_t profileUsedBytes = 0;
    std::int64_t profileTotalBytes = 0;
    bool operator==(const HomeState&) const = default;
};

// ---- 布局读取 --------------------------------------------------------------

const HomeCardSpec* FindHomeCard(HomeCardKind kind) {
    for (const HomeCardSpec& spec : kHomeCards) {
        if (spec.kind == kind) return &spec;
    }
    return nullptr;
}

// 首页布局是**编译期常量**（见 AGENTS.md / CLAUDE.md 第 13 条）：顺序与尺寸只由
// kHomeCards 决定，没有编辑态、拖动排序、增删卡片，也没有 home.layout.* 持久化。
HomeLayout DefaultHomeLayout() {
    HomeLayout layout;
    layout.reserve(std::size(kHomeCards));
    for (const HomeCardSpec& spec : kHomeCards) {
        layout.push_back(HomeCardEntry{spec.kind, {spec.width, spec.height}});
    }
    return layout;
}

// ---- 运行期状态泵 ----------------------------------------------------------

// Kept outside the composable body: HuxerUI's code generator deliberately
// rejects conditional compilation within a composable function.
// s.core 由调用方从 CoreModel 填好（见 core_model.h）。
void updateRuntime(HomeState& s, store::CoreStore& core) {
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
    static_cast<void>(core);
    // 内核停掉后清空折线；桌面流量/总量由推送订阅写入（见 SubscribeHomeStreams）。
    if (s.core.state != core::CoreState::Running && !s.history.empty()) {
        s.history.clear();
        s.latest = {};
    }
#endif
}

// 桌面：订阅 /traffic 与 /connections 推送，帧到达时才更新首页（不再按节拍
// 轮询槽位）。Android 没有这两个 WS 通道，速率来自 libbox 快照（updateRuntime）。
// 返回订阅 id（Android 返回 0），调用方在 Lifecycle 清理里注销。
std::uint64_t SubscribeHomeStreams(huxerui::TaskScope tasks,
                                   huxerui::State<HomeState> state) {
#if defined(__ANDROID__)
    static_cast<void>(tasks);
    static_cast<void>(state);
    return 0;
#else
    return SubscribeStreamUpdates(tasks, [state](stream::StreamKind kind) {
        if (kind != stream::StreamKind::Traffic &&
            kind != stream::StreamKind::Connections) {
            return;
        }
        auto& core = store::coreStore();
        HomeState s = state.Get();
        if (kind == stream::StreamKind::Traffic) {
            stream::TrafficPoint point;
            if (core.streams().takeTraffic(point)) {
                s.latest = point;
                s.history.push_back(point);
                if (s.history.size() > kHistoryPoints) {
                    s.history.erase(s.history.begin());
                }
            }
        } else {
            core.streams().takeConnectionTotals(s.totalUp, s.totalDown);
        }
        state = s;
    });
#endif
}

// 把模型里的运行态镜像进首页 HomeState。桌面由模型 State 变化驱动（见
// HomeRuntimePump 的桌面实现）；Android 额外需要固定节拍采样 libbox 速率
// （模型只在数值变化时通知，空闲时不会有事件），因此两处共用本函数。
void MirrorHomeState(huxerui::State<HomeState> state,
                     const std::shared_ptr<CoreModel>& coreModel,
                     const std::shared_ptr<ProfilesModel>& profilesModel) {
    auto& core = store::coreStore();
    HomeState s = state.Get();
    // 内核状态来自唯一来源 CoreModel。
    s.core = coreModel->view.Get().core;
    s.coreReady = coreModel->view.Get().ready;
    updateRuntime(s, core);

    // Hydration has not established whether a selected profile exists yet.
    if (!profilesModel->loaded.Get()) {
        state = s;
        return;
    }
    s.profilesReady = true;

    // 选中订阅来自 ProfilesModel：直接扫它的列表（借用引用，不做整表拷贝），
    // 不再每次调用 profilesStore().selected()。
    const model::Profile* selected = nullptr;
    for (const model::Profile& candidate : profilesModel->list.Get()) {
        if (candidate.selected && candidate.type != "pptp") {
            selected = &candidate;
            break;
        }
    }
    if (selected != nullptr) {
        s.profileId = selected->id;
        s.profileName = selected->name;
        s.profileUsedBytes = selected->usedBytes;
        s.profileTotalBytes = selected->totalBytes;
    } else {
        s.profileId = 0;
        s.profileName = "未启用订阅";
        s.profileUsedBytes = 0;
        s.profileTotalBytes = 0;
    }
    state = s;
}

// 首页运行态泵：平台差异由宏在文件作用域选择整份实现（composable 体内不做
// 条件编译）。
#if defined(__ANDROID__)
// Android：libbox 速率只在快照里、且模型空闲时不通知，需要固定节拍采样。
[[huxerui::composable]] huxerui::View HomeRuntimePump(
    huxerui::State<HomeState> state, std::shared_ptr<CoreModel> coreModel,
    std::shared_ptr<ProfilesModel> profilesModel) {
    auto tasks = huxerui::UseTaskScope();
    huxerui::Lifecycle(
        [tasks, state, coreModel, profilesModel] {
            tasks.Launch([state, coreModel, profilesModel]() -> huxerui::Task<void> {
                co_await PollWhile(std::chrono::duration<double>{0.5}, [=] {
                    MirrorHomeState(state, coreModel, profilesModel);
                    return true;
                });
            });
            return [] {};
        },
        0);
    return {};
}
#else
// 桌面：模型一变就镜像，没有任何定时器；流量/总量由推送订阅写入
// （见 SubscribeHomeStreams）。
[[huxerui::composable]] huxerui::View HomeRuntimePump(
    huxerui::State<HomeState> state, std::shared_ptr<CoreModel> coreModel,
    std::shared_ptr<ProfilesModel> profilesModel) {
    huxerui::Lifecycle(
        [state, coreModel, profilesModel] {
            MirrorHomeState(state, coreModel, profilesModel);
            return [] {};
        },
        coreModel->view, profilesModel->list, profilesModel->loaded);
    return {};
}
#endif
#define CLASHFLUX_HOME_RUNTIME_PUMP HomeRuntimePump

// ---- 通用卡片部件 ----------------------------------------------------------

huxerui::Font HomeCardHeadingFont() {
    return huxerui::Font::System(font_size::kBody).WithWeight(huxerui::FontWeight::SemiBold);
}

// 卡片标题：所有卡片共用同一排版；卡片外壳由卡片槽统一提供。
// 首页卡片左上角均带有契合卡片功能的语义标识（规范见 AGENTS.md）。
[[huxerui::composable]] huxerui::View HomeCardHeadingWith(
    huxerui::View leading, huxerui::StringVariant title) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Row {
        std::move(leading),
        huxerui::Text(title).LineLimit(1).Overflow(huxerui::TextOverflow::Ellipsis)
            .Style(huxerui::TextStyle{HomeCardHeadingFont(), theme.colors.on_surface}),
    }.With(huxerui::Spacing(8.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

[[huxerui::composable]] huxerui::View HomeCardHeading(
    huxerui::ImageResource icon, huxerui::StringVariant title) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return HomeCardHeadingWith(
        huxerui::Image(icon)
            .Fit(huxerui::ImageFit::Contain)
            .Tint(theme.colors.on_surface_variant)
            .With(huxerui::Frame{.width = 20.0F, .height = 20.0F}),
        std::move(title));
}

[[huxerui::composable]] huxerui::View HomeCardFlagHeading(
    std::string countryCode, huxerui::StringVariant title) {
    return HomeCardHeadingWith(CountryFlag(std::move(countryCode), 20.0F, 15.0F, true),
                               std::move(title));
}

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

// 用窗口内索引作为唯一的样本键和递增的横轴坐标。
huxerui::XYChartData BuildTrafficChartData(
    const std::vector<stream::TrafficPoint>& history,
    const std::string& label, std::int64_t stream::TrafficPoint::* rate) {
    std::vector<huxerui::XYDatum> samples;
    samples.reserve(history.size());
    for (std::size_t i = 0; i < history.size(); ++i) {
        samples.push_back({std::to_string(i), static_cast<double>(i), static_cast<double>(history[i].*rate)});
    }
    return huxerui::XYChartData({huxerui::XYSeries("rate", label, std::move(samples))});
}

// ---- 卡片内容 --------------------------------------------------------------

// 流量统计：圆环旁保留方向占比，累计上传/下载量固定排在卡片底部。
[[huxerui::composable]] huxerui::View HomeTotalCard(const HomeState& s) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const bool fourColor = huxerui::UseEnvironment<FluxGradientStyle>().enabled;
    const auto muted = theme.colors.on_surface_variant;
    const auto directions = fourColor ? FourColorTrafficTotalColors(theme) : TrafficColors{muted, muted};
    const std::int64_t upload = s.coreReady ? std::max<std::int64_t>(0, s.totalUp) : 0;
    const std::int64_t download = s.coreReady ? std::max<std::int64_t>(0, s.totalDown) : 0;
    const double total = static_cast<double>(upload) + static_cast<double>(download);
    const auto shareText = [total](std::int64_t value) {
        if (total <= 0.0) return std::string{"0%"};
        const int percent = static_cast<int>(static_cast<double>(value) / total * 100.0 + 0.5);
        return std::to_string(percent) + "%";
    };
    const auto shareFont = huxerui::Font::System(font_size::kCaption)
                               .WithWeight(huxerui::FontWeight::SemiBold);
    const auto shareMetric = [muted, shareFont](std::string direction, const std::string& share) {
        return huxerui::Text(direction + " " + share)
            .Style(huxerui::TextStyle{shareFont, muted});
    };
    const auto amountFont = huxerui::Font::System(font_size::kBody)
                                .WithWeight(huxerui::FontWeight::SemiBold);
    const auto amountMetric = [&theme, amountFont](std::string direction, std::int64_t bytes, huxerui::Color color) {
        return huxerui::Row {
            huxerui::Text(std::move(direction))
                .Style(huxerui::TextStyle{amountFont, color}),
            huxerui::Text(bytes == 0 ? "0B" : formatBytes(bytes))
                .Style(huxerui::TextStyle{amountFont, theme.colors.on_surface}),
        }.With(huxerui::Spacing(6.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    };
    const huxerui::PieChartData data({
        {"upload", "↑",
         static_cast<double>(upload)},
        {"download", "↓",
         static_cast<double>(download)},
    });

    // fork 自带圆角段、中心占比与悬停动画，不再覆盖一份静态中心文字。
    return huxerui::Column {
        HomeCardHeading(app::images::swap, Localized("流量统计")),
        huxerui::Row {
            huxerui::PieChart(data)
                .Style(huxerui::PieChartStyle{.inner_radius = 0.62F, .rounded_caps = true})
                .SliceStyle("upload", {.fill = huxerui::Brush(directions.upload)})
                .SliceStyle("download", {.fill = huxerui::Brush(fourColor ? directions.download : theme.colors.on_surface)})
                .With(huxerui::Frame{.width = 72.0F, .height = 72.0F}),
            huxerui::Column {
                shareMetric("↑", shareText(upload)),
                shareMetric("↓", shareText(download)),
            }.With(huxerui::Grow(1.0F),
                   huxerui::Spacing(12.0F),
                   huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
        }.With(huxerui::Grow(1.0F),
               huxerui::Spacing(8.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Column {
            amountMetric("↑", upload, directions.upload),
            amountMetric("↓", download, directions.download),
        }.With(huxerui::Spacing(4.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
    }.With(huxerui::Spacing(8.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View HomeTrafficCard(const HomeState& s, bool upload) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const bool fourColor = huxerui::UseEnvironment<FluxGradientStyle>().enabled;
    const bool fresh = huxerui::UseService<CoreModel>()->view.Get().health.traffic.state == health::State::Fresh;
    const auto directions = ResolveTrafficColors(theme, fourColor);
    const auto color = fourColor ? (upload ? directions.upload : directions.download) : theme.colors.on_surface_variant;
    auto fill = color;
    fill.alpha = fourColor ? 0.14F : 0.18F;
    const auto rateField = upload ? &stream::TrafficPoint::up : &stream::TrafficPoint::down;
    const auto rate = s.latest.*rateField;
    const auto title = upload ? Localized("上传") : Localized("下载");
    const auto chartLabel = huxerui::UseString(title);
    const bool hasTraffic = std::ranges::any_of(s.history, [](std::int64_t sample) { return sample > 0; }, rateField);
    huxerui::View chart;
    if (hasTraffic) {
        chart = huxerui::AreaChart(BuildTrafficChartData(s.history, chartLabel, rateField))
            .Baseline(0.0)
            .Interpolation(huxerui::ChartInterpolation::Monotone)
            // 隐藏两轴的轴线、刻度数字和网格（Visible(false)
            // 不改变标度域）。Y 轴 formatter 仍供 tooltip 与语义读数使用。
            .XAxis(huxerui::ChartAxis::Linear().Visible(false))
            .YAxis(huxerui::ChartAxis::Linear()
                       .IncludeZero()
                       .Visible(false)
                       .Formatter([](double value) {
                           return formatRate(static_cast<std::int64_t>(value));
                       }))
            .Legend(huxerui::ChartLegendOptions{.visible = false})
            .Accessibility(huxerui::ChartAccessibilityOptions{.summary = chartLabel})
            .SeriesStyle("rate",
                         huxerui::AreaChartStyle{
                             .fill = huxerui::Brush(fill),
                             .stroke = huxerui::Brush(color),
                             .stroke_style = huxerui::StrokeStyle{.width = upload ? 1.5F : 2.0F,
                                 .cap = huxerui::StrokeCap::Round, .join = huxerui::StrokeJoin::Round}});
    }
    return HomeTrafficCardLayout(
        HomeCardHeading(upload ? app::images::arrow_up : app::images::arrow_down, title),
        huxerui::Text(fresh && rate > 0 ? formatRate(rate) : "0B/s")
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption), theme.colors.on_surface_variant}),
        std::move(chart), ResolveIslandTheme(theme).island_padding);
}

// 出站模式：规则、全局、直连各占一行。点击乐观更新，失败做目标值校验后回落。
[[huxerui::composable]] huxerui::View HomeModeCard() {
    const auto control = UseCoreControl();
    const std::size_t selected = HomeModeIndex(control.Mode());

    std::vector<huxerui::StringVariant> labels;
    for (const auto& label : kModeLabels) labels.push_back(Localized(label));
    return huxerui::Column {
        HomeCardHeading(app::images::route, Localized("出站模式")),
        HomeModeChoices(labels, selected, true, [control](std::size_t index) {
            control.Run({.action = CoreAction::Mode, .mode = kModes[index]});
        }),
    }.With(huxerui::Spacing(4.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View HomeProfileProxyPickers(
    bool hasProfile, huxerui::State<std::string> expandedPicker) {
    const auto proxies = huxerui::UseService<ProxiesModel>();
    const auto profiles = huxerui::UseService<ProfilesModel>();
    const auto core = huxerui::UseService<CoreModel>();
    const auto tasks = huxerui::UseApplicationTaskScope();
    const auto toast = UseFluxToast();
    const bool enabled = !profiles->selectionPending.Get() && !proxies->homeSelectionPending.Get();
    const auto revision = clashflux::data::session().profilesRevision();
    return HomeProxyPickers(proxies->snapshot, revision, hasProfile, core->view.Get().Mode() == "global",
        enabled, expandedPicker,
        [proxies, profiles, tasks, toast](std::string group, std::string node, std::uint64_t revision) {
            if (profiles->selectionPending.Get() || proxies->homeSelectionPending.Get()) return;
            const auto& snapshot = proxies->snapshot.Get();
            if (snapshot.profilesRevision != revision ||
                clashflux::data::session().profilesRevision() != revision) {
                proxies->RequestRefresh();
                return;
            }
            const auto found = std::ranges::find(snapshot.groups, group, &ProxyGroupSnapshot::name);
            if (found == snapshot.groups.end() || !found->selectable || found->current == node ||
                std::ranges::find(found->nodes, node) == found->nodes.end()) return;
            proxies->homeSelectionPending = true;
            tasks.Launch([proxies, toast, group = std::move(group), node = std::move(node),
                          revision]() -> huxerui::Task<void> {
                bool selected = false;
                try {
                    selected = co_await RunOnTaskThread([group, node, revision] {
                        return clashflux::data::session().profilesRevision() == revision &&
                               SelectProxyLine(group, node);
                    });
                } catch (...) {
                    selected = false;
                }
                proxies->homeSelectionPending = false;
                proxies->RequestRefresh();
                if (!selected && clashflux::data::session().profilesRevision() == revision) {
                    toast.Show(Localized("线路切换失败"));
                }
            });
        });
}

// 当前订阅卡：用订阅名作标题，策略组和节点复用代理页的快照与切换接口。
// 流量行只在解析到 subscription-userinfo 数据时显示；total 未知时进度条摆满。
[[huxerui::composable]] huxerui::View HomeProfileCard(
    const HomeState& s, huxerui::State<std::string> expandedPicker) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const bool hasProfile = s.profileId != 0;
    const huxerui::Color muted = theme.colors.on_surface_variant;
    const std::string profileName =
        !s.profilesReady ? std::string{} : s.profileName == "未启用订阅"
            ? huxerui::UseString(Localized("未启用订阅"))
            : s.profileName;
    const bool hasTraffic =
        hasProfile && (s.profileTotalBytes > 0 || s.profileUsedBytes > 0);
    const int percent = s.profileTotalBytes > 0
                            ? static_cast<int>(std::clamp(
                                  static_cast<double>(s.profileUsedBytes) /
                                      static_cast<double>(s.profileTotalBytes),
                                  0.0, 1.0) *
                              100.0)
                            : 0;
    const huxerui::StringVariant trafficLine =
        s.profileTotalBytes > 0
            ? LocalizedFormat("{}% {} / {}", percent,
                              formatBytes(s.profileUsedBytes),
                              formatBytes(s.profileTotalBytes))
            : LocalizedFormat("{} / 不限量",
                              formatBytes(s.profileUsedBytes));
    // total 未知/不限量时没有可用比例，按「满」呈现（解析到数据前也一致）。
    const float progress =
        s.profileTotalBytes > 0
            ? std::clamp(static_cast<float>(s.profileUsedBytes) /
                             static_cast<float>(s.profileTotalBytes),
                         0.0F, 1.0F)
            : (hasProfile ? 1.0F : 0.0F);

    std::vector<huxerui::View> children;
    if (!s.profilesReady) {
        children.push_back(huxerui::Row {
            huxerui::Image(app::images::request)
                .Fit(huxerui::ImageFit::Contain)
                .Tint(theme.colors.on_surface_variant)
                .With(huxerui::Frame{.width = 20.0F, .height = 20.0F}),
            PlaceholderText(HomeCardHeadingFont(), 64.0F),
        }.With(huxerui::Spacing(8.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
    } else {
        children.push_back(HomeCardHeading(app::images::request, profileName));
    }
    children.push_back(HomeProfileProxyPickers(hasProfile, expandedPicker).Key(std::to_string(s.profileId)));
    children.push_back(huxerui::Spacer());
    if (hasTraffic) {
        children.push_back(
            huxerui::Text(trafficLine)
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption), muted}));
    }
    if (hasProfile) {
        children.push_back(
            ProfileQuotaBar(progress));
    }

    return huxerui::Column(std::move(children))
        .With(huxerui::Spacing(5.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// ---- 网络信息卡片（双平台共用）--------------------------------------------

// 内网 IP：只展示当前默认路由使用的接口和地址。首页可见时每 5 秒请求一次刷新；
// 结果归应用级 HomeNetworkModel，同一时刻最多一个读取在途，到达后直接替换，
// 重新进入首页立即显示上次结果（路由选择的系统调用仍按约定放任务线程）。
huxerui::Task<std::optional<std::string>> FetchLanAddress() {
    auto active = co_await RunOnTaskThread([] { return netinfo::activeLanIpv4Address(); });
    if (!active) co_return std::nullopt;
    co_return active->address;
}

[[huxerui::composable]] huxerui::View HomeLanIpCard() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    const auto appTasks = huxerui::UseApplicationTaskScope();
    const auto model = huxerui::UseService<HomeNetworkModel>();
    huxerui::Lifecycle(
        [tasks, appTasks, model] {
            tasks.Launch([appTasks, model]() -> huxerui::Task<void> {
                for (;;) {
                    model->RefreshLan(appTasks, [] { return FetchLanAddress(); });
                    co_await huxerui::Delay(std::chrono::duration<double>{5.0});
                }
            });
            return [] {};
        },
        0);

    const std::optional<std::string> current = model->lanAddress.Get();
    const auto font = HomeCardHeadingFont();
    huxerui::View line;
    if (current) {
        line = huxerui::Text(*current)
            .LineLimit(1)
            .Overflow(huxerui::TextOverflow::Ellipsis)
            .Style(huxerui::TextStyle{font, theme.colors.on_surface})
            .Key("home-lan-ip-current");
    } else if (model->lanReady.Get()) {
        line = huxerui::Text(Localized("未检测到内网地址"))
            .Style(huxerui::TextStyle{font, theme.colors.on_surface_variant});
    } else {
        line = PlaceholderText(font);
    }

    return huxerui::Column {
        HomeCardHeading(app::images::network, Localized("内网 IP")),
        std::move(line),
        huxerui::Spacer(),
    }.With(huxerui::Spacing(4.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// ---- 网络检测（公网出口 IP）-----------------------------------------------
// 回显服务列表见 public_ip_lookup.h（与网站测试的出站核实共用）。

#if defined(__ANDROID__) || defined(CLASHFLUX_IOS)
// 手机：应用自身流量不经过本应用的 TUN，直连回显服务即设备出口。
huxerui::Task<wire::PublicIpLookup> FetchPublicIp(std::shared_ptr<AppHttpClient> http,
                                                  bool proxyActive, std::string mixedPort) {
    static_cast<void>(proxyActive);
    static_cast<void>(mixedPort);
    for (const std::string_view url : kPublicIpLookupUrls) {
        try {
            huxerui::HttpRequest request;
            request.url = std::string(url);
            request.timeout = std::chrono::seconds{8};
            request.headers = {{"Cache-Control", "no-cache"}};
            auto response = co_await http->SendAsync(std::move(request));
            if (!response.Succeeded()) continue;
            const auto& responseValue = response.Value();
            if (responseValue.status_code != 200 || responseValue.body.empty() ||
                responseValue.body.size() > 4096) continue;
            std::string text(reinterpret_cast<const char*>(responseValue.body.data()),
                             responseValue.body.size());
            if (auto decoded = wire::DecodePublicIpLookup(text)) co_return std::move(decoded.value);
        } catch (...) {
            // 单个回显端点失败时继续尝试下一个。
        }
    }
    co_return wire::PublicIpLookup{};
}
#else
// 桌面：内核代理监听时经 mixed 入站测量代理出口；只有待命内核时测直连。
huxerui::Task<wire::PublicIpLookup> FetchPublicIp(std::shared_ptr<AppHttpClient> http,
                                                  bool proxyActive, std::string mixedPort) {
    static_cast<void>(http);
    co_return co_await RunOnTaskThread([proxyActive, mixedPort = std::move(mixedPort)] {
        const std::string proxyUrl =
            proxyActive ? "http://127.0.0.1:" + mixedPort : std::string{};
        for (const std::string_view url : kPublicIpLookupUrls) {
            const api::ApiResult result = api::fetchText(std::string(url), proxyUrl, 8);
            if (!result.ok) continue;
            if (auto decoded = wire::DecodePublicIpLookup(result.body))
                return std::move(decoded.value);
        }
        return wire::PublicIpLookup{};
    });
}
#endif

// 网络检测：挂载、内核代理启停、入站端口、出站模式、启用订阅或出口节点变化时异步检测；
// 结果归应用级 HomeNetworkModel：同一时刻只有一个检测在途，新路由覆盖旧请求，
// 未取得首份结果前使用空白占位，检测期间继续显示上次结果，到达后直接替换；
// 失败时按退避间隔自动重试，不提供手动刷新按钮。
[[huxerui::composable]] huxerui::View HomeNetCheckCard(std::int64_t profileId) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const auto appTasks = huxerui::UseApplicationTaskScope();
    const auto http = huxerui::UseService<AppHttpClient>();
    const auto coreModel = huxerui::UseService<CoreModel>();
    const auto proxiesModel = huxerui::UseService<ProxiesModel>();
    const auto model = huxerui::UseService<HomeNetworkModel>();
    const CoreView core = coreModel->view.Get();
    const bool proxyActive = core.ProxyActive();
    const std::string mixedPort = std::to_string(core.core.mixedPort);
    const std::string mode = core.Mode();
    const std::string activeNodeTag =
        ResolveActiveProxySelection(proxiesModel->snapshot.Get(), mode).tag;
    const std::string routeKey = std::to_string(proxyActive ? 1 : 0) + "|" +
        mixedPort + "|" + mode + "|" + std::to_string(profileId) + "|" +
        activeNodeTag;
    huxerui::Lifecycle([model, appTasks, http, proxyActive, mixedPort, routeKey] {
        model->RequestPublic(appTasks, routeKey, [http, proxyActive, mixedPort] {
            return FetchPublicIp(http, proxyActive, mixedPort);
        });
        return [] {};
    }, routeKey);

    const wire::PublicIpLookup current = model->publicIp.Get();
    const auto font = HomeCardHeadingFont();
    huxerui::View status;
    if (!model->publicReady.Get()) {
        status = PlaceholderText(font);
    } else if (model->publicFailed.Get()) {
        status = huxerui::Text(Localized("检测失败"))
            .Style(huxerui::TextStyle{font, theme.colors.error});
    } else {
        status = huxerui::Text(current.ip)
            .LineLimit(1)
            .Overflow(huxerui::TextOverflow::Ellipsis)
            .Style(huxerui::TextStyle{font, theme.colors.on_surface})
            .With(huxerui::Tooltip(current.ip));
    }

    return huxerui::Column {
        HomeCardFlagHeading(current.countryCode, Localized("网络检测")),
        std::move(status),
        huxerui::Spacer(),
    }.With(huxerui::Spacing(4.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// ---- 平台专属卡片（整个函数由编译宏在调用点选择）---------------------------

BoxCatPhase DesktopHomeCatPhase(const CoreControl& control,
                                CoreAction presented = CoreAction::Idle) {
    // A Start presentation now means the successful cat is merging into its
    // card, rather than a kernel command that is still waiting to finish.
    if (presented == CoreAction::Start) return BoxCatPhase::Running;
    const CoreAction action = presented == CoreAction::Idle
        ? control.ConnectionAction() : presented;
    if (action == CoreAction::Start) return BoxCatPhase::Starting;
    if (action == CoreAction::Stop) return BoxCatPhase::Stopping;
    const auto& view = control.model->home.Get();
    if (view.RestartingProxy(action)) return BoxCatPhase::Restarting;
    switch (view.coreState) {
    case core::CoreState::Starting: return BoxCatPhase::Sleeping;
    case core::CoreState::Running: return view.proxyActive ? BoxCatPhase::Running : BoxCatPhase::Sleeping;
    case core::CoreState::Failed: return BoxCatPhase::Failed;
    case core::CoreState::Stopped: return BoxCatPhase::Sleeping;
    }
    return BoxCatPhase::Sleeping;
}

#if !defined(__ANDROID__)

[[huxerui::composable]] huxerui::View DesktopModeSwitchRow(
    huxerui::ImageVariant icon, huxerui::StringVariant label,
    huxerui::View action, huxerui::View control) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Column {
        huxerui::Row {
            huxerui::Image(icon)
                .Fit(huxerui::ImageFit::Contain)
                .Tint(theme.colors.on_surface_variant)
                .With(huxerui::Frame{.width = 20.0F, .height = 20.0F}),
            huxerui::Text(label).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody)
                    .WithWeight(huxerui::FontWeight::SemiBold),
                theme.colors.on_surface}),
        }.With(huxerui::Spacing(8.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Row {
            std::move(action),
            huxerui::Spacer(),
            control,
        }.With(huxerui::Spacing(8.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
    }.With(huxerui::Spacing(2.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// Desktop entry points share application state, operation lifetime and feedback.
[[huxerui::composable]] huxerui::View DesktopHomeProxyCard() {
    const auto control = UseCoreControl();
    return huxerui::Column {
        DesktopModeSwitchRow(
            app::images::system_proxy, Localized("系统代理"),
            HomeProxySettingsAction(HomeProxySettingsKind::SystemProxy),
            huxerui::Switch(control.SystemProxyEnabled())
                .OnChanged([control](bool on) {
                    control.Run({.action = CoreAction::SystemProxy, .enabled = on});
                })),
        huxerui::Spacer(),
    }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View DesktopHomeTunCard() {
    const auto control = UseCoreControl();
    return huxerui::Column {
        DesktopModeSwitchRow(
            app::images::tun, Localized("虚拟网卡"),
            HomeProxySettingsAction(HomeProxySettingsKind::Tun),
            huxerui::Switch(control.TunEnabled())
                .OnChanged([control](bool on) {
                    control.Run({.action = CoreAction::Tun, .enabled = on});
                })),
        huxerui::Spacer(),
    }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View DesktopHomePlatformCard(
    HomeCardKind kind) {
    if (kind == HomeCardKind::Proxy) return DesktopHomeProxyCard();
    if (kind == HomeCardKind::Tun) return DesktopHomeTunCard();
    return huxerui::Row{};
}

void RunDesktopHomeCoreAction(const CoreControl& control,
                              huxerui::State<bool> cardShown,
                              huxerui::State<HomeControlMotion> motion,
                              huxerui::SharedTransitionHandle transition,
                              CoreAction action, bool reducedMotion) {
    if (control.Busy()) return;
    // Startup stays in the button until real success and the cat's completion
    // signal. Stopping still shrinks the card back into the sleeping button.
    if (action == CoreAction::Disconnect)
        AnimateHomeControlCard(false, cardShown, motion, transition, control.tasks, reducedMotion);
    control.Run({.action = action});
}

[[huxerui::composable]] huxerui::View DesktopHomeStatusCard(
    huxerui::State<bool> cardShown,
    huxerui::State<HomeControlMotion> motion,
    huxerui::SharedTransitionHandle transition) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    if (motion.Get().collapsed) {
        return huxerui::Stack {
            BoxCatButtonContent(BoxCatPhase::Running, theme.colors.on_surface, false)
                .With(huxerui::SharedBounds("home-core-control")
                    .BoundsTransform(HomeControlReturnToDock{})),
        }.With(huxerui::Align(huxerui::HorizontalAlignment::Center,
                             huxerui::VerticalAlignment::Center));
    }
    const auto control = UseCoreControl();
    const auto tasks = huxerui::UseTaskScope();
    auto clock = huxerui::UseState<std::chrono::steady_clock::time_point>(
        std::chrono::steady_clock::now());
    huxerui::Lifecycle(
        [tasks, clock] {
            tasks.Launch([clock]() -> huxerui::Task<void> {
                for (;;) {
                    co_await huxerui::Delay(std::chrono::seconds{1});
                    clock = std::chrono::steady_clock::now();
                }
            });
            return [] {};
        },
        0);
    const CoreAction action = control.ConnectionAction();
    const bool entering = motion.Get().action == CoreAction::Start;
    const bool returning = motion.Get().action == CoreAction::Stop;
    // Shared-bounds playback keeps the destination paint-stable; the cat's
    // retained clocks resume as soon as the outer transition finishes.
    const bool changing = !entering && (action == CoreAction::Start ||
                                        action == CoreAction::Stop);
    const auto phase = DesktopHomeCatPhase(control, motion.Get().action);
    const huxerui::StringVariant status = Localized(
        phase == BoxCatPhase::Restarting ? "正在重启"
        : action == CoreAction::Start ? "正在启动"
        : returning || action == CoreAction::Stop ? "正在停止"
                                     : "运行中");
    const auto runningSince = control.model->home.Get().ProxySessionSince();
    const auto elapsed = runningSince == std::chrono::steady_clock::time_point{}
        ? std::chrono::seconds{0}
        : std::max(std::chrono::seconds{0},
                   std::chrono::duration_cast<std::chrono::seconds>(
                       clock.Get() - runningSince));
    const auto seconds = elapsed.count();
    const std::string uptime = FormatHomeUptime(seconds);
    huxerui::View icon = BoxCatStatusIcon(phase, motion.Get().action == CoreAction::Idle);
    huxerui::View content = huxerui::Row {
        std::move(icon)
            .With(huxerui::Scale(huxerui::AnimateTo(
                      changing ? 0.78F : 1.0F,
                      huxerui::TweenSpec{.duration = 0.28,
                                         .easing = huxerui::Easing::EaseOut}))),
        huxerui::Column {
            huxerui::Text(status).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption)
                    .WithWeight(huxerui::FontWeight::SemiBold),
                theme.colors.on_surface}),
            huxerui::Text(uptime).Style(huxerui::TextStyle{
                huxerui::Font::Monospace(font_size::kCaption),
                theme.colors.on_surface_variant}),
        }.With(huxerui::Spacing(2.0F)),
    }.With(huxerui::Spacing(6.0F),
           huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
           huxerui::MainAlign(huxerui::MainAxisAlignment::Center));
    return huxerui::View{HomeCardSurface(theme, std::move(content), "control")}
        .OnClick([control, cardShown, motion, transition, reducedMotion = theme.motion.reduced_motion] {
            RunDesktopHomeCoreAction(control, cardShown, motion, transition,
                                     CoreAction::Disconnect, reducedMotion);
        })
        .With(huxerui::Scale(huxerui::AnimateTo(
                  motion.Get().collapsing ? kHomeControlCollapsedScale : 1.0F,
                  kHomeControlCollapseMotion)),
              huxerui::Enabled(!control.ConnectionBusy() && !entering && !returning &&
                  phase != BoxCatPhase::Restarting),
              huxerui::Tooltip(Localized("断开连接")),
              huxerui::Indication{},
              huxerui::SharedBounds("home-core-control")
                  .BoundsTransform(HomeControlMoveExpand{}),
              huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                 .label = huxerui::UseString(
                                     Localized("断开连接"))});
}

// 桌面宽窗：内核猫咪并入网格卡片；Compact 与移动端共用标题栏组件。
// The main button opens/closes proxy listeners. The ready core remains available for tests.
[[huxerui::composable]] huxerui::View DesktopHomeFloatingAction(
    huxerui::View page, huxerui::State<bool> cardShown,
    huxerui::State<HomeControlMotion> motion,
    huxerui::State<bool> catReady,
    huxerui::SharedTransitionHandle transition) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const auto control = UseCoreControl();
    huxerui::View base = page;
    const bool returning = motion.Get().action == CoreAction::Stop;
    const bool active = returning ? false : control.ProxyActive();
    const CoreAction command = control.ConnectionAction();
    const bool enabled = !control.ConnectionBusy() && !returning &&
        !control.model->home.Get().RestartingProxy(command);
    const bool changing = !returning && (command == CoreAction::Start ||
                                        command == CoreAction::Stop);
    const auto toggle = [control, active, cardShown, motion, transition,
                         reducedMotion = theme.motion.reduced_motion] {
        RunDesktopHomeCoreAction(control, cardShown, motion, transition,
            active ? CoreAction::Disconnect : CoreAction::Connect, reducedMotion);
    };

    const auto onRunningReady = [control, catReady] {
        const auto action = control.ConnectionAction();
        if (action != CoreAction::Start && action != CoreAction::Stop &&
            action != CoreAction::Restart &&
            control.ProxyActive())
            catReady = true;
    };
    huxerui::View button = BoxCatButtonContent(
        DesktopHomeCatPhase(control, motion.Get().action),
        theme.colors.on_surface,
        motion.Get().action == CoreAction::Idle, {}, onRunningReady,
        returning ? std::optional{BoxCatPhase::Running} : std::nullopt).OnClick(toggle)
        .With(huxerui::Tooltip(Localized(active ? "断开连接" : "连接代理")),
              huxerui::Enabled(enabled),
              huxerui::Focusable(true),
              huxerui::Indication{},
              huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                 .label = huxerui::UseString(Localized(
                                     active ? "断开连接" : "连接代理"))});
    auto bounds = huxerui::SharedBounds("home-core-control");
    if (returning) bounds = std::move(bounds).BoundsTransform(HomeControlReturnToDock{});
    else bounds = std::move(bounds).BoundsTransform(HomeControlMoveExpand{});
    huxerui::View floating = huxerui::Stack {
        std::move(button),
    }.With(huxerui::Frame{.width = 56.0F, .height = 56.0F},
           huxerui::Align(huxerui::HorizontalAlignment::Center,
                          huxerui::VerticalAlignment::Center),
           huxerui::Scale(huxerui::AnimateTo(
               changing ? 0.94F : 1.0F,
               huxerui::TweenSpec{.duration = 0.18,
                                  .easing = huxerui::Easing::EaseOut})),
           std::move(bounds));
    huxerui::View dock = huxerui::Column {
        std::move(floating),
    }.With(huxerui::Padding(huxerui::EdgeInsets{
                 .right = theme.spacing.medium + kScrollBarGutter,
                 .bottom = theme.spacing.medium,
                 .left = theme.spacing.medium,
             }),
             huxerui::MainAlign(huxerui::MainAxisAlignment::End),
             huxerui::CrossAlign(huxerui::CrossAxisAlignment::End));
    return huxerui::Stack {
        std::move(base),
        !cardShown.Get() ? std::move(dock)
                                    : huxerui::View{huxerui::Row{}},
    }.With(huxerui::Grow(1.0F),
           huxerui::Align(huxerui::HorizontalAlignment::Stretch,
                          huxerui::VerticalAlignment::Stretch),
           transition.Scope())
        .Key("home-control-transition-scope");
}

#endif

// ---- 卡片内容分发 ----------------------------------------------------------

[[huxerui::composable]] huxerui::View HomeCardContent(
    HomeCardKind kind, huxerui::State<HomeState> state, huxerui::State<std::string> expandedPicker) {
    const HomeState& s = state.Get();
    if (kind == HomeCardKind::Download || kind == HomeCardKind::Upload) {
        return HomeTrafficCard(s, kind == HomeCardKind::Upload);
    }
    if (kind == HomeCardKind::Total) return HomeTotalCard(s);
    if (kind == HomeCardKind::Mode) {
        return HomeModeCard();
    }
    if (kind == HomeCardKind::Profile) return HomeProfileCard(s, expandedPicker);
    if (kind == HomeCardKind::LanIp) return HomeLanIpCard();
    if (kind == HomeCardKind::NetCheck) return HomeNetCheckCard(s.profileId);
    return CLASHFLUX_HOME_PLATFORM_CARD(s, state, kind);
}

[[huxerui::composable]] huxerui::View HomeCardWithEasterEgg(
    const huxerui::ThemeSpec& theme, HomeCardKind kind, huxerui::State<HomeState> state, std::string cardId) {
    auto expandedPicker = huxerui::UseState(std::string{});
    return HomeCardSurface(theme, HomeCardContent(kind, state, expandedPicker), std::move(cardId), [] {
        return HomeEasterEggContent(kHomeEasterEggPool[PickHomeEasterEgg()], [](std::string_view url) {
            core::openInBrowser(std::string{url});
        });
    }, expandedPicker);
}

} // namespace

[[huxerui::composable]] huxerui::View HomePage(
    huxerui::State<std::size_t> navPage, bool active) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const float pageInset = ResolveIslandTheme(theme).nested_radius;
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    auto tasks = huxerui::UseTaskScope();
    auto toast = UseFluxToast();
    // 策略组快照 / 内核与接管状态的全局唯一来源（见 *_model.h）。
    const auto coreModel = huxerui::UseService<CoreModel>();
    const auto profilesModel = huxerui::UseService<ProfilesModel>();
    auto state = huxerui::UseState<HomeState>({});
    const auto scrollOffset = huxerui::UseState(std::make_shared<PageScrollOffset>()).Get();
    auto homeView = coreModel->home.Get();
    auto controlCardShown = huxerui::UseState(homeView.proxyActive || homeView.proxyRestartSince.has_value());
    auto controlMotion = huxerui::UseState<HomeControlMotion>({});
    auto controlCardSynced = huxerui::UseState<bool>(false);
    auto controlCatReady = huxerui::UseState<bool>(false);
    const auto controlTransition = huxerui::UseSharedTransition();
    const bool headerControlEnabled = !kPageTitlesInWindow || compact;
    const auto control = UseCoreControl();
    const auto vpnStatus = kDesktopHomeControl
        ? HomeVpnStatus{BoxCatPhase::Sleeping, "", false, false, false}
        : ResolveHomeVpnStatus(homeView.vpnState, coreModel->operations.command.Get());
    const auto headerPhase = kDesktopHomeControl ? DesktopHomeCatPhase(control) : vpnStatus.phase;
    const auto headerControl = UseHomeHeaderControl(HomeHeaderSnapshot{
        .ready = homeView.ready && (kDesktopHomeControl || homeView.vpnSnapshotReady),
        .active = active && headerControlEnabled,
        .running = kDesktopHomeControl ? homeView.proxyActive : vpnStatus.connected,
        .busy = kDesktopHomeControl
            ? control.ConnectionBusy() || headerPhase == BoxCatPhase::Restarting : control.Busy(),
        .canStart = kDesktopHomeControl || state.Get().profileId != 0,
        .phase = headerPhase,
        .elapsedSeconds = homeView.vpnElapsedMs / 1000,
        .runningSince = kDesktopHomeControl ? homeView.ProxySessionSince()
            : std::chrono::steady_clock::time_point{},
    });
    const auto toggleHeader = [control] {
        if (kDesktopHomeControl) {
            control.Run({.action = control.ProxyActive() ? CoreAction::Disconnect : CoreAction::Connect});
        } else {
            control.Run({.action = CoreAction::Tun, .enabled = control.model->view.Get().vpnState != 2});
        }
    };
    const auto headerStopLabel = Localized(kDesktopHomeControl ? "断开连接" : "停止 VPN");
    const auto headerActionLabel = headerControl.latest.Get().running ? headerStopLabel
        : Localized(kDesktopHomeControl ? "连接代理" : "启动 VPN");

    // 基础卡片顺序与尺寸只由 kHomeCards 决定，没有布局持久化；尾部控制卡
    // 只随桌面运行状态出现，不改变其它卡片的声明跨度。
    const HomeLayout order = DefaultHomeLayout();

    // 桌面：流量/连接由推送驱动（见 SubscribeHomeStreams），不占用轮询。
    huxerui::Lifecycle(
        [tasks, state] {
            const std::uint64_t subscription = SubscribeHomeStreams(tasks, state);
            return [subscription] { UnsubscribeStreamUpdates(subscription); };
        },
        0);

    if (kDesktopHomeControl) {
        huxerui::Lifecycle(
            [coreModel, controlCardShown, controlCardSynced, controlCatReady,
             controlMotion, controlTransition, tasks, active, compact,
             reducedMotion = theme.motion.reduced_motion] {
                const auto& view = coreModel->home.Get();
                if (view.ready) {
                    const bool running = view.proxyActive;
                    const auto command = coreModel->operations.command.Get().action;
                    const bool restarting = view.RestartingProxy(command);
                    if (!controlCardSynced.Get() || !active || compact) {
                        controlCardSynced = true;
                        controlCardShown = running || restarting;
                        controlCatReady = running || restarting;
                    } else if (!restarting) {
                        if (!running || command == CoreAction::Connect ||
                            command == CoreAction::Disconnect || command == CoreAction::Stop)
                            controlCatReady = false;
                        if (!coreModel->operations.Busy() &&
                            controlCardShown.Get() != running &&
                            (!running || controlCatReady.Get())) {
                            AnimateHomeControlCard(running, controlCardShown,
                                controlMotion, controlTransition, tasks, reducedMotion);
                        }
                    }
                }
                return [] {};
            },
            coreModel->home, coreModel->operations.command, controlCatReady,
            active, compact, theme.motion.reduced_motion);
    }

    // 不可见时只保留本页 State/Lifecycle，不构建重子树：huxerui 的 Pager 会把
    // 四个一级页同时挂载，隐藏页即使不重组，其已挂载子树仍随每一帧被重新测量。
    // 真机实测（代理页大分组）：四页同挂时每帧 1443 次测量请求 / ~20ms，
    // 只留当前页后降到 28 次 / ~0ms；因此不可见页必须返回空占位。
    if (!active) return huxerui::View{huxerui::Row{}}.Key("home-idle");

    // 运行态镜像：桌面由模型 State 驱动（无定时器），Android 由固定节拍泵
    // 采样 libbox 速率。平台实现由宏选择（见文件上方的 HomeRuntimePump）。
    //
    // 必须把返回的 View **挂载**进视图树：hcg 把 composable 体包成
    // huxerui::Scope 工厂，只有挂载时工厂才会执行。当裸语句丢弃返回值时，
    // 里面的 Lifecycle 永不注册、MirrorHomeState 一次都不跑，HomeState 保持
    // 默认值（首页订阅/流量/总量全空）——Android 真机上实测过的回归。
    huxerui::View runtimePump = CLASHFLUX_HOME_RUNTIME_PUMP(
        state, coreModel, profilesModel);

    std::vector<huxerui::View> cards;
    cards.reserve(order.size());
    for (const HomeCardEntry& entry : order) {
        const HomeCardSpec* spec = FindHomeCard(entry.kind);
        if (spec == nullptr) continue;
        const std::string id{spec->id};
        const HomeCardSize size = entry.size;
        const HomeCardKind kind = entry.kind;

        // 首页所有卡片一视同仁：都是 raised 二级岛外壳。「当前订阅」不显示选中态
        // （哪个订阅在使用中由订阅页的选中卡承担）。
        huxerui::View cardView = HomeCardWithEasterEgg(theme, kind, state, id);
        cards.push_back(std::move(cardView)
                            .LayoutValue<HomeCardSpan>(size)
                            .Key("home-card-" + id));
    }
    if (kDesktopHomeControl && !compact && controlCardShown.Get()) {
        cards.push_back(huxerui::View{CLASHFLUX_HOME_CONTROL_CARD(controlCardShown, controlMotion, controlTransition)}
                            .LayoutValue<HomeCardSpan>({1, 1})
                            .LayoutValue<HomeControlSpan>(true)
                            .Key("home-card-control"));
    }

    huxerui::View cardArea = cards.empty()
        ? EmptyState(Localized("首页还没有卡片"), app::images::home)
        : huxerui::View{HomeGrid(std::move(cards))
                            .LayoutValue<HomeGridCompact>(compact)
                            .With(huxerui::Grow(1.0F))};
    huxerui::View grid = huxerui::Column {
        std::move(cardArea),
    }.With(huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    std::vector<huxerui::View> bodySections;
    bodySections.push_back(std::move(grid));
    huxerui::View pageBody = huxerui::Column(std::move(bodySections))
        .With(huxerui::Spacing(0.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    // 注意：首页不要在滚动内容里声明 Focusable(true)——运行时会自动把初始
    // 焦点所在的节点滚入视野，导致首页一打开就被滚到中途（且随后每次重组都可能
    // 来回滚）。可聚焦性交给内置控件自己的默认策略，分段选择器只保留语义。
    // Runtime effects remain mounted independently from the replaceable header.
    const bool showFloatingCat = !compact &&
        !(kDesktopHomeControl && controlCardShown.Get());
    auto homeInsets = ScrollAreaInsets{};
    homeInsets.bottom_avoidance = compact ? kCompactFloatingNavigationInset
        : (showFloatingCat ? kHomeFloatingButtonInset : 0.0F);
    // The cat sits beside the scrollbar. Keep the extra footer clearance for
    // content, but do not shorten the wide rail like the old proxy speed FAB.
    homeInsets.rail_bottom_avoidance = 0.0F;
    huxerui::View scrollContent = huxerui::Stack {
        std::move(runtimePump),
        ScrollablePageContent(pageBody, scrollOffset, homeInsets),
    }.With(huxerui::Grow(1.0F));
    huxerui::View page = PageScaffold(
        Localized("首页"), {}, std::move(scrollContent), true, false, true,
        pageInset, {}, {}, true,
        headerControlEnabled ? HomeHeaderTitle(headerControl, toggleHeader, headerStopLabel) : huxerui::View{},
        true);

    // Mobile and desktop Compact share the same title/dock geometry and motion.
    if (headerControlEnabled) return HomeHeaderFloatingAction(page, headerControl, toggleHeader,
        headerActionLabel, compact ? kCompactFloatingNavigationInset : theme.spacing.medium);

    huxerui::View shell = CLASHFLUX_HOME_FLOATING_ACTION(
        std::move(page), state, compact, controlCardShown, controlMotion, controlCatReady,
        controlTransition);

    return std::move(shell).With(huxerui::Grow(1.0F));
}

#undef CLASHFLUX_HOME_PLATFORM_CARD
#undef CLASHFLUX_HOME_CONTROL_CARD
#undef CLASHFLUX_HOME_FLOATING_ACTION

} // namespace clashflux::ui
