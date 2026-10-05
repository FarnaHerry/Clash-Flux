// home_page.cpp — 固定网格首页：卡片目录 + 网格摆放（宽/高各 1..4 单位）。
//
// 结构：
//   kHomeCards   文件作用域 #if 选出的平台卡片表（composable 体内禁止条件编译，
//                平台差异只出现在这里和平台函数选择宏上）；顺序与尺寸即最终布局；
//   HomeGrid     把「页面逻辑宽度」分成 1..4 列，按卡片声明的宽高（HomeCardSpan）
//                做左上紧凑的二维打包；
//   HomeCardContent 按种类组装卡片内容，平台专属卡片经宏选择完整函数。
// 布局是**编译期常量**（见 AGENTS.md / CLAUDE.md 第 13 条）：没有编辑态、拖动排序、
// 增删卡片，也没有 home.layout.* 持久化，因此首帧就是最终布局。
//
// 数据流：UI 泵每 500ms 从 CoreStreams 取最新流量帧追加进 60 点环形历史
// （State<vector<TrafficPoint>>），连接快照帧只取总量字段；内核状态与启用
// 订阅每拍重读。Canvas 画家捕获历史快照，重组后按最新序列重绘。
#include <huxerui/huxerui.h>
#include <huxerui/charts.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app_resources.h"
#include "proxies_model.h"
#include "ui.h"
#include "empty_state.h"
#include "task_bridge.h"

import clashflux.core;
import clashflux.db;
import clashflux.stream;
import clashflux.store.core;
import clashflux.store.profiles;
import clashflux.utils;

// CoreView 含 store::CoreSnapshot、ProfilesModel 含 db::Profile，必须在模块导入
// 之后（同 profiles_cache.h）。
#include "core_model.h"
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

enum class HomeCardKind {
    Traffic,    // 流量曲线（标题行带实时上/下行速率）
    Total,      // 流量统计（总上传 / 总下载）
    Mode,       // 出站模式
    Profile,    // 当前订阅
    Proxy,      // 系统代理（桌面）
    Tun,        // TUN 模式（桌面）
    Vpn,        // 隧道状态（移动端）
};

struct HomeCardSpec {
    HomeCardKind kind;
    std::string_view id;     // 持久化身份，不要改名（改名会让旧布局回落默认）
    std::string_view title;  // 编辑态标题
    int width = 1;           // 默认占用列数 1..4
    int height = 1;          // 默认占用行数 1..4
};

#if defined(__ANDROID__)
// 移动端可选卡片：桌面独有的系统代理/TUN 换成隧道状态。
// 手机屏是 2 列：需要整宽的（图表、横排出站模式、开关行）给 2 格，信息卡 1 格；
// 高度仍按 FlClash 跨度（图表/信息卡 2 行，开关行 1 行）。
constexpr HomeCardSpec kHomeCards[] = {
    // Lib-Charts 的紧凑绘图面最小 240×96，适配 2 行（188pt）的卡片。
    {HomeCardKind::Traffic, "traffic", "流量曲线", 2, 2},
    {HomeCardKind::Total, "total", "流量统计", 1, 2},
    {HomeCardKind::Mode, "mode", "出站模式", 2, 1},
    {HomeCardKind::Profile, "profile", "当前订阅", 1, 2},
    {HomeCardKind::Vpn, "vpn", "隧道状态", 1, 2},
};
constexpr std::string_view kDefaultCoreName = "sing-box libbox";
#define CLASHFLUX_HOME_PLATFORM_CARD(homeState, state, kind) \
    AndroidHomePlatformCard(homeState, kind)
#define CLASHFLUX_HOME_FLOATING_ACTION(page, state, compact) \
    AndroidHomeFloatingAction(std::move(page), state, compact)
#else
// 桌面可选卡片：系统代理/TUN 开关是桌面专有能力。
//
// 初始宽高按 FlClash 仪表盘实测跨度（其 3 列网格：单元 264x79.5、间距 14 逻辑 px）：
//   网络速度 2x2 → 流量曲线 2x2   流量统计 1x2 → 流量统计 1x2
//   系统代理 1x1 → 系统代理 1x1   TUN 1x1      → TUN 模式 1x1
//   出站模式(竖排) 1x2 / V2(横排) → 出站模式取 2x1（横排三段按钮吃宽度）
//   其余信息类卡片（当前订阅）与 FlClash 的信息卡同级，取 1x2。
constexpr HomeCardSpec kHomeCards[] = {
    // 与移动端共用紧凑绘图面，流量卡片高度降为 2 行。
    {HomeCardKind::Traffic, "traffic", "流量曲线", 2, 2},
    {HomeCardKind::Total, "total", "流量统计", 1, 2},
    {HomeCardKind::Mode, "mode", "出站模式", 2, 1},
    {HomeCardKind::Profile, "profile", "当前订阅", 1, 2},
    {HomeCardKind::Proxy, "proxy", "系统代理", 1, 1},
    {HomeCardKind::Tun, "tun", "TUN 模式", 1, 1},
};
constexpr std::string_view kDefaultCoreName = "sing-box";
#define CLASHFLUX_HOME_PLATFORM_CARD(homeState, state, kind) \
    DesktopHomePlatformCard(kind)
#define CLASHFLUX_HOME_FLOATING_ACTION(page, state, compact) \
    DesktopHomeFloatingAction(std::move(page), compact)
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

// 页面逻辑宽度分档只由可用宽度决定，使手机与桌面 Compact 窗口在同宽时
// 使用相同卡片网格；更宽视口逐步增加列数。
constexpr int HomeGridColumns(float width) {
    if (width < 840.0F) return 2;
    if (width < 1200.0F) return 3;
    return 4;
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
    const int columns = HomeGridColumns(available);
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
        const HomeCardSize span =
            child.LayoutValueOr<HomeCardSpan>(HomeCardSize{});
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
    std::vector<ProxyGroupSnapshot> proxyGroups;

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

std::string currentProxyLine(const std::vector<ProxyGroupSnapshot>& groups) {
    for (const ProxyGroupSnapshot& group : groups) {
        if (!group.current.empty()) return group.displayName + " · " +
            (group.nodeLabels.contains(group.current) ? group.nodeLabels.at(group.current) : group.current);
    }
    return {};
}

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
                     huxerui::State<std::size_t> homeMode,
                     huxerui::State<bool> modePending,
                     const std::shared_ptr<CoreModel>& coreModel,
                     const std::shared_ptr<ProfilesModel>& profilesModel,
                     const std::shared_ptr<ProxiesModel>& proxiesModel) {
    auto& core = store::coreStore();
    HomeState s = state.Get();
    // 内核状态来自唯一来源 CoreModel。
    s.core = coreModel->view.Get().core;
    updateRuntime(s, core);
    if (!modePending.Get()) homeMode = HomeModeIndex(s.core.mode);

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
    // 策略组取自全局唯一的 ProxiesModel（由 AppRoot 驱动刷新）。
    s.proxyGroups = proxiesModel->snapshot.Get().groups;
    state = s;
}

// 首页运行态泵：平台差异由宏在文件作用域选择整份实现（composable 体内不做
// 条件编译）。
#if defined(__ANDROID__)
// Android：libbox 速率只在快照里、且模型空闲时不通知，需要固定节拍采样。
[[huxerui::composable]] huxerui::View HomeRuntimePump(
    huxerui::State<HomeState> state, huxerui::State<std::size_t> homeMode,
    huxerui::State<bool> modePending, std::shared_ptr<CoreModel> coreModel,
    std::shared_ptr<ProfilesModel> profilesModel,
    std::shared_ptr<ProxiesModel> proxiesModel) {
    auto tasks = huxerui::UseTaskScope();
    huxerui::Lifecycle(
        [tasks, state, homeMode, modePending, coreModel, profilesModel,
         proxiesModel] {
            tasks.Launch([state, homeMode, modePending, coreModel, profilesModel,
                          proxiesModel]() -> huxerui::Task<void> {
                co_await PollWhile(std::chrono::duration<double>{0.5}, [=] {
                    MirrorHomeState(state, homeMode, modePending, coreModel,
                                    profilesModel, proxiesModel);
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
    huxerui::State<HomeState> state, huxerui::State<std::size_t> homeMode,
    huxerui::State<bool> modePending, std::shared_ptr<CoreModel> coreModel,
    std::shared_ptr<ProfilesModel> profilesModel,
    std::shared_ptr<ProxiesModel> proxiesModel) {
    huxerui::Lifecycle(
        [state, homeMode, modePending, coreModel, profilesModel, proxiesModel] {
            MirrorHomeState(state, homeMode, modePending, coreModel,
                            profilesModel, proxiesModel);
            return [] {};
        },
        coreModel->view, profilesModel->list, proxiesModel->snapshot);
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

// 等宽分段选择器的滑动指示块：选中项下方自绘圆角色块，切换时按补间滑动。
// 修饰符契约 = 嵌套 Extension 类型（见 huxerui NodeExtension 文档注释）。
struct HomeSlidingSegments {
    class Extension;

    std::size_t selected_index = 0;
    huxerui::Color indicator = huxerui::Color::Transparent();
    float corner_radius = 8.0F;
    double duration = 0.18;

    bool operator==(const HomeSlidingSegments&) const = default;
};

class HomeSlidingSegments::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode& node, const HomeSlidingSegments& spec) {
        Update(node, spec);
    }

    void Update(huxerui::ViewNode& node, const HomeSlidingSegments& spec) {
        static_cast<void>(node);
        const bool selection_changed =
            initialized_ && selected_index_ != spec.selected_index;
        selected_index_ = spec.selected_index;
        indicator_ = spec.indicator;
        corner_radius_ = spec.corner_radius;
        duration_ = spec.duration;
        geometry_pending_ = geometry_pending_ || !initialized_ || selection_changed;
        initialized_ = true;
    }

    FrameResult OnFrame(huxerui::ViewNode& node, const huxerui::FrameInfo& frame) override {
        static_cast<void>(node);
        const huxerui::MotionAdvanceResult result = offset_.Advance(frame);
        if (result.changed) InvalidatePaint(PaintInvalidation::Content);
        return FrameResult{.needs_frame = geometry_pending_ || result.needs_frame,
                           .wake_after = result.wake_after};
    }

    PaintInvalidation PrepareGeometry(huxerui::ViewNode& node,
                                      huxerui::TextMeasurer&) override {
        if (selected_index_ >= node.ChildCount()) {
            return PaintInvalidation::None;
        }
        const huxerui::ViewNode& selected = node.ChildAt(selected_index_);
        const float target = selected.LayoutOffset().x;
        const float width = selected.LayoutSize().width;
        geometry_pending_ = false;
        if (!geometry_initialized_) {
            geometry_initialized_ = true;
            width_ = width;
            offset_.Set(target);
            return PaintInvalidation::Content;
        }
        bool changed = false;
        if (width_ != width) {
            width_ = width;
            changed = true;
        }
        if (offset_.Target() != target) {
            if (duration_ > 0.0) {
                offset_.AnimateTo(
                    target, huxerui::TweenSpec{
                                duration_, huxerui::Easing::EaseOut});
            } else {
                offset_.Set(target);
            }
            changed = true;
        }
        return changed ? PaintInvalidation::Content : PaintInvalidation::None;
    }

    // 画在内容之下：node.Bounds() 是节点本地坐标（原点 0,0），子项偏移不含
    // 本节点的 padding（这里本就没有 padding）。
    void PaintBehindContent(const huxerui::ViewNode& node,
                            huxerui::PaintContext& context) const override {
        if (!geometry_initialized_ || width_ <= 0.0F ||
            indicator_.alpha <= 0.0F) {
            return;
        }
        const huxerui::Rect frame = node.Bounds();
        context.DrawRect(
            huxerui::Rect{offset_.Value(), 0.0F, width_, frame.height},
            indicator_, corner_radius_);
    }

private:
    std::size_t selected_index_ = 0;
    huxerui::Color indicator_ = huxerui::Color::Transparent();
    float corner_radius_ = 8.0F;
    double duration_ = 0.18;
    huxerui::MotionController offset_;
    float width_ = 0.0F;
    bool initialized_ = false;
    bool geometry_initialized_ = false;
    bool geometry_pending_ = false;
};

// ---- 卡片内容 --------------------------------------------------------------

// 流量统计：饼图展示上传/下载累计占比，右侧保留各自总量与百分比。
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
    const auto totalMetric = [&theme](huxerui::StringVariant label, huxerui::Color color,
                                      const std::string& value,
                                      const std::string& share) {
        return huxerui::Column {
            huxerui::Row {
                huxerui::Text(label).Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption)
                        .WithWeight(huxerui::FontWeight::Medium),
                    color}),
                huxerui::Spacer(),
                huxerui::Text(share).Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption)
                        .WithWeight(huxerui::FontWeight::SemiBold),
                    color}),
            }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
            huxerui::Text(value).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody)
                    .WithWeight(huxerui::FontWeight::SemiBold),
                theme.colors.on_surface}),
        }.With(huxerui::Spacing(2.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    };
    const huxerui::PieChartData data({
        {"upload", huxerui::UseString(Localized("上传")),
         static_cast<double>(upload)},
        {"download", huxerui::UseString(Localized("下载")),
         static_cast<double>(download)},
    });

    return huxerui::Column {
        HomeCardHeading(Localized("流量统计")),
        huxerui::Row {
            huxerui::PieChart(data)
                .SliceStyle("upload", {.fill = huxerui::Brush(upColor)})
                .SliceStyle("download", {.fill = huxerui::Brush(downColor)})
                .With(huxerui::Frame{.width = 72.0F, .height = 72.0F}),
            huxerui::Column {
                totalMetric(Localized("↑ 上传"), upColor, formatBytes(upload), shareText(upload)),
                totalMetric(Localized("↓ 下载"), downColor, formatBytes(download), shareText(download)),
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
// HuxerUI Lib-Charts（面积图），替掉原先手绘的 Canvas：多了坐标轴刻度（速率格式化）
// 与 hover tooltip，外观仍保持「下载实心面积 + 上传细线」。
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
            // 小卡片里 X 轴（窗口内索引）没有信息量，只留 Y 轴刻度；刻度文本按速率
            // 格式化，比原先「按窗口峰值归一化、无任何数字」可读。
            .XAxis(huxerui::ChartAxis::Linear().Labels(false).Grid(false))
            .YAxis(huxerui::ChartAxis::Linear()
                       .IncludeZero()
                       .TickCount(3)
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

// 出站模式：三枚等宽按钮（无标题文字）。选中指示块由 HomeSlidingSegments
// 在内容之下自绘，切换时做滑动补间。
[[huxerui::composable]] huxerui::View HomeModeCard(
    huxerui::State<std::size_t> mode,
    huxerui::State<bool> modePending,
    huxerui::TaskScope tasks, huxerui::ToastHandle toast) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const std::size_t selected = mode.Get();

    huxerui::Color indicator = theme.colors.primary;
    indicator.alpha = 0.22F;
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

    std::vector<huxerui::View> segments;
    segments.reserve(kModeLabels.size());
    for (std::size_t i = 0; i < kModeLabels.size(); ++i) {
        const bool active = i == selected;
        segments.push_back(
            huxerui::Row {
                huxerui::Text(Localized(kModeLabels[i])).Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kBody)
                        .WithWeight(active ? huxerui::FontWeight::SemiBold
                                           : huxerui::FontWeight::Regular),
                    active ? theme.colors.primary
                           : theme.colors.on_surface_variant}),
            }
                .With(huxerui::Grow(1.0F),
                      huxerui::Padding(
                          huxerui::EdgeInsets::Symmetric(0.0F, 9.0F)),
                      huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                      huxerui::CrossAlign(
                          huxerui::CrossAxisAlignment::Center),
                      huxerui::CornerRadius(8.0F),
                      indication,
                      huxerui::Semantics{
                          .role = huxerui::SemanticRole::Tab,
                          .label = Localized(kModeLabels[i]),
                          .selected = active})
                .OnClick([tasks, toast, mode, modePending, i] {
                    if (modePending.Get()) return;
                    const std::size_t previous = mode.Get();
                    mode = i;
                    modePending = true;
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        bool ok = false;
                        std::string error;
                        try {
                            ok = co_await RunOnTaskThread(
                                [i] { return store::coreStore().applyMode(kModes[i]); });
                        } catch (const std::exception& exception) {
                            error = exception.what();
                        }
                        modePending = false;
                        if (!ok) {
                            mode = previous;
                            toast.Show(error.empty() ? Localized("出站模式切换失败")
                                                     : huxerui::StringVariant(error));
                        }
                    });
                })
                .Key("home-mode-" + std::to_string(i)));
    }

    return huxerui::Row(std::move(segments))
        .With(huxerui::Spacing(4.0F),
              huxerui::Background(theme.colors.surface_container_high),
              huxerui::CornerRadius(12.0F),
              huxerui::ClipChildren(),
              HomeSlidingSegments{selected, indicator, 8.0F, 0.22});
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

// 内核状态文字：标题行状态图标的提示文本（内核状态不再单独占一张卡片）。
huxerui::StringVariant HomeKernelStatusText(const HomeState& s) {
    switch (s.core.state) {
    case core::CoreState::Running:
        return LocalizedFormat("内核运行中 · {}", s.core.version.empty()
                                                       ? std::string{kDefaultCoreName}
                                                       : s.core.version);
    case core::CoreState::Stopped: return Localized("内核已停止");
    case core::CoreState::Starting: return Localized("内核启动中");
    case core::CoreState::Failed: return Localized("内核启动失败");
    }
    return Localized("内核状态未知");
}

// 标题行内核状态图标（放在编辑按钮之前）：运行中主色、启动中琥珀、失败错误色、
// 未运行次级色；详细状态走 Tooltip。
[[huxerui::composable]] huxerui::View HomeKernelStatusIcon(
    huxerui::State<HomeState> state) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const HomeState& s = state.Get();
    huxerui::Color color = theme.colors.on_surface_variant;
    if (s.core.state == core::CoreState::Running) {
        color = theme.colors.primary;
    } else if (s.core.state == core::CoreState::Starting) {
        color = SemanticWarningColor(theme);
    } else if (s.core.state == core::CoreState::Failed) {
        color = theme.colors.error;
    }
    return huxerui::Image(app::images::bolt)
        .Fit(huxerui::ImageFit::Contain)
        .Tint(color)
        .With(huxerui::Frame{.width = 20.0F, .height = 20.0F},
              huxerui::Tooltip(HomeKernelStatusText(s)),
              huxerui::Semantics{.role = huxerui::SemanticRole::Image,
                                 .label = huxerui::UseString(
                                     HomeKernelStatusText(s))});
}

// ---- 平台专属卡片（整个函数由编译宏在调用点选择）---------------------------

#if defined(__ANDROID__)

// Android 隧道状态：只读展示 VpnService 状态与当前线路（启动/停止仍由首页
// 右下角的固定悬浮按钮负责，卡片不重复一套控制）。
[[huxerui::composable]] huxerui::View AndroidHomeVpnCard(const HomeState& state) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto vpn_state = huxerui::UseState(AndroidVpnState());

    huxerui::Lifecycle(
        [tasks, vpn_state] {
            tasks.Launch([vpn_state]() -> huxerui::Task<void> {
                co_await PollWhile(std::chrono::duration<double>{1.0},
                                   [vpn_state] {
                                       vpn_state = AndroidVpnState();
                                       return true;
                                   });
            });
            return [] {};
        },
        0);

    const int vpnStatus = vpn_state.Get();
    const huxerui::StringVariant status = Localized(
        vpnStatus == 2   ? "已连接"
        : vpnStatus == 1 ? "正在连接"
        : vpnStatus == 3 ? "启动失败"
                         : "未连接");
    const huxerui::Color statusColor =
        vpnStatus == 2   ? theme.colors.primary
        : vpnStatus == 1 ? SemanticWarningColor(theme)
        : vpnStatus == 3 ? theme.colors.error
                         : theme.colors.on_surface_variant;
    const std::string proxyLine = currentProxyLine(state.proxyGroups);

    return huxerui::Column {
        HomeCardHeading(Localized("隧道状态")),
        huxerui::Row {
            huxerui::Text(Localized("状态")).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody),
                theme.colors.on_surface_variant}),
            huxerui::Spacer(),
            huxerui::Text(status).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody)
                    .WithWeight(huxerui::FontWeight::SemiBold),
                statusColor}),
        }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Row {
            huxerui::Text(Localized("当前线路")).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody),
                theme.colors.on_surface_variant}),
            huxerui::Spacer(),
            huxerui::Text(proxyLine.empty() ? Localized("暂无当前线路")
                                            : huxerui::StringVariant(proxyLine))
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kChip),
                    theme.colors.on_surface}),
        }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Spacer(),
    }.With(huxerui::Spacing(8.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View AndroidHomePlatformCard(
    const HomeState& state, HomeCardKind kind) {
    if (kind == HomeCardKind::Vpn) return AndroidHomeVpnCard(state);
    return huxerui::Row{};
}

[[huxerui::composable]] huxerui::View AndroidHomeFloatingAction(
    huxerui::View page, huxerui::State<HomeState> homeState, bool compact) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const HomeState& state = homeState.Get();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto busy = huxerui::UseState(false);
    auto activeState = huxerui::UseState(AndroidVpnState() == 1 ||
                                         AndroidVpnState() == 2);
    auto pending = huxerui::UseState(false);
    // composable 形参被 codegen 固定为 const：拷贝到局部再走右值链。
    huxerui::View base = page;
    if (!compact) return base;

    const bool active = activeState.Get();
    const bool canStart = state.profileId != 0;
    const bool enabled = !busy.Get() && (active || canStart);
    huxerui::Lifecycle(
        [tasks, activeState, pending] {
            tasks.Launch([activeState, pending]() -> huxerui::Task<void> {
                co_await PollWhile(std::chrono::duration<double>{0.5},
                                   [activeState, pending] {
                    if (!pending.Get()) {
                        const int current = AndroidVpnState();
                        activeState = current == 1 || current == 2;
                    }
                    return true;
                });
            });
            return [] {};
        },
        0);
    const auto toggle = [tasks, toast, busy, activeState, pending] {
        if (busy.Get() || pending.Get()) return;
        const bool previous = activeState.Get();
        activeState = !previous;
        pending = true;
        busy = true;
        tasks.Launch([toast, busy, activeState, pending,
                      previous]() -> huxerui::Task<void> {
            bool succeeded = false;
            std::string error;
            try {
                co_await RunOnTaskThread([previous] {
                    auto& core = store::coreStore();
                    core.setSetting("core.tun_enabled",
                                    previous ? "false" : "true");
                    if (previous) {
                        AndroidStopVpn();
                        WaitForAndroidVpnStopped();
                        core.startCore(store::profilesStore().selectedYaml(),
                                       false, false);
                    } else {
                        core.startCore(store::profilesStore().selectedYaml(),
                                       false, true);
                        AndroidStartVpn();
                    }
                });
                if (previous) {
                    const auto [state, tunEnabled] = co_await RunOnTaskThread([] {
                        return std::pair{AndroidVpnState(),
                                         store::coreStore().setting(
                                             "core.tun_enabled", "false") == "true"};
                    });
                    succeeded = state != 1 && state != 2 && !tunEnabled;
                } else {
                    for (int attempt = 0; attempt < 300; ++attempt) {
                        const auto [state, tunEnabled] = co_await RunOnTaskThread([] {
                            return std::pair{AndroidVpnState(),
                                             store::coreStore().setting(
                                                 "core.tun_enabled", "false") == "true"};
                        });
                        if (state == 1 || state == 2) {
                            succeeded = true;
                            break;
                        }
                        if (state == 3 || !tunEnabled) break;
                        co_await huxerui::Delay(std::chrono::duration<double>{0.2});
                    }
                }
            } catch (const std::exception& exception) {
                error = exception.what();
            }
            if (!succeeded) {
                try {
                    co_await RunOnTaskThread([previous] {
                        store::coreStore().setSetting(
                            "core.tun_enabled", previous ? "true" : "false");
                    });
                } catch (const std::exception& exception) {
                    if (error.empty()) error = exception.what();
                }
                if (error.empty()) error = store::coreStore().snapshot().lastError;
            }
            pending = false;
            busy = false;
            if (!succeeded) {
                activeState = previous;
                toast.Show(error.empty()
                               ? Localized(previous ? "VPN 隧道未能关闭"
                                                    : "VPN 启动已取消或超时")
                               : huxerui::StringVariant(error));
            } else if (previous) {
                toast.Show(Localized("VPN 隧道已关闭"));
            } else {
                toast.Show(Localized("正在启动 VPN 隧道"));
            }
        });
    };

    // 三角（play）启动、双竖线（pause）暂停；与代理页悬浮测速按钮共用
    // 固定悬浮层定位（主轴末端 + 交叉轴末端，避开底部悬浮导航）。
    huxerui::View floating =
        huxerui::IconButton(active ? app::images::pause : app::images::play,
                            Localized(active ? "停止 VPN" : "启动 VPN"))
            .OnClick(toggle)
            .With(huxerui::Tooltip(Localized(active ? "停止 VPN" : "启动 VPN")),
                  huxerui::Frame{.width = 56.0F, .height = 56.0F},
                  huxerui::Enabled(enabled),
                  huxerui::Background(active ? theme.colors.primary_container
                                             : theme.colors.primary),
                  huxerui::Foreground(active
                                          ? theme.colors.on_primary_container
                                          : theme.colors.on_primary),
                  huxerui::CornerRadius(28.0F),
                  huxerui::Shadow{ThemeShadowColor(theme, 0.28F), {}, 14.0F,
                                  2.0F},
                  huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                     .label = huxerui::UseString(Localized(
                                         active ? "停止 VPN" : "启动 VPN"))});
    floating = WithoutIconButtonOutlines(floating);
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

[[huxerui::composable]] huxerui::View DesktopModeSwitchRow(
    huxerui::ImageVariant icon, huxerui::StringVariant label,
    huxerui::StringVariant hint, huxerui::View control) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Row {
        huxerui::Image(std::move(icon))
            .Fit(huxerui::ImageFit::Contain)
            .Tint(theme.colors.primary)
            .With(huxerui::Frame{.width = 22.0F, .height = 22.0F}),
        huxerui::Column {
            huxerui::Text(label).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody),
                theme.colors.on_surface}),
            huxerui::Text(hint).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption),
                theme.colors.on_surface_variant}),
        }.With(huxerui::Spacing(2.0F), huxerui::Grow(1.0F)),
        std::move(control),
    }.With(huxerui::Spacing(12.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

// 桌面：系统代理开关卡片（自洽管理自己的任务、乐观状态和失败提示）。
[[huxerui::composable]] huxerui::View DesktopHomeProxyCard() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    // 内核/接管状态的全局唯一来源（见 core_model.h）：本卡片不再自己轮询
    // coreStore()，组合期读模型即可，模型变化时本卡片自动重组。
    const auto coreModel = huxerui::UseService<CoreModel>();
    const CoreView coreView = coreModel->view.Get();
    // 动作期间显示乐观值，动作结束后回到模型的权威值。
    const bool modelProxy = coreView.systemProxyIntent;
    auto proxyEnabled = huxerui::UseState(modelProxy);
    auto pending = huxerui::UseState(false);
    const bool shownProxy = pending.Get() ? proxyEnabled.Get() : modelProxy;

    return huxerui::Column {
        DesktopModeSwitchRow(
            app::images::system_proxy, Localized("系统代理"),
            Localized("为桌面应用设置系统代理"),
            huxerui::Switch(shownProxy)
                .OnChanged([tasks, toast, proxyEnabled, pending, modelProxy,
                            coreModel](bool on) {
                    if (pending.Get()) return;
                    const bool previous = modelProxy;
                    proxyEnabled = on;
                    pending = true;
                    tasks.Launch([toast, proxyEnabled, pending, previous, on,
                                  coreModel]() -> huxerui::Task<void> {
                        DesktopModeApplyResult result;
                        try {
                            result = co_await RunOnTaskThread([on] {
                                return ApplyDesktopSystemProxy(on);
                            });
                        } catch (const std::exception& exception) {
                            result.error = exception.what();
                        }
                        // 成功：权威值写透模型（本卡片与设置页/托盘读同一份），
                        // 失败：目标值校验后回落，不覆盖用户更新的意图。
                        pending = false;
                        if (result.status != DesktopModeApplyStatus::Applied) {
                            if (proxyEnabled.Get() == on) proxyEnabled = previous;
                            toast.Show(result.error.empty()
                                           ? Localized("系统代理设置失败")
                                           : huxerui::StringVariant(result.error));
                        } else {
                            coreModel->Update([on](CoreView& view) {
                                view.systemProxyIntent = on;
                                view.systemProxyActive = on;
                            });
                        }
                    });
                })),
        huxerui::Spacer(),
    }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// 桌面：TUN 模式开关卡片（需要管理员权限；权限不足时引导装服务模式）。
[[huxerui::composable]] huxerui::View DesktopHomeTunCard() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const huxerui::ApplicationHandle application = huxerui::UseApplication();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto dialog = huxerui::UseDialog();
    auto clipboard = application.Clipboard();
    auto tunEnabled = huxerui::UseState(false);
    auto pending = huxerui::UseState(false);
    const huxerui::Color text_color = theme.colors.on_surface;
    const huxerui::Color hint_color = theme.colors.on_surface_variant;

    // 内核/接管状态的全局唯一来源（见 core_model.h）：TUN 意图来自模型。
    const auto coreModel = huxerui::UseService<CoreModel>();
    const CoreView coreView = coreModel->view.Get();
    const bool modelTun = coreView.core.tunEnabled;
    const bool shownTun = pending.Get() ? tunEnabled.Get() : modelTun;

    return huxerui::Column {
        DesktopModeSwitchRow(
            app::images::tun, Localized("TUN 模式"),
            Localized("全局透明代理（需管理员权限）"),
            huxerui::Switch(shownTun)
                .OnChanged([tasks, toast, dialog, clipboard, text_color,
                            hint_color, tunEnabled, pending, modelTun,
                            coreModel](bool on) {
                    if (pending.Get()) return;
                    const bool previous = modelTun;
                    tunEnabled = on;
                    pending = true;
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        DesktopModeApplyResult result;
                        try {
                            result = co_await RunOnTaskThread(
                                [on] { return ApplyDesktopTun(on); });
                        } catch (const std::exception& exception) {
                            result.error = exception.what();
                        }
                        // 成功：TUN 切换在内核运行时是重启流程（耗时较长），权威值
                        // 立刻写透模型，不必等下一拍；失败目标值校验后回落。
                        pending = false;
                        if (result.status == DesktopModeApplyStatus::Applied) {
                            coreModel->Update([on](CoreView& view) {
                                view.core.tunEnabled = on;
                            });
                            co_return;
                        }
                        if (tunEnabled.Get() == on) tunEnabled = previous;
                        if (result.status == DesktopModeApplyStatus::ElevationRequested) {
                            toast.Show(Localized("已请求管理员权限重启，请在新窗口开启 TUN"));
                            co_return;
                        }
                        if (result.status == DesktopModeApplyStatus::PermissionDenied) {
                            ShowTunGuideDialog(dialog, clipboard, toast,
                                               text_color, hint_color);
                            co_return;
                        }
                        toast.Show(result.error.empty()
                                       ? Localized("TUN 设置失败")
                                       : huxerui::StringVariant(result.error));
                    });
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

// 桌面：内核启动/停止悬浮按钮（右下角，与移动端的 VPN 悬浮按钮同形制）。
// 「正式启动」是独立动作：只负责拉起/停止内核，启动时按已记录的 TUN 与
// 系统代理意图恢复；不再由各个开关隐式触发内核。
[[huxerui::composable]] huxerui::View DesktopHomeFloatingAction(
    huxerui::View page, bool compact) {
    static_cast<void>(compact);
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto busy = huxerui::UseState(false);
    auto pending = huxerui::UseState(false);
    // 内核状态来自 CoreModel（见 core_model.h）：本按钮不再自己 0.5s 轮询。
    const auto coreModel = huxerui::UseService<CoreModel>();
    const CoreView coreView = coreModel->view.Get();
    const core::CoreState modelState = coreView.core.state;
    auto coreState = huxerui::UseState(modelState);
    const core::CoreState shownState = pending.Get() ? coreState.Get() : modelState;

    huxerui::View base = page;
    const bool running = shownState == core::CoreState::Running;
    const bool starting = shownState == core::CoreState::Starting;
    const bool active = running;
    const bool enabled = !busy.Get() && !starting;

    const auto toggle = [tasks, toast, busy, pending, coreState, running,
                         modelState, coreModel] {
        if (busy.Get() || pending.Get()) return;
        const core::CoreState previous = modelState;
        if (previous == core::CoreState::Starting) return;
        coreState = running ? core::CoreState::Stopped : core::CoreState::Running;
        pending = true;
        busy = true;
        tasks.Launch([toast, busy, pending, coreState, previous, running,
                      coreModel]() -> huxerui::Task<void> {
            std::string error;
            bool failed = false;
            try {
                const bool ok = co_await RunOnTaskThread([running] {
                    auto& core = store::coreStore();
                    if (running) return core.stopCore();
                    const bool resumeSysProxy = core.systemProxyEnabled();
                    const bool resumeTun = core.tunEnabled();
                    core.startCore(store::profilesStore().selectedYaml(), false,
                                   resumeTun, resumeSysProxy);
                    return core.snapshot().state == core::CoreState::Running;
                });
                if (!ok) {
                    failed = true;
                    error = store::coreStore().snapshot().lastError;
                }
            } catch (const std::exception& exception) {
                failed = true;
                error = exception.what();
            }
            // 成功：把权威值（内核状态）写透模型，界面不必等下一拍；失败做
            // 目标值校验后回落（用户若已再点一次就不覆盖新意图）。
            pending = false;
            busy = false;
            if (failed) {
                const core::CoreState target =
                    running ? core::CoreState::Stopped : core::CoreState::Running;
                if (coreState.Get() == target) coreState = previous;
                toast.Show(error.empty()
                               ? Localized(running ? "停止内核失败" : "启动内核失败")
                               : huxerui::StringVariant(error));
                co_return;
            }
            const core::CoreState landed = running ? core::CoreState::Stopped
                                                   : core::CoreState::Running;
            coreModel->Update([landed](CoreView& view) {
                view.core.state = landed;
            });
        });
    };

    huxerui::View floating =
        huxerui::IconButton(active ? app::images::pause : app::images::play,
                            Localized(active ? "停止内核" : "启动内核"))
            .OnClick(toggle)
            .With(huxerui::Tooltip(Localized(active ? "停止内核" : "启动内核")),
                  huxerui::Frame{.width = 56.0F, .height = 56.0F},
                  huxerui::Enabled(enabled),
                  huxerui::Background(active ? theme.colors.primary_container
                                             : theme.colors.primary),
                  huxerui::Foreground(active
                                          ? theme.colors.on_primary_container
                                          : theme.colors.on_primary),
                  huxerui::CornerRadius(28.0F),
                  huxerui::Shadow{ThemeShadowColor(theme, 0.28F), {}, 14.0F,
                                  2.0F},
                  huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                     .label = huxerui::UseString(Localized(
                                         active ? "停止内核" : "启动内核"))});
    floating = WithoutIconButtonOutlines(floating);
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
        std::move(dock),
    }.With(huxerui::Grow(1.0F),
           huxerui::Align(huxerui::HorizontalAlignment::Stretch,
                          huxerui::VerticalAlignment::Stretch));
}

#endif

// ---- 卡片内容分发 ----------------------------------------------------------

[[huxerui::composable]] huxerui::View HomeCardContent(
    HomeCardKind kind, huxerui::State<HomeState> state,
    huxerui::State<std::size_t> homeMode,
    huxerui::State<bool> modePending,
    huxerui::TaskScope tasks, huxerui::ToastHandle toast) {
    const HomeState& s = state.Get();
    if (kind == HomeCardKind::Traffic) return HomeTrafficCard(s);
    if (kind == HomeCardKind::Total) return HomeTotalCard(s);
    if (kind == HomeCardKind::Mode) {
        return HomeModeCard(homeMode, modePending, tasks, toast);
    }
    if (kind == HomeCardKind::Profile) return HomeProfileCard(s);
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
    const auto proxiesModel = huxerui::UseService<ProxiesModel>();
    const auto coreModel = huxerui::UseService<CoreModel>();
    const auto profilesModel = huxerui::UseService<ProfilesModel>();
    const auto settingsModel = huxerui::UseService<SettingsModel>();
    auto state = huxerui::UseState<HomeState>({});
    auto homeMode = huxerui::UseState<std::size_t>(
        HomeModeIndex(store::coreStore().snapshot().mode));
    auto modePending = huxerui::UseState(false);
    // 首页布局是**编译期常量**（见 AGENTS.md / CLAUDE.md 第 13 条）：顺序与尺寸只由
    // kHomeCards 决定，没有编辑态、拖动排序、增删卡片与 home.layout.* 持久化，也就
    // 没有"先画默认、persistence hydrate 之后再换一份库里的布局"的首帧跳变。
    const HomeLayout order = DefaultHomeLayout();

    // 桌面：流量/连接由推送驱动（见 SubscribeHomeStreams），不占用轮询。
    huxerui::Lifecycle(
        [tasks, state] {
            const std::uint64_t subscription = SubscribeHomeStreams(tasks, state);
            return [subscription] { UnsubscribeStreamUpdates(subscription); };
        },
        0);

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
        state, homeMode, modePending, coreModel, profilesModel, proxiesModel);

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
        huxerui::View body = HomeCardContent(kind, state, homeMode, modePending,
                                             tasks, toast);
        huxerui::View cardView = Card(std::move(body));
        cards.push_back(std::move(cardView)
                            .LayoutValue<HomeCardSpan>(size)
                            .Key("home-card-" + id));
    }

    huxerui::View cardArea = cards.empty()
        ? EmptyState(Localized("首页还没有卡片"), app::images::home)
        : huxerui::View{HomeGrid(std::move(cards)).With(huxerui::Grow(1.0F))};
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
                      huxerui::Frame{.height = kHomeFloatingButtonInset})},
    }.With(huxerui::Spacing(kHomeGridGap),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    // 标题行右缘只剩内核状态图标：首页布局固定，不再有编辑/保存入口。
    huxerui::View headerActions = huxerui::Row {
        // 运行态泵是空 View（无布局），挂在这里即可让它内部的 Lifecycle 注册。
        std::move(runtimePump),
        HomeKernelStatusIcon(state),
    }.With(huxerui::Spacing(4.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));

    // 注意：首页不要在滚动内容里声明 Focusable(true)——运行时会自动把初始
    // 焦点所在的节点滚入视野，导致首页一打开就被滚到中途（且随后每次重组都可能
    // 来回滚）。可聚焦性交给内置控件自己的默认策略，分段选择器只保留语义。
    huxerui::View scrollContent =
        huxerui::ScrollView(std::move(pageBody)).With(huxerui::Grow(1.0F));
    huxerui::View page = PageScaffold(
        Localized("首页"), std::move(headerActions), std::move(scrollContent), true, false, true, kHomeGridGap);

    // 移动端启动/停止按钮脱离滚动内容，固定在底部悬浮导航之上的位置。
    huxerui::View shell = CLASHFLUX_HOME_FLOATING_ACTION(
        std::move(page), state, compact);

    return std::move(shell).With(huxerui::Grow(1.0F));
}

#undef CLASHFLUX_HOME_PLATFORM_CARD
#undef CLASHFLUX_HOME_FLOATING_ACTION

} // namespace clashflux::ui
