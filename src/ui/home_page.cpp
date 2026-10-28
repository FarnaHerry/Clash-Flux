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
#include "box_cat.h"
#include "home_control_motion.h"
#include "home_header_control.h"
#include "page_layout.h"
#include "home_grid.h"
#include "home_card.h"
#include "home_mode_choices.h"
#include "empty_state.h"
#include "task_bridge.h"

import clashflux.core;
import clashflux.db;
import clashflux.netinfo;
import clashflux.stream;
import clashflux.store.core;
import clashflux.store.profiles;
import clashflux.utils;

// CoreView 含 store::CoreSnapshot、ProfilesModel 含 db::Profile，必须在模块导入
// 之后（同 profiles_cache.h）。
#include "core_model.h"
#include "core_control.h"
#include "home_vpn_status.h"
#include "profiles_model.h"
#include "settings_model.h"
#include "stream_updates.h"

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
//   网络速度 2x2 → 流量曲线 2x2   流量统计 1x2 → 流量统计 1x2
//   系统代理 1x1 → 系统代理 1x1   TUN 1x1      → TUN 模式 1x1
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
    HomeCardKind kind = HomeCardKind::Traffic;
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
    std::string profileUpdated;
    std::int64_t profileUsedBytes = 0;
    std::int64_t profileTotalBytes = 0;
    std::int64_t profileExpireAt = 0;  // Unix 秒；0 = 订阅未提供，不显示到期行
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
    const db::Profile* selected = nullptr;
    for (const db::Profile& candidate : profilesModel->list.Get()) {
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
        s.profileExpireAt = selected->expireAt;
        s.profileUpdated = selected->updatedAt > 0
                               ? "更新于 " + formatTime(selected->updatedAt)
                               : "未拉取";
        if (!selected->error.empty()) s.profileUpdated = "拉取失败";
    } else {
        s.profileId = 0;
        s.profileName = "未启用订阅";
        s.profileUpdated = "";
        s.profileUsedBytes = 0;
        s.profileTotalBytes = 0;
        s.profileExpireAt = 0;
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

// 卡片标题：所有卡片共用同一排版；卡片外壳由卡片槽统一提供。
// 首页卡片左上角均带有契合卡片功能的语义图标（规范见 AGENTS.md）。
[[huxerui::composable]] huxerui::View HomeCardHeading(
    huxerui::ImageResource icon, huxerui::StringVariant title) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Row {
        huxerui::Image(icon)
            .Fit(huxerui::ImageFit::Contain)
            .Tint(theme.colors.primary)
            .With(huxerui::Frame{.width = 20.0F, .height = 20.0F}),
        huxerui::Text(title).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kBody)
                .WithWeight(huxerui::FontWeight::SemiBold),
            theme.colors.on_surface}),
    }.With(huxerui::Spacing(8.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
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

// 流量曲线：下载面积图（蓝色半透明填充）+ 上传折线（橙色）。历史不足两点画平线。
// 首页流量曲线数据：把滚动窗口拍成 Lib-Charts 的坐标快照（datum key 必须非空且唯一、
// x 必须严格递增；这里用窗口内索引当 x，天然递增）。窗口约 1 Hz 变一次，重建 60×2 个
// 点可忽略；空窗口也是合法快照（库会呈现空数据提示）。
huxerui::XYChartData BuildTrafficChartData(
    const std::vector<stream::TrafficPoint>& history,
    const std::string& downloadLabel, const std::string& uploadLabel) {
    std::vector<huxerui::XYDatum> down;
    std::vector<huxerui::XYDatum> up;
    down.reserve(history.size());
    up.reserve(history.size());
    for (std::size_t i = 0; i < history.size(); ++i) {
        std::string key = std::to_string(i);
        const double x = static_cast<double>(i);
        down.push_back({key, x, static_cast<double>(history[i].down)});
        up.push_back({key, x, static_cast<double>(history[i].up)});
    }
    return huxerui::XYChartData({
        huxerui::XYSeries("down", downloadLabel, std::move(down)),
        huxerui::XYSeries("up", uploadLabel, std::move(up)),
    });
}

// ---- 卡片内容 --------------------------------------------------------------

// 流量统计：圆环旁保留方向占比，累计上传/下载量固定排在卡片底部。
[[huxerui::composable]] huxerui::View HomeTotalCard(const HomeState& s) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const auto trafficColors = ResolveTrafficColors(theme);
    const huxerui::Color upColor = trafficColors.upload;
    const huxerui::Color downColor = trafficColors.download;
    const std::int64_t upload = std::max<std::int64_t>(0, s.totalUp);
    const std::int64_t download = std::max<std::int64_t>(0, s.totalDown);
    const double total = static_cast<double>(upload) + static_cast<double>(download);
    const auto shareText = [total](std::int64_t value) {
        if (total <= 0.0) return std::string("—");
        const int percent = static_cast<int>(static_cast<double>(value) / total * 100.0 + 0.5);
        return std::to_string(percent) + "%";
    };
    const auto shareMetric = [](std::string direction, huxerui::Color color,
                                const std::string& share) {
        return huxerui::Text(direction + " " + share).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kCaption)
                .WithWeight(huxerui::FontWeight::SemiBold), color});
    };
    const auto amountMetric = [&theme](std::string direction, huxerui::Color color,
                                       const std::string& value) {
        return huxerui::Row {
            huxerui::Text(direction).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody).WithWeight(huxerui::FontWeight::SemiBold),
                color}),
            huxerui::Text(value).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody).WithWeight(huxerui::FontWeight::SemiBold),
                theme.colors.on_surface}),
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
                .SliceStyle("upload", {.fill = huxerui::Brush(upColor)})
                .SliceStyle("download", {.fill = huxerui::Brush(downColor)})
                .With(huxerui::Frame{.width = 72.0F, .height = 72.0F}),
            huxerui::Column {
                shareMetric("↑", upColor, shareText(upload)),
                shareMetric("↓", downColor, shareText(download)),
            }.With(huxerui::Grow(1.0F),
                   huxerui::Spacing(12.0F),
                   huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
        }.With(huxerui::Grow(1.0F),
               huxerui::Spacing(8.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Column {
            amountMetric("↑", upColor, s.coreReady ? formatBytes(upload) : "—"),
            amountMetric("↓", downColor, s.coreReady ? formatBytes(download) : "—"),
        }.With(huxerui::Spacing(4.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
    }.With(huxerui::Spacing(8.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// 流量卡片：标题行 + 曲线；曲线吃掉卡片剩余高度，任意高度都成立。曲线用
// HuxerUI Lib-Charts（面积图），替掉原先手绘的 Canvas：只保留曲线本身与
// hover tooltip（速率数字在标题行），外观保持「下载实心面积 + 上传细线」。
[[huxerui::composable]] huxerui::View HomeTrafficCard(const HomeState& s) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const bool fresh = huxerui::UseService<CoreModel>()->view.Get().health.traffic.state == health::State::Fresh;
    const auto trafficColors = ResolveTrafficColors(theme);
    const huxerui::Color downColor = trafficColors.download;
    const huxerui::Color upColor = trafficColors.upload;
    huxerui::Color fillDown = downColor;
    fillDown.alpha = 0.18F;  // 与旧手绘面积填充同一透明度

    return huxerui::Column {
        huxerui::Row {
            HomeCardHeading(app::images::speed, Localized("流量")),
            huxerui::Spacer(),
            huxerui::Text(fresh ? "↑ " + formatRate(s.latest.up) : "↑ —")
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption), upColor}),
            huxerui::Text(fresh ? "↓ " + formatRate(s.latest.down) : "↓ —")
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption), downColor}),
        }.With(huxerui::Spacing(12.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::AreaChart(BuildTrafficChartData(
            s.history, huxerui::UseString(Localized("↓ 下载")),
            huxerui::UseString(Localized("↑ 上传"))))
            .Baseline(0.0)
            .Interpolation(huxerui::ChartInterpolation::Linear)
            // 只显示折线：两轴的轴线/刻度数字/网格全部隐藏（Visible(false)
            // 不改变标度域）。Y 轴 formatter 仍供 tooltip 与语义读数使用。
            .XAxis(huxerui::ChartAxis::Linear().Visible(false))
            .YAxis(huxerui::ChartAxis::Linear()
                       .IncludeZero()
                       .Visible(false)
                       .Formatter([](double value) {
                           return formatRate(static_cast<std::int64_t>(value));
                       }))
            .Legend(huxerui::ChartLegendOptions{.visible = false})
            .Accessibility(huxerui::ChartAccessibilityOptions{
                .summary = huxerui::UseString(
                    Localized("上/下行实时速率曲线"))})
            .SeriesStyle("down",
                         huxerui::AreaChartStyle{
                             .fill = huxerui::Brush(fillDown),
                             .stroke = huxerui::Brush(downColor),
                             .stroke_style =
                                 huxerui::StrokeStyle{.width = 2.0F}})
            .SeriesStyle("up",
                         huxerui::AreaChartStyle{
                             .fill =
                                 huxerui::Brush(huxerui::Color::Transparent()),
                             .stroke = huxerui::Brush(upColor),
                             .stroke_style =
                                 huxerui::StrokeStyle{.width = 1.5F}})
            .With(huxerui::Grow(1.0F)),
    }.With(huxerui::Spacing(10.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// 出站模式：纵向单选列表（规则/全局/直连），排布参考 FlClash；单选圈、配色
// 与圆角沿用主题。点击乐观更新，失败做目标值校验后回落。
[[huxerui::composable]] huxerui::View HomeModeCard() {
    const auto control = UseCoreControl();
    const std::size_t selected = HomeModeIndex(control.Mode());

    std::vector<huxerui::StringVariant> labels;
    for (const auto& label : kModeLabels) labels.push_back(Localized(label));
    return huxerui::Column {
        HomeCardHeading(app::images::route, Localized("出站模式")),
        HomeModeChoices(labels, selected, !control.Busy(), [control](std::size_t index) {
            control.Run({.action = CoreAction::Mode, .mode = kModes[index]});
        }),
    }.With(huxerui::Spacing(4.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// 当前订阅卡：标题直接用订阅名（不再放「当前订阅」标签；哪张订阅在用由订阅页
// 的选中态表达，首页只汇报这份订阅自身的状态）。流量行只在解析到
// subscription-userinfo 数据时显示——total 未知（不限量）时进度条摆满；
// 到期时间只在订阅提供了 expire 时显示，否则整行不占位。
[[huxerui::composable]] huxerui::View HomeProfileCard(const HomeState& s) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const bool hasProfile = s.profileId != 0;
    const huxerui::Color muted = theme.colors.on_surface_variant;
    const std::string profileName =
        !s.profilesReady ? "—" : s.profileName == "未启用订阅"
            ? huxerui::UseString(Localized("未启用订阅"))
            : s.profileName;
    const huxerui::StringVariant profileUpdated = [&] {
        if (s.profileUpdated == "未拉取") return Localized("未拉取");
        if (s.profileUpdated == "拉取失败") return Localized("拉取失败");
        constexpr std::string_view prefix = "更新于 ";
        if (s.profileUpdated.starts_with(prefix)) {
            return LocalizedFormat("更新于 {}",
                                   s.profileUpdated.substr(prefix.size()));
        }
        return huxerui::StringVariant(s.profileUpdated);
    }();
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
    const bool hasExpiry = hasProfile && s.profileExpireAt > 0;

    std::vector<huxerui::View> children;
    children.push_back(HomeCardHeading(app::images::request, profileName));
    if (!s.profileUpdated.empty()) {
        children.push_back(
            huxerui::Text(profileUpdated)
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption), muted}));
    }
    if (hasExpiry) {
        children.push_back(
            huxerui::Text(LocalizedFormat(
                "到期时间 {}", formatDate(s.profileExpireAt)))
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption), muted}));
    }
    children.push_back(huxerui::Spacer());
    if (hasTraffic) {
        children.push_back(
            huxerui::Text(trafficLine)
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption), muted}));
    }
    if (hasProfile) {
        children.push_back(
            huxerui::ProgressBar(progress).With(huxerui::Frame{.height = 3.0F}));
    }

    return huxerui::Column(std::move(children))
        .With(huxerui::Spacing(6.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// ---- 网络信息卡片（双平台共用）--------------------------------------------

// 内网 IP：枚举活动接口的本机 IPv4，每 5 秒刷新一次（getifaddrs 级别的
// 便宜系统调用，仍按约定放任务线程）。
[[huxerui::composable]] huxerui::View HomeLanIpCard() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto addresses = huxerui::UseState<std::vector<netinfo::LanAddress>>({});
    auto addressesReady = huxerui::UseState(false);
    huxerui::Lifecycle(
        [tasks, addresses, addressesReady] {
            tasks.Launch([addresses, addressesReady]() -> huxerui::Task<void> {
                for (;;) {
                    auto list = co_await RunOnTaskThread(
                        [] { return netinfo::lanIpv4Addresses(); });
                    addresses = std::move(list);
                    addressesReady = true;
                    co_await huxerui::Delay(std::chrono::duration<double>{5.0});
                }
            });
            return [] {};
        },
        0);

    const std::vector<netinfo::LanAddress>& list = addresses.Get();
    std::vector<huxerui::View> lines;
    // 卡片 1x2（两行高）：标题 + 最多五条明细，更多的接口折叠成「+N」。
    constexpr std::size_t kMaxLines = 5;
    const std::size_t shown = std::min(list.size(), kMaxLines);
    for (std::size_t i = 0; i < shown; ++i) {
        lines.push_back(
            huxerui::Row {
                huxerui::Text(list[i].iface).Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    theme.colors.on_surface_variant}),
                huxerui::Spacer(),
                huxerui::Text(list[i].address).Style(huxerui::TextStyle{
                    huxerui::Font::Monospace(font_size::kChip),
                    theme.colors.on_surface}),
            }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
    }
    if (list.size() > kMaxLines) {
        lines.push_back(huxerui::Text("+" +
                                      std::to_string(list.size() - kMaxLines))
                            .Style(huxerui::TextStyle{
                                huxerui::Font::System(font_size::kCaption),
                                theme.colors.on_surface_variant}));
    }
    if (list.empty()) {
        lines.push_back(huxerui::Text(addressesReady.Get() ? Localized("未检测到内网地址") : huxerui::StringVariant("—"))
                            .Style(huxerui::TextStyle{
                                huxerui::Font::System(font_size::kCaption),
                                theme.colors.on_surface_variant}));
    }

    return huxerui::Column {
        HomeCardHeading(app::images::network, Localized("内网 IP")),
        huxerui::Column(std::move(lines))
            .With(huxerui::Spacing(3.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
        huxerui::Spacer(),
    }.With(huxerui::Spacing(4.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// ---- 平台专属卡片（整个函数由编译宏在调用点选择）---------------------------

#if !defined(__ANDROID__)

[[huxerui::composable]] huxerui::View DesktopModeSwitchRow(
    huxerui::ImageVariant icon, huxerui::StringVariant label,
    huxerui::StringVariant description, huxerui::View control) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Column {
        huxerui::Row {
            huxerui::Image(icon)
                .Fit(huxerui::ImageFit::Contain)
                .Tint(theme.colors.primary)
                .With(huxerui::Frame{.width = 20.0F, .height = 20.0F}),
            huxerui::Text(label).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody)
                    .WithWeight(huxerui::FontWeight::SemiBold),
                theme.colors.on_surface}),
        }.With(huxerui::Spacing(8.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Row {
            huxerui::Text(description).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption),
                theme.colors.on_surface_variant})
                .With(huxerui::Grow(1.0F)),
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
            Localized("应用代理"),
            huxerui::Switch(control.SystemProxyEnabled())
                .With(huxerui::Enabled(!control.Busy()))
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
            app::images::tun, Localized("TUN 模式"),
            Localized("全局接管"),
            huxerui::Switch(control.TunEnabled())
                .With(huxerui::Enabled(!control.Busy()))
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
    if (action == CoreAction::Stop)
        AnimateHomeControlCard(false, cardShown, motion, transition, control.tasks, reducedMotion);
    control.Run({.action = action});
}

BoxCatPhase DesktopHomeCatPhase(const CoreControl& control,
                                CoreAction presented = CoreAction::Idle) {
    // A Start presentation now means the successful cat is merging into its
    // card, rather than a kernel command that is still waiting to finish.
    if (presented == CoreAction::Start) return BoxCatPhase::Running;
    const CoreAction action = presented == CoreAction::Idle
        ? control.model->operations.command.Get().action : presented;
    if (action == CoreAction::Start) return BoxCatPhase::Starting;
    if (action == CoreAction::Stop) return BoxCatPhase::Stopping;
    switch (control.model->view.Get().core.state) {
    case core::CoreState::Starting: return BoxCatPhase::Starting;
    case core::CoreState::Running: return BoxCatPhase::Running;
    case core::CoreState::Failed: return BoxCatPhase::Failed;
    case core::CoreState::Stopped: return BoxCatPhase::Sleeping;
    }
    return BoxCatPhase::Sleeping;
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
    const CoreAction action = control.model->operations.command.Get().action;
    const bool entering = motion.Get().action == CoreAction::Start;
    const bool returning = motion.Get().action == CoreAction::Stop;
    // Shared-bounds playback keeps the destination paint-stable; the cat's
    // retained clocks resume as soon as the outer transition finishes.
    const bool changing = !entering && (action == CoreAction::Start ||
                                        action == CoreAction::Stop);
    const huxerui::StringVariant status = Localized(
        action == CoreAction::Start ? "正在启动"
        : returning || action == CoreAction::Stop ? "正在停止"
                                     : "运行中");
    const auto runningSince = control.model->view.Get().core.runningSince;
    const auto elapsed = runningSince == std::chrono::steady_clock::time_point{}
        ? std::chrono::seconds{0}
        : std::max(std::chrono::seconds{0},
                   std::chrono::duration_cast<std::chrono::seconds>(
                       clock.Get() - runningSince));
    const auto seconds = elapsed.count();
    const std::string uptime = FormatHomeUptime(seconds);
    huxerui::View icon = BoxCatStatusIcon(
        DesktopHomeCatPhase(control, motion.Get().action),
        motion.Get().action == CoreAction::Idle);
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
    return huxerui::View{HomeCardSurface(theme, std::move(content), "control", /*allowFlip=*/false)}
        .OnClick([control, cardShown, motion, transition, reducedMotion = theme.motion.reduced_motion] {
            RunDesktopHomeCoreAction(control, cardShown, motion, transition,
                                     CoreAction::Stop, reducedMotion);
        })
        .With(huxerui::Scale(huxerui::AnimateTo(
                  motion.Get().collapsing ? kHomeControlCollapsedScale : 1.0F,
                  kHomeControlCollapseMotion)),
              huxerui::Enabled(!control.Busy() && !entering && !returning),
              huxerui::Tooltip(Localized("停止内核")),
              huxerui::Indication{},
              huxerui::SharedBounds("home-core-control")
                  .BoundsTransform(HomeControlMoveExpand{}),
              huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                 .label = huxerui::UseString(
                                     Localized("停止内核"))});
}

// 桌面宽窗：内核猫咪并入网格卡片；Compact 与移动端共用标题栏组件。
// 「正式启动」是独立动作：只负责拉起/停止内核，启动时按已记录的 TUN 与
// 系统代理意图恢复；不再由各个开关隐式触发内核。
[[huxerui::composable]] huxerui::View DesktopHomeFloatingAction(
    huxerui::View page, huxerui::State<bool> cardShown,
    huxerui::State<HomeControlMotion> motion,
    huxerui::State<bool> catReady,
    huxerui::SharedTransitionHandle transition) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const auto control = UseCoreControl();
    huxerui::View base = page;
    const bool returning = motion.Get().action == CoreAction::Stop;
    const bool active = returning ? false : control.Running();
    const bool enabled = !control.Busy() && !returning;
    const CoreAction command = control.model->operations.command.Get().action;
    const bool changing = !returning && (command == CoreAction::Start ||
                                        command == CoreAction::Stop);
    const auto toggle = [control, active, cardShown, motion, transition,
                         reducedMotion = theme.motion.reduced_motion] {
        RunDesktopHomeCoreAction(control, cardShown, motion, transition,
            active ? CoreAction::Stop : CoreAction::Start, reducedMotion);
    };

    const auto onRunningReady = [control, catReady] {
        const auto action = control.model->operations.command.Get().action;
        if (action != CoreAction::Start && action != CoreAction::Stop &&
            action != CoreAction::Restart &&
            control.model->view.Get().core.state == core::CoreState::Running)
            catReady = true;
    };
    huxerui::View button = BoxCatButtonContent(
        DesktopHomeCatPhase(control, motion.Get().action),
        theme.colors.on_surface,
        motion.Get().action == CoreAction::Idle, {}, onRunningReady,
        returning ? std::optional{BoxCatPhase::Running} : std::nullopt).OnClick(toggle)
        .With(huxerui::Tooltip(Localized(active ? "停止内核" : "启动内核")),
              huxerui::Enabled(enabled),
              huxerui::Focusable(true),
              huxerui::Indication{},
              huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                 .label = huxerui::UseString(Localized(
                                     active ? "停止内核" : "启动内核"))});
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
                 .right = theme.spacing.medium,
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
    HomeCardKind kind, huxerui::State<HomeState> state) {
    const HomeState& s = state.Get();
    if (kind == HomeCardKind::Traffic) return HomeTrafficCard(s);
    if (kind == HomeCardKind::Total) return HomeTotalCard(s);
    if (kind == HomeCardKind::Mode) {
        return HomeModeCard();
    }
    if (kind == HomeCardKind::Profile) return HomeProfileCard(s);
    if (kind == HomeCardKind::LanIp) return HomeLanIpCard();
    return CLASHFLUX_HOME_PLATFORM_CARD(s, state, kind);
}

} // namespace

[[huxerui::composable]] huxerui::View HomePage(
    huxerui::State<std::size_t> navPage, bool active) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    // 策略组快照 / 内核与接管状态的全局唯一来源（见 *_model.h）。
    const auto coreModel = huxerui::UseService<CoreModel>();
    const auto profilesModel = huxerui::UseService<ProfilesModel>();
    auto state = huxerui::UseState<HomeState>({});
    auto controlCardShown = huxerui::UseState<bool>(
        coreModel->view.Get().core.state == core::CoreState::Running);
    auto controlMotion = huxerui::UseState<HomeControlMotion>({});
    auto controlCardSynced = huxerui::UseState<bool>(false);
    auto controlCatReady = huxerui::UseState<bool>(false);
    const auto controlTransition = huxerui::UseSharedTransition();
    const bool headerControlEnabled = !kPageTitlesInWindow || compact;
    const auto control = UseCoreControl();
    const auto& coreView = coreModel->view.Get();
    const auto vpnStatus = ResolveHomeVpnStatus(coreView.vpnState, coreModel->operations.command.Get());
    const auto action = coreModel->operations.command.Get().action;
    const auto headerPhase = kDesktopHomeControl
        ? (action == CoreAction::Stop ? BoxCatPhase::Stopping
            : action == CoreAction::Start || action == CoreAction::Restart ? BoxCatPhase::Starting
            : coreView.core.state == core::CoreState::Running ? BoxCatPhase::Running
            : coreView.core.state == core::CoreState::Starting ? BoxCatPhase::Starting
            : coreView.core.state == core::CoreState::Failed ? BoxCatPhase::Failed : BoxCatPhase::Sleeping)
        : vpnStatus.phase;
    const auto headerControl = UseHomeHeaderControl(HomeHeaderSnapshot{
        .ready = coreView.ready && (kDesktopHomeControl || coreView.vpnSnapshotReady),
        .active = active && headerControlEnabled,
        .running = kDesktopHomeControl ? coreView.core.state == core::CoreState::Running : vpnStatus.connected,
        .busy = control.Busy(),
        .canStart = kDesktopHomeControl || state.Get().profileId != 0,
        .phase = headerPhase,
        .elapsedSeconds = coreView.vpnElapsedMs / 1000,
        .runningSince = kDesktopHomeControl ? coreView.core.runningSince
            : std::chrono::steady_clock::time_point{},
    });
    const auto toggleHeader = [control] {
        if (kDesktopHomeControl) {
            control.Run({.action = control.Running() ? CoreAction::Stop : CoreAction::Start});
        } else {
            control.Run({.action = CoreAction::Tun, .enabled = control.model->view.Get().vpnState != 2});
        }
    };
    const auto headerStopLabel = Localized(kDesktopHomeControl ? "停止内核" : "停止 VPN");
    const auto headerActionLabel = headerControl.latest.Get().running ? headerStopLabel
        : Localized(kDesktopHomeControl ? "启动内核" : "启动 VPN");

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
                const CoreView& view = coreModel->view.Get();
                if (view.ready) {
                    const bool running =
                        view.core.state == core::CoreState::Running;
                    if (!controlCardSynced.Get() || !active || compact) {
                        controlCardSynced = true;
                        controlCardShown = running;
                        controlCatReady = running;
                    } else {
                        const auto command = coreModel->operations.command.Get().action;
                        if (!running || command == CoreAction::Start ||
                            command == CoreAction::Stop || command == CoreAction::Restart)
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
            coreModel->view, coreModel->operations.command, controlCatReady,
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
        huxerui::View body = HomeCardContent(kind, state);
        huxerui::View cardView = HomeCardSurface(theme, std::move(body), id);
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
    bodySections.push_back(
        // 悬浮启动按钮压在滚动内容之上，尾部留出等高的空白，避免最后一张
        // 卡片被按钮遮住（紧凑视口本来就带底部悬浮导航的空白）。
        compact ? CompactFloatingNavigationFooter()
                : huxerui::View{huxerui::Row{}.With(
                      huxerui::Frame{.height =
                          kDesktopHomeControl && controlCardShown.Get()
                              ? 0.0F : kHomeFloatingButtonInset})});
    huxerui::View pageBody = huxerui::Column(std::move(bodySections))
        .With(huxerui::Spacing(kHomeGridGap),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    // 注意：首页不要在滚动内容里声明 Focusable(true)——运行时会自动把初始
    // 焦点所在的节点滚入视野，导致首页一打开就被滚到中途（且随后每次重组都可能
    // 来回滚）。可聚焦性交给内置控件自己的默认策略，分段选择器只保留语义。
    // Runtime effects remain mounted independently from the replaceable header.
    huxerui::View scrollContent = huxerui::Stack {
        std::move(runtimePump),
        huxerui::ScrollView(std::move(pageBody)).With(huxerui::Grow(1.0F)),
    }.With(huxerui::Grow(1.0F));
    huxerui::View page = PageScaffold(
        Localized("首页"), {}, std::move(scrollContent), true, false, true,
        kHomeGridGap, {}, {}, true,
        headerControlEnabled ? HomeHeaderTitle(headerControl, toggleHeader, headerStopLabel) : huxerui::View{});

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
