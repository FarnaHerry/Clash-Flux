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
#include "profiles_model.h"
#include "settings_model.h"
#include "stream_updates.h"

namespace clashflux::ui {
namespace {

constexpr std::size_t kHistoryPoints = 60;  // 60 拍 ≈ 30s 窗口
constexpr huxerui::TweenSpec kHomeControlMotion{
    .duration = 0.42, .easing = huxerui::Easing::EaseInOut};

const std::vector<std::string> kModeLabels{"规则", "全局", "直连"};
const std::vector<std::string> kModes{"rule", "global", "direct"};

std::size_t HomeModeIndex(const std::string& mode) {
    for (std::size_t index = 0; index < kModes.size(); ++index) {
        if (mode == kModes[index]) return index;
    }
    return 0;
}

// ---- 卡片种类与平台目录 ----------------------------------------------------

enum class HomeCardKind {
    Traffic,    // 流量曲线（标题行带实时上/下行速率）
    Total,      // 流量统计（总上传 / 总下载）
    Mode,       // 出站模式
    Profile,    // 当前订阅
    Proxy,      // 系统代理（桌面）
    Tun,        // TUN 模式（桌面）
    LanIp,      // 内网 IP（本机局域网地址）
};

struct HomeCardSpec {
    HomeCardKind kind;
    std::string_view id;     // 持久化身份，不要改名（改名会让旧布局回落默认）
    std::string_view title;  // 编辑态标题
    int width = 1;           // 默认占用列数 1..4
    int height = 1;          // 默认占用行数 1..4
};

#if defined(__ANDROID__)
// 移动端由悬浮按钮展示隧道状态，不再重复一张状态卡片。
// 手机屏是 2 列：需要整宽的（图表）给 2 格，信息/单选卡 1 格；
// 高度仍按 FlClash 跨度（图表/信息卡/出站模式 2 行，开关与网络信息卡 1 行）。
constexpr HomeCardSpec kHomeCards[] = {
    // Lib-Charts 的紧凑绘图面最小 240×96，适配 2 行（188pt）的卡片。
    {HomeCardKind::Traffic, "traffic", "流量曲线", 2, 2},
    {HomeCardKind::Total, "total", "流量统计", 1, 2},
    // 出站模式是纵向单选列表（标题 + 3 个选项），占 1x2。
    {HomeCardKind::Mode, "mode", "出站模式", 1, 2},
    {HomeCardKind::Profile, "profile", "当前订阅", 1, 2},
    // 内网 IP 紧跟当前订阅，1x2：两行高度可容纳更多接口明细。
    {HomeCardKind::LanIp, "lanip", "内网 IP", 1, 2},
};
constexpr bool kDesktopHomeControl = false;
#define CLASHFLUX_HOME_PLATFORM_CARD(homeState, state, kind) \
    huxerui::Row{}
#define CLASHFLUX_HOME_CONTROL_CARD() huxerui::Row{}
#define CLASHFLUX_HOME_FLOATING_ACTION(page, state, compact, shown, transition) \
    AndroidHomeFloatingAction(std::move(page), state, compact)
#else
// 桌面可选卡片：系统代理/TUN 开关是桌面专有能力。
//
// 初始宽高按 FlClash 仪表盘实测跨度（其 3 列网格：单元 264x79.5、间距 14 逻辑 px）：
//   网络速度 2x2 → 流量曲线 2x2   流量统计 1x2 → 流量统计 1x2
//   系统代理 1x1 → 系统代理 1x1   TUN 1x1      → TUN 模式 1x1
//   出站模式(竖排) 1x2 → 出站模式取 1x2（纵向单选列表）
//   其余信息类卡片（当前订阅）与 FlClash 的信息卡同级，取 1x2。
constexpr HomeCardSpec kHomeCards[] = {
    // 与移动端共用紧凑绘图面，流量卡片高度降为 2 行。
    {HomeCardKind::Traffic, "traffic", "流量曲线", 2, 2},
    {HomeCardKind::Total, "total", "流量统计", 1, 2},
    {HomeCardKind::Mode, "mode", "出站模式", 1, 2},
    {HomeCardKind::Profile, "profile", "当前订阅", 1, 2},
    // 内网 IP 紧跟当前订阅，1x2：两行高度可容纳更多接口明细。
    {HomeCardKind::LanIp, "lanip", "内网 IP", 1, 2},
    {HomeCardKind::Proxy, "proxy", "系统代理", 1, 1},
    {HomeCardKind::Tun, "tun", "TUN 模式", 1, 1},
};
constexpr bool kDesktopHomeControl = true;
#define CLASHFLUX_HOME_PLATFORM_CARD(homeState, state, kind) \
    DesktopHomePlatformCard(kind)
#define CLASHFLUX_HOME_CONTROL_CARD() DesktopHomeStatusCard()
#define CLASHFLUX_HOME_FLOATING_ACTION(page, state, compact, shown, transition) \
    DesktopHomeFloatingAction(std::move(page), compact, shown, transition)
#endif

// ---- 卡片网格尺寸模型 ------------------------------------------------------

// 卡片声明的宽高（单位格，各 1..4）。宽会被页面当前列数收窄，高不随页面变化。
struct HomeCardSize {
    int width = 1;
    int height = 1;

    bool operator==(const HomeCardSize&) const = default;
};

// 布局值 key：卡片在 HomeGrid 中占用的格数；Value 用独立类型，避免与其他
// 布局值（Grow/Spacing 等）的语义混淆。
struct HomeCardSpan {
    using Value = HomeCardSize;
};

struct HomeControlSpan {
    using Value = bool;
};

struct HomeGridCompact {
    using Value = bool;
};

struct HomeCardEntry {
    HomeCardKind kind = HomeCardKind::Traffic;
    HomeCardSize size;

    bool operator==(const HomeCardEntry&) const = default;
};

using HomeLayout = std::vector<HomeCardEntry>;

// 首页右下角悬浮启动按钮的占位高度（滚动内容尾部留白）。
constexpr float kHomeFloatingButtonInset = 72.0F;

constexpr int kHomeGridMaxSpan = 4;   // 卡片宽高上限（格）
constexpr float kHomeGridGap = 12.0F; // 卡片间距，同时用于首页顶部与左右留白
// 一行单位高度：h=1 的卡片刚好容纳标题 + 一行控件。
constexpr float kHomeGridUnitHeight = 88.0F;
constexpr float kHomeGridFallbackWidth = 360.0F;

// 两列与页面的 Compact 切换同步；Medium/Expanded 至少三列，
// 更宽的内容区仍可扩展到五列。
constexpr int HomeGridColumns(float width, bool compact) {
    if (compact) return 2;
    if (width < 1200.0F) return 3;
    return 5;
}

// 自定义网格布局：按声明顺序把每张卡片放进第一个放得下的空位（左上紧凑），
// 宽度按当前列数均分。卡片尺寸由布局给（不是内容撑开），因此编辑态与运行态
// 占据完全相同的大小。
class HomeGrid final : public huxerui::Layout<HomeGrid> {
public:
    using Layout::Layout;

    static huxerui::LayoutResult Measure(huxerui::LayoutContext& context,
                                         huxerui::ViewNode& node,
                                         huxerui::Constraints constraints);
};

huxerui::LayoutResult HomeGrid::Measure(huxerui::LayoutContext& context,
                                        huxerui::ViewNode& node,
                                        huxerui::Constraints constraints) {
    huxerui::LayoutResult result;
    const float available = constraints.HasBoundedWidth()
                                ? constraints.max_width
                                : kHomeGridFallbackWidth;
    const int columns = HomeGridColumns(
        available, node.LayoutValueOr<HomeGridCompact>(false));
    const float column_width =
        std::max(0.0F, (available - kHomeGridGap * static_cast<float>(columns - 1)) /
                           static_cast<float>(columns));

    struct Placement {
        huxerui::ViewNode* child;
        int width;
        int height;
        int row;
        int column;
    };

    std::vector<Placement> placements;
    placements.reserve(node.Children().Size());
    for (huxerui::ViewNode& child : node.Children()) {
        HomeCardSize span = child.LayoutValueOr<HomeCardSpan>(HomeCardSize{});
        if (child.LayoutValueOr<HomeControlSpan>(false)) {
            span.width = columns >= 5 ? 2 : 1;
        }
        placements.push_back(Placement{
            &child,
            std::clamp(span.width, 1, columns),
            std::clamp(span.height, 1, kHomeGridMaxSpan),
            0,
            0,
        });
    }

    // 二维占位表：行按需增长（外层高度无界，不能依赖 constraints.max_height）。
    std::vector<std::vector<bool>> occupied;
    const auto ensure_rows = [&occupied, columns](int rows) {
        while (static_cast<int>(occupied.size()) < rows) {
            occupied.emplace_back(static_cast<std::size_t>(columns), false);
        }
    };
    const auto fits = [&occupied](int row, int column, int width, int height) {
        for (int r = row; r < row + height; ++r) {
            const std::vector<bool>& line =
                occupied[static_cast<std::size_t>(r)];
            for (int c = column; c < column + width; ++c) {
                if (line[static_cast<std::size_t>(c)]) return false;
            }
        }
        return true;
    };

    int total_rows = 0;
    for (Placement& placement : placements) {
        bool placed = false;
        for (int row = 0; !placed; ++row) {
            ensure_rows(row + placement.height);
            for (int column = 0; column + placement.width <= columns; ++column) {
                if (!fits(row, column, placement.width, placement.height)) {
                    continue;
                }
                placement.row = row;
                placement.column = column;
                for (int r = row; r < row + placement.height; ++r) {
                    for (int c = column; c < column + placement.width; ++c) {
                        occupied[static_cast<std::size_t>(r)]
                                [static_cast<std::size_t>(c)] = true;
                    }
                }
                total_rows = std::max(total_rows, row + placement.height);
                placed = true;
                break;
            }
        }
    }

    for (const Placement& placement : placements) {
        const float width =
            column_width * static_cast<float>(placement.width) +
            kHomeGridGap * static_cast<float>(placement.width - 1);
        const float height =
            kHomeGridUnitHeight * static_cast<float>(placement.height) +
            kHomeGridGap * static_cast<float>(placement.height - 1);
        static_cast<void>(context.Measure(
            *placement.child,
            huxerui::Constraints{width, width, height, height}));
        result.Place(*placement.child,
                     huxerui::Point{
                         (column_width + kHomeGridGap) *
                             static_cast<float>(placement.column),
                         (kHomeGridUnitHeight + kHomeGridGap) *
                             static_cast<float>(placement.row),
                     });
    }

    const float total_height =
        total_rows == 0
            ? 0.0F
            : kHomeGridUnitHeight * static_cast<float>(total_rows) +
                  kHomeGridGap * static_cast<float>(total_rows - 1);
    return result.SetSize(constraints.Constrain({available, total_height}));
}

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
    updateRuntime(s, core);

    // 选中订阅来自 ProfilesModel：直接扫它的列表（借用引用，不做整表拷贝），
    // 不再每次调用 profilesStore().selected()。
    const db::Profile* selected = nullptr;
    for (const db::Profile& candidate : profilesModel->list.Get()) {
        if (candidate.selected && candidate.type != "pptp" &&
            candidate.type != "openvpn") {
            selected = &candidate;
            break;
        }
    }
    if (selected != nullptr) {
        s.profileId = selected->id;
        s.profileName = selected->name;
        s.profileUsedBytes = selected->usedBytes;
        s.profileTotalBytes = selected->totalBytes;
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
        coreModel->view, profilesModel->list);
    return {};
}
#endif
#define CLASHFLUX_HOME_RUNTIME_PUMP HomeRuntimePump

// ---- 通用卡片部件 ----------------------------------------------------------

// 卡片标题：所有卡片共用同一排版；卡片外壳由卡片槽统一提供。
// 首页卡片没有选中态（「当前订阅」也是普通卡片），标题一律用 on_surface。
[[huxerui::composable]] huxerui::View HomeCardHeading(
    huxerui::StringVariant title) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Text(title).Style(huxerui::TextStyle{
        huxerui::Font::System(font_size::kBody)
            .WithWeight(huxerui::FontWeight::SemiBold),
        theme.colors.on_surface});
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

// 流量曲线：下载面积图（主色渐变填充）+ 上传折线（琥珀）。历史不足两点画平线。
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

// 流量统计：箭头与占比同列，累计量在下一行；饼图仍区分两个方向。
[[huxerui::composable]] huxerui::View HomeTotalCard(const HomeState& s) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const huxerui::Color upColor = SemanticWarningColor(theme);
    const huxerui::Color downColor = theme.colors.primary;
    const std::int64_t upload = std::max<std::int64_t>(0, s.totalUp);
    const std::int64_t download = std::max<std::int64_t>(0, s.totalDown);
    const double total = static_cast<double>(upload) + static_cast<double>(download);
    const auto shareText = [total](std::int64_t value) {
        if (total <= 0.0) return std::string("—");
        const int percent = static_cast<int>(static_cast<double>(value) / total * 100.0 + 0.5);
        return std::to_string(percent) + "%";
    };
    const auto totalMetric = [&theme](std::string direction, huxerui::Color color,
                                      const std::string& value,
                                      const std::string& share) {
        return huxerui::Column {
            huxerui::Text(direction + " " + share).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption)
                    .WithWeight(huxerui::FontWeight::SemiBold),
                color}),
            huxerui::Text(value).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody)
                    .WithWeight(huxerui::FontWeight::SemiBold),
                theme.colors.on_surface}),
        }.With(huxerui::Spacing(2.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    };
    const huxerui::PieChartData data({
        {"upload", "↑",
         static_cast<double>(upload)},
        {"download", "↓",
         static_cast<double>(download)},
    });

    // fork 自带圆角段、中心占比与悬停动画，不再覆盖一份静态中心文字。
    return huxerui::Column {
        HomeCardHeading(Localized("流量统计")),
        huxerui::Row {
            huxerui::PieChart(data)
                .Style(huxerui::PieChartStyle{.inner_radius = 0.62F, .rounded_caps = true})
                .SliceStyle("upload", {.fill = huxerui::Brush(upColor)})
                .SliceStyle("download", {.fill = huxerui::Brush(downColor)})
                .With(huxerui::Frame{.width = 72.0F, .height = 72.0F}),
            huxerui::Column {
                totalMetric("↑", upColor, formatBytes(upload), shareText(upload)),
                totalMetric("↓", downColor, formatBytes(download), shareText(download)),
            }.With(huxerui::Grow(1.0F),
                   huxerui::Spacing(12.0F),
                   huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
        }.With(huxerui::Grow(1.0F),
               huxerui::Spacing(8.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
    }.With(huxerui::Spacing(8.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// 流量卡片：标题行 + 曲线；曲线吃掉卡片剩余高度，任意高度都成立。曲线用
// HuxerUI Lib-Charts（面积图），替掉原先手绘的 Canvas：只保留曲线本身与
// hover tooltip（速率数字在标题行），外观保持「下载实心面积 + 上传细线」。
[[huxerui::composable]] huxerui::View HomeTrafficCard(const HomeState& s) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const huxerui::Color downColor = theme.colors.primary;
    const huxerui::Color upColor = SemanticWarningColor(theme);
    huxerui::Color fillDown = theme.colors.primary;
    fillDown.alpha = 0.18F;  // 与旧手绘面积填充同一透明度

    return huxerui::Column {
        huxerui::Row {
            HomeCardHeading(Localized("流量")),
            huxerui::Spacer(),
            huxerui::Text("↑ " + formatRate(s.latest.up))
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption), upColor}),
            huxerui::Text("↓ " + formatRate(s.latest.down))
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
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const auto control = UseCoreControl();
    const std::size_t selected = HomeModeIndex(control.Mode());

    huxerui::Color hoverFill = theme.colors.on_surface;
    hoverFill.alpha = 0.08F;
    huxerui::Color pressFill = theme.colors.on_surface;
    pressFill.alpha = 0.14F;
    const huxerui::Indication indication{
        .hover = huxerui::IndicationLayer{
            .fill = huxerui::VisualFill{huxerui::Brush{hoverFill}}},
        .press = huxerui::IndicationLayer{
            .fill = huxerui::VisualFill{huxerui::Brush{pressFill}}},
    };

    std::vector<huxerui::View> options;
    options.reserve(kModeLabels.size());
    for (std::size_t i = 0; i < kModeLabels.size(); ++i) {
        const bool active = i == selected;
        // 单选圈：18pt 外环（选中 primary / 未选 outline），选中时中心实心点。
        huxerui::View dot =
            active ? huxerui::View{huxerui::Row{}.With(
                         huxerui::Frame{.width = 8.0F, .height = 8.0F},
                         huxerui::CornerRadius(4.0F),
                         huxerui::Background(theme.colors.primary))}
                   : huxerui::View{huxerui::Row{}};
        huxerui::View ring = huxerui::Column {
            std::move(dot),
        }.With(huxerui::Frame{.width = 18.0F, .height = 18.0F},
               huxerui::CornerRadius(9.0F),
               huxerui::Border(active ? theme.colors.primary
                                      : theme.colors.outline,
                               2.0F),
               huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
        options.push_back(
            huxerui::Row {
                std::move(ring),
                huxerui::Text(Localized(kModeLabels[i])).Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kBody)
                        .WithWeight(active ? huxerui::FontWeight::SemiBold
                                           : huxerui::FontWeight::Regular),
                    active ? theme.colors.primary
                           : theme.colors.on_surface}),
            }
                .With(huxerui::Spacing(10.0F),
                      huxerui::Grow(1.0F),
                      huxerui::Padding(
                          huxerui::EdgeInsets::Symmetric(6.0F, 4.0F)),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                      huxerui::CornerRadius(8.0F),
                      huxerui::ClipChildren(),
                      indication,
                      huxerui::Semantics{
                          .role = huxerui::SemanticRole::RadioButton,
                          .label = Localized(kModeLabels[i]),
                          .selected = active})
                .With(huxerui::Enabled(!control.Busy()))
                .OnClick([control, i] {
                    control.Run({.action = CoreAction::Mode, .mode = kModes[i]});
                })
                .Key("home-mode-" + std::to_string(i)));
    }

    return huxerui::Column {
        HomeCardHeading(Localized("出站模式")),
        huxerui::Column(std::move(options))
            .With(huxerui::Spacing(2.0F),
                  huxerui::Grow(1.0F),
                  huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
    }.With(huxerui::Spacing(4.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// 当前订阅卡：普通首页卡片，和别的卡片一样是 raised 表面 + on_surface 文字。
// 「哪个订阅在使用中」由订阅页承担（那边是 primary 实心底的选中卡），首页不再
// 复刻一套选中态/进度条配色。
[[huxerui::composable]] huxerui::View HomeProfileCard(const HomeState& s) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const bool hasProfile = s.profileId != 0;
    const huxerui::Color fg = theme.colors.on_surface;
    const huxerui::Color muted = theme.colors.on_surface_variant;
    const std::string profileName =
        s.profileName == "未启用订阅"
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
    const float progress =
        s.profileTotalBytes > 0
            ? std::clamp(static_cast<float>(s.profileUsedBytes) /
                             static_cast<float>(s.profileTotalBytes),
                         0.0F, 1.0F)
            : (hasProfile ? 1.0F : 0.0F);

    return huxerui::Column {
        HomeCardHeading(Localized("当前订阅")),
        huxerui::Text(profileName).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kBody), fg}),
        s.profileUpdated.empty()
            ? huxerui::View{huxerui::Row{}}
            : huxerui::View{
                  huxerui::Text(profileUpdated)
                      .Style(huxerui::TextStyle{
                          huxerui::Font::System(font_size::kCaption), muted})},
        huxerui::Spacer(),
        hasProfile ? huxerui::View{huxerui::ProgressBar(progress)
                                       .With(huxerui::Frame{.height = 3.0F})}
                   : huxerui::View{huxerui::Row{}},
    }.With(huxerui::Spacing(6.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// ---- 网络信息卡片（双平台共用）--------------------------------------------

// 内网 IP：枚举活动接口的本机 IPv4，每 5 秒刷新一次（getifaddrs 级别的
// 便宜系统调用，仍按约定放任务线程）。
[[huxerui::composable]] huxerui::View HomeLanIpCard() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto addresses = huxerui::UseState<std::vector<netinfo::LanAddress>>({});
    huxerui::Lifecycle(
        [tasks, addresses] {
            tasks.Launch([addresses]() -> huxerui::Task<void> {
                for (;;) {
                    auto list = co_await RunOnTaskThread(
                        [] { return netinfo::lanIpv4Addresses(); });
                    addresses = std::move(list);
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
        lines.push_back(huxerui::Text(Localized("未检测到内网地址"))
                            .Style(huxerui::TextStyle{
                                huxerui::Font::System(font_size::kCaption),
                                theme.colors.on_surface_variant}));
    }

    return huxerui::Column {
        HomeCardHeading(Localized("内网 IP")),
        huxerui::Column(std::move(lines))
            .With(huxerui::Spacing(3.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
        huxerui::Spacer(),
    }.With(huxerui::Spacing(4.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// ---- 平台专属卡片（整个函数由编译宏在调用点选择）---------------------------

#if defined(__ANDROID__)

[[huxerui::composable]] huxerui::View AndroidHomeFloatingAction(
    huxerui::View page, huxerui::State<HomeState> homeState, bool compact) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const HomeState& state = homeState.Get();
    const auto control = UseCoreControl();
    huxerui::View base = page;
    const int vpnStatus = control.model->view.Get().vpnState;
    const bool active = vpnStatus == 2;
    const CoreCommand command = control.model->operations.command.Get();
    const bool changing = command.action == CoreAction::Tun || vpnStatus == 1;
    const bool failed = vpnStatus == 3 && !changing;
    const bool enabled = !control.Busy() && !changing &&
                         (active || state.profileId != 0);
    const auto toggle = [control, active] {
        control.Run({.action = CoreAction::Tun, .enabled = !active});
    };

    const huxerui::StringVariant actionLabel =
        Localized(active ? "停止 VPN" : "启动 VPN");
    const huxerui::StringVariant statusLabel =
        Localized(command.action == CoreAction::Tun && !command.enabled
                      ? "正在断开"
                      : changing ? "正在连接"
                      : failed ? "启动失败"
                               : active ? "已连接" : "未连接");
    // 过程态保留按钮的位置和尺寸，进度环替代图标；结果由后台快照确认。
    huxerui::View button =
        huxerui::IconButton(active ? app::images::pause : app::images::play,
                            actionLabel)
            .OnClick(toggle)
            .With(huxerui::Tooltip(actionLabel),
                  huxerui::Frame{.width = 56.0F, .height = 56.0F},
                  huxerui::Enabled(enabled),
                  huxerui::Opacity(changing ? 0.0F : 1.0F),
                  huxerui::Foreground(failed ? OnErrorColor(theme)
                      : active ? theme.colors.on_primary_container
                               : theme.colors.on_primary),
                  huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                     .label = huxerui::UseString(actionLabel)});
    button = WithoutIconButtonOutlines(button);
    huxerui::View floating = huxerui::Stack {
        std::move(button),
        changing ? huxerui::View{huxerui::ProgressCircle().With(
                       huxerui::Frame{.width = 28.0F, .height = 28.0F})}
                 : huxerui::View{huxerui::Row{}},
    }.With(huxerui::Frame{.width = 56.0F, .height = 56.0F},
           huxerui::Align(huxerui::HorizontalAlignment::Center,
                          huxerui::VerticalAlignment::Center),
           huxerui::Background(failed ? theme.colors.error
               : active || changing ? theme.colors.primary_container
                                    : theme.colors.primary),
           huxerui::CornerRadius(kFloatingActionCornerRadius),
           huxerui::Shadow{ThemeShadowColor(theme, 0.28F), {}, 14.0F, 2.0F},
           huxerui::Scale(huxerui::AnimateTo(
               changing ? 0.94F : 1.0F,
               huxerui::TweenSpec{.duration = 0.18,
                                  .easing = huxerui::Easing::EaseOut})),
           huxerui::Semantics{.label = huxerui::UseString(statusLabel)});
    // 与 Android 底部悬浮导航栏相同：按钮放在独立的全屏覆盖层中，
    // 由覆盖层的 Column 在主轴末端、交叉轴末端定位，不依赖页面内容容器。
    huxerui::View dock = huxerui::Column {
        std::move(floating),
    }.With(huxerui::Padding(huxerui::EdgeInsets{
                 .right = theme.spacing.medium,
                 .bottom = compact ? kCompactFloatingNavigationInset : theme.spacing.medium,
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

struct HomeControlMoveExpand {
    static constexpr float kMoveFraction = 0.48F;

    static huxerui::Rect Expand(huxerui::Rect button,
                                huxerui::Rect card, float progress) {
        const float move = std::clamp(progress / kMoveFraction, 0.0F, 1.0F);
        const float grow = std::clamp((progress - kMoveFraction) /
                                          (1.0F - kMoveFraction), 0.0F, 1.0F);
        const float buttonCenterX = button.x + button.width / 2.0F;
        const float buttonCenterY = button.y + button.height / 2.0F;
        const float cardCenterX = card.x + card.width / 2.0F;
        const float cardCenterY = card.y + card.height / 2.0F;
        const float centerX = buttonCenterX + (cardCenterX - buttonCenterX) * move;
        const float centerY = buttonCenterY + (cardCenterY - buttonCenterY) * move;
        const float width = button.width + (card.width - button.width) * grow;
        const float height = button.height + (card.height - button.height) * grow;
        return {centerX - width / 2.0F, centerY - height / 2.0F,
                width, height};
    }

    huxerui::Rect Evaluate(huxerui::Rect from, huxerui::Rect to,
                           float progress) const {
        if (progress <= 0.0F) return from;
        if (progress >= 1.0F) return to;
        // The status card is larger than the floating button. Reversing the
        // endpoints plays the same path backwards: shrink, then move home.
        if (from.width * from.height > to.width * to.height) {
            return Expand(to, from, 1.0F - progress);
        }
        return Expand(from, to, progress);
    }

    bool operator==(const HomeControlMoveExpand&) const = default;
};

[[huxerui::composable]] huxerui::View DesktopModeSwitchRow(
    huxerui::ImageVariant icon, huxerui::StringVariant label,
    huxerui::StringVariant description, huxerui::View control) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Column {
        huxerui::Row {
            huxerui::Image(icon)
                .Fit(huxerui::ImageFit::Contain)
                .Tint(theme.colors.primary)
                .With(huxerui::Frame{.width = 22.0F, .height = 22.0F}),
            huxerui::Text(label).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody),
                theme.colors.on_surface}),
        }.With(huxerui::Spacing(10.0F),
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

[[huxerui::composable]] huxerui::View DesktopHomeStatusCard() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
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
    const bool changing = action == CoreAction::Start ||
                          action == CoreAction::Stop;
    const huxerui::StringVariant status = Localized(
        action == CoreAction::Start ? "正在启动"
        : action == CoreAction::Stop ? "正在停止"
                                     : "运行中");
    const auto runningSince = control.model->view.Get().core.runningSince;
    const auto elapsed = runningSince == std::chrono::steady_clock::time_point{}
        ? std::chrono::seconds{0}
        : std::max(std::chrono::seconds{0},
                   std::chrono::duration_cast<std::chrono::seconds>(
                       clock.Get() - runningSince));
    const auto seconds = elapsed.count();
    const std::string uptime = std::format("{:02}:{:02}:{:02}",
        seconds / 3600, (seconds / 60) % 60, seconds % 60);
    huxerui::View icon = changing
        ? huxerui::View{huxerui::ProgressCircle().With(
              huxerui::Frame{.width = 24.0F, .height = 24.0F})}
        : huxerui::View{huxerui::Image(app::images::pause)
              .Fit(huxerui::ImageFit::Contain)
              .Tint(theme.colors.on_primary_container)
              .With(huxerui::Frame{.width = 22.0F, .height = 22.0F})};
    huxerui::View content = huxerui::Row {
        huxerui::Stack { std::move(icon) }
            .With(huxerui::Frame{.width = 36.0F, .height = 36.0F},
                  huxerui::Align(huxerui::HorizontalAlignment::Center,
                                 huxerui::VerticalAlignment::Center),
                  huxerui::Background(theme.colors.primary_container),
                  huxerui::CornerRadius(18.0F),
                  huxerui::Scale(huxerui::AnimateTo(
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
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
           huxerui::MainAlign(huxerui::MainAxisAlignment::Center));
    return huxerui::View{Card(std::move(content))}
        .OnClick([control] {
            control.Run({.action = CoreAction::Stop});
        })
        .With(huxerui::Enabled(!control.Busy()),
              huxerui::Tooltip(Localized("停止内核")),
              huxerui::Indication{},
              huxerui::SharedBounds("home-core-control")
                  .BoundsTransform(HomeControlMoveExpand{}),
              huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                 .label = huxerui::UseString(
                                     Localized("停止内核"))});
}

// 桌面：内核启动/停止悬浮按钮（右下角，与移动端的 VPN 悬浮按钮同形制）。
// 「正式启动」是独立动作：只负责拉起/停止内核，启动时按已记录的 TUN 与
// 系统代理意图恢复；不再由各个开关隐式触发内核。
[[huxerui::composable]] huxerui::View DesktopHomeFloatingAction(
    huxerui::View page, bool compact, huxerui::State<bool> cardShown,
    huxerui::SharedTransitionHandle transition) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const auto control = UseCoreControl();
    huxerui::View base = page;
    const bool active = control.Running();
    const bool enabled = !control.Busy();
    const CoreAction command = control.model->operations.command.Get().action;
    const bool changing = command == CoreAction::Start ||
                          command == CoreAction::Stop;
    const auto toggle = [control, active] {
        if (control.Busy()) return;
        // Keep the button visible while the command is pending. The HomePage
        // lifecycle starts the shared transition once the new state is read
        // back, so both directions animate against stable content.
        control.Run({.action = active ? CoreAction::Stop : CoreAction::Start});
    };

    huxerui::View button =
        huxerui::IconButton(active ? app::images::pause : app::images::play,
                            Localized(active ? "停止内核" : "启动内核"))
            .OnClick(toggle)
            .With(huxerui::Tooltip(Localized(active ? "停止内核" : "启动内核")),
                  huxerui::Frame{.width = 56.0F, .height = 56.0F},
                  huxerui::Enabled(enabled),
                  huxerui::Opacity(changing ? 0.0F : 1.0F),
                  huxerui::Foreground(active
                                          ? theme.colors.on_primary_container
                                          : theme.colors.on_primary),
                  huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                     .label = huxerui::UseString(Localized(
                                         active ? "停止内核" : "启动内核"))});
    button = WithoutIconButtonOutlines(button);
    huxerui::View floating = huxerui::Stack {
        std::move(button),
        changing ? huxerui::View{huxerui::ProgressCircle().With(
                       huxerui::Frame{.width = 28.0F, .height = 28.0F})}
                 : huxerui::View{huxerui::Row{}},
    }.With(huxerui::Frame{.width = 56.0F, .height = 56.0F},
           huxerui::Align(huxerui::HorizontalAlignment::Center,
                          huxerui::VerticalAlignment::Center),
           huxerui::Background(active || changing
                                   ? theme.colors.primary_container
                                   : theme.colors.primary),
           huxerui::CornerRadius(kFloatingActionCornerRadius),
           huxerui::Shadow{ThemeShadowColor(theme, 0.28F), {}, 14.0F, 2.0F},
           huxerui::Scale(huxerui::AnimateTo(
               changing ? 0.94F : 1.0F,
               huxerui::TweenSpec{.duration = 0.18,
                                  .easing = huxerui::Easing::EaseOut})),
           huxerui::SharedBounds("home-core-control")
               .BoundsTransform(HomeControlMoveExpand{}));
    huxerui::View dock = huxerui::Column {
        std::move(floating),
    }.With(huxerui::Padding(huxerui::EdgeInsets{
                 .right = theme.spacing.medium,
                 .bottom = compact ? kCompactFloatingNavigationInset : theme.spacing.medium,
                 .left = theme.spacing.medium,
             }),
             huxerui::MainAlign(huxerui::MainAxisAlignment::End),
             huxerui::CrossAlign(huxerui::CrossAxisAlignment::End));
    return huxerui::Stack {
        std::move(base),
        compact || !cardShown.Get() ? std::move(dock)
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
    const auto settingsModel = huxerui::UseService<SettingsModel>();
    auto state = huxerui::UseState<HomeState>({});
    auto controlCardShown = huxerui::UseState<bool>(
        coreModel->view.Get().core.state == core::CoreState::Running);
    auto controlCardSynced = huxerui::UseState<bool>(false);
    const auto controlTransition = huxerui::UseSharedTransition();
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
            [coreModel, controlCardShown, controlCardSynced,
             controlTransition, active, compact] {
                const CoreView& view = coreModel->view.Get();
                if (view.ready) {
                    const bool running =
                        view.core.state == core::CoreState::Running;
                    if (!controlCardSynced.Get() || !active || compact) {
                        controlCardSynced = true;
                        controlCardShown = running;
                    } else if (!coreModel->operations.Busy() &&
                               controlCardShown.Get() != running) {
                        try {
                            controlTransition.Run(kHomeControlMotion,
                                [controlCardShown, running] {
                                    controlCardShown = running;
                                });
                        } catch (const std::logic_error&) {
                            // 页面切换期间作用域可能暂时卸载，直接同步真实状态。
                            controlCardShown = running;
                        }
                    }
                }
                return [] {};
            },
            coreModel->view, coreModel->operations.command,
            active, compact);
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
        huxerui::View cardView = Card(std::move(body));
        cards.push_back(std::move(cardView)
                            .LayoutValue<HomeCardSpan>(size)
                            .Key("home-card-" + id));
    }
    if (kDesktopHomeControl && !compact && controlCardShown.Get()) {
        cards.push_back(huxerui::View{CLASHFLUX_HOME_CONTROL_CARD()}
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

    huxerui::View pageBody = huxerui::Column {
        std::move(grid),
        // 悬浮启动按钮压在滚动内容之上，尾部留出等高的空白，避免最后一张
        // 卡片被按钮遮住（紧凑视口本来就带底部悬浮导航的空白）。
        compact ? CompactFloatingNavigationFooter()
                : huxerui::View{huxerui::Row{}.With(
                      huxerui::Frame{.height =
                          kDesktopHomeControl && controlCardShown.Get()
                              ? 0.0F : kHomeFloatingButtonInset})},
    }.With(huxerui::Spacing(kHomeGridGap),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    // 首页标题栏不重复展示内核状态；状态统一显示在左侧品牌 Logo。
    huxerui::View headerActions = huxerui::Row {
        // 运行态泵是空 View（无布局），挂在这里即可让它内部的 Lifecycle 注册。
        std::move(runtimePump),
    };

    // 注意：首页不要在滚动内容里声明 Focusable(true)——运行时会自动把初始
    // 焦点所在的节点滚入视野，导致首页一打开就被滚到中途（且随后每次重组都可能
    // 来回滚）。可聚焦性交给内置控件自己的默认策略，分段选择器只保留语义。
    huxerui::View scrollContent =
        huxerui::ScrollView(std::move(pageBody)).With(huxerui::Grow(1.0F));
    huxerui::View page = PageScaffold(
        Localized("首页"), std::move(headerActions), std::move(scrollContent), true, false, true, kHomeGridGap);

    // 移动端启动/停止按钮脱离滚动内容，固定在底部悬浮导航之上的位置。
    huxerui::View shell = CLASHFLUX_HOME_FLOATING_ACTION(
        std::move(page), state, compact, controlCardShown,
        controlTransition);

    return std::move(shell).With(huxerui::Grow(1.0F));
}

#undef CLASHFLUX_HOME_PLATFORM_CARD
#undef CLASHFLUX_HOME_CONTROL_CARD
#undef CLASHFLUX_HOME_FLOATING_ACTION

} // namespace clashflux::ui
