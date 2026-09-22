// home_page.cpp — 可自定义首页：卡片目录（增删）+ 网格摆放（宽/高各 1..4 单位）
// + 排序（桌面鼠标拖动 / 移动端长按拖动）。
//
// 结构：
//   kHomeCards   文件作用域 #if 选出的平台卡片表（composable 体内禁止条件编译，
//                平台差异只出现在这里和平台函数选择宏上）；
//   HomeGrid     自定义布局：把「页面逻辑宽度」分成 1..4 列，按卡片声明的宽高
//                （HomeCardSpan）做左上紧凑的二维打包。编辑态与运行态共用同一
//                套算法与同一份约束，所以卡片占用的大小完全一致（所见即所得）；
//   HomeCardContent 按种类组装卡片内容，平台专属卡片经宏选择完整函数。
// 持久化：settings KV "home.layout.desktop|android"，值为 "v1:<id>:<宽>x<高>,…"。
// 编辑态只改会话内 State，点保存才写库；编辑态下点编辑按钮回滚到进入时快照。
//
// 数据流：UI 泵每 500ms 从 CoreStreams 取最新流量帧追加进 60 点环形历史
// （State<vector<TrafficPoint>>），连接快照帧只取总量字段；内核状态与启用
// 订阅每拍重读。Canvas 画家捕获历史快照，重组后按最新序列重绘。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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

const std::vector<std::string> kModeLabels{"规则", "全局", "直连"};
const std::vector<std::string> kModes{"rule", "global", "direct"};

// ---- 卡片种类与平台目录 ----------------------------------------------------

enum class HomeCardKind {
    Traffic,    // 流量曲线（标题行带实时上/下行速率）
    Total,      // 流量统计（总上传 / 总下载）
    Mode,       // 出站模式
    Profile,    // 当前订阅
    Proxy,      // 系统代理（桌面）
    Tun,        // TUN 模式（桌面）
    Vpn,        // 隧道状态（移动端）
    Background, // 后台保活（移动端）
};

struct HomeCardSpec {
    HomeCardKind kind;
    std::string_view id;     // 持久化身份，不要改名（改名会让旧布局回落默认）
    std::string_view title;  // 编辑态标题
    int width = 1;           // 默认占用列数 1..4
    int height = 1;          // 默认占用行数 1..4
};

#if defined(__ANDROID__)
// 移动端可选卡片：桌面独有的系统代理/TUN 换成隧道状态与后台保活。手机默认
// 单列，宽只影响横向占位、高决定卡片长度。
// 初始宽高同样按 FlClash 仪表盘跨度：图表 2 行、信息卡 2 行、按钮/开关类 1 行。
constexpr HomeCardSpec kHomeCards[] = {
    {HomeCardKind::Traffic, "traffic", "流量曲线", 1, 2},
    {HomeCardKind::Total, "total", "流量统计", 1, 2},
    {HomeCardKind::Mode, "mode", "出站模式", 1, 1},
    {HomeCardKind::Profile, "profile", "当前订阅", 1, 2},
    {HomeCardKind::Vpn, "vpn", "隧道状态", 1, 2},
    {HomeCardKind::Background, "background", "后台保活", 1, 1},
};
constexpr std::string_view kHomeLayoutKey = "home.layout.android";
constexpr std::string_view kDefaultCoreName = "sing-box libbox";
// 移动端：滚动优先，按住卡片 0.35s 后才进入拖动（延迟拖动 = 长按拖动）。
constexpr bool kHomeLongPressDrag = true;
constexpr std::string_view kHomeDragHint = "长按卡片拖动排序，× 移除卡片";
#define CLASHFLUX_HOME_PLATFORM_CARD(state, kind) \
    AndroidHomePlatformCard(state, kind)
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
    {HomeCardKind::Traffic, "traffic", "流量曲线", 2, 2},
    {HomeCardKind::Total, "total", "流量统计", 1, 2},
    {HomeCardKind::Mode, "mode", "出站模式", 2, 1},
    {HomeCardKind::Profile, "profile", "当前订阅", 1, 2},
    {HomeCardKind::Proxy, "proxy", "系统代理", 1, 1},
    {HomeCardKind::Tun, "tun", "TUN 模式", 1, 1},
};
constexpr std::string_view kHomeLayoutKey = "home.layout.desktop";
constexpr std::string_view kDefaultCoreName = "sing-box";
// 桌面端：鼠标按下即拖动（滚轮负责滚动，不与拖动争用指针）。
constexpr bool kHomeLongPressDrag = false;
constexpr std::string_view kHomeDragHint = "拖动卡片排序，× 移除卡片";
#define CLASHFLUX_HOME_PLATFORM_CARD(state, kind) \
    DesktopHomePlatformCard(state, kind)
#define CLASHFLUX_HOME_FLOATING_ACTION(page, state, compact) \
    DesktopHomeFloatingAction(std::move(page), state, compact)
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
constexpr float kHomeGridGap = 12.0F; // 格间距，与页面卡片间距一致
// 一行单位高度：h=1 的卡片刚好容纳标题 + 一行控件。
constexpr float kHomeGridUnitHeight = 88.0F;
constexpr float kHomeGridFallbackWidth = 360.0F;

// 页面逻辑宽度分档：1..4 列。网格布局与卡片内容的粗分档共用同一组阈值。
constexpr int HomeGridColumns(float width) {
    if (width < 480.0F) return 1;
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

// ---- 布局读写 --------------------------------------------------------------

// 版本前缀让「用户清空全部卡片」("v3:") 与「从未配置过」("") 区分开；旧前缀
// （v1/v2 / 无前缀）的布局在加载时保留卡片与顺序、补齐这一版新增的卡片，并把
// 尺寸刷新为本版默认——尺寸模型这两版一直在调（v3 起按 FlClash 实测跨度定稿），
// 旧值没有保留价值；写入 v3 之后完全尊重用户调整过的尺寸。
constexpr std::string_view kHomeLayoutPrefix = "v3:";

const HomeCardSpec* FindHomeCard(HomeCardKind kind) {
    for (const HomeCardSpec& spec : kHomeCards) {
        if (spec.kind == kind) return &spec;
    }
    return nullptr;
}

std::optional<HomeCardKind> FindHomeCard(std::string_view id) {
    for (const HomeCardSpec& spec : kHomeCards) {
        if (spec.id == id) return spec.kind;
    }
    return std::nullopt;
}

HomeLayout DefaultHomeLayout() {
    HomeLayout layout;
    layout.reserve(std::size(kHomeCards));
    for (const HomeCardSpec& spec : kHomeCards) {
        layout.push_back(HomeCardEntry{spec.kind, {spec.width, spec.height}});
    }
    return layout;
}

// "3x2" → 宽 3 高 2；缺省或非法时回落卡片默认尺寸。
HomeCardSize ParseHomeCardSize(std::string_view text,
                               const HomeCardSpec& spec) {
    const HomeCardSize fallback{spec.width, spec.height};
    const std::size_t separator = text.find('x');
    if (separator == std::string_view::npos) return fallback;
    const auto parse = [](std::string_view value) -> std::optional<int> {
        if (value.empty() || value.size() > 2) return std::nullopt;
        int number = 0;
        for (const char c : value) {
            if (c < '0' || c > '9') return std::nullopt;
            number = number * 10 + (c - '0');
        }
        return number;
    };
    const std::optional<int> width = parse(text.substr(0, separator));
    const std::optional<int> height = parse(text.substr(separator + 1));
    if (!width.has_value() || !height.has_value()) return fallback;
    return HomeCardSize{std::clamp(*width, 1, kHomeGridMaxSpan),
                        std::clamp(*height, 1, kHomeGridMaxSpan)};
}

HomeLayout ParseHomeLayout(std::string_view stored) {
    HomeLayout layout;
    std::size_t begin = 0;
    while (begin <= stored.size()) {
        const std::size_t end = stored.find(',', begin);
        const std::string_view item =
            stored.substr(begin, end == std::string_view::npos
                                     ? std::string_view::npos
                                     : end - begin);
        if (!item.empty()) {
            const std::size_t separator = item.find(':');
            const std::string_view id = separator == std::string_view::npos
                                            ? item
                                            : item.substr(0, separator);
            if (const auto kind = FindHomeCard(id)) {
                const HomeCardSpec* spec = FindHomeCard(*kind);
                const bool duplicate = std::any_of(
                    layout.begin(), layout.end(),
                    [kind](const HomeCardEntry& entry) {
                        return entry.kind == *kind;
                    });
                if (spec != nullptr && !duplicate) {
                    const HomeCardSize size =
                        separator == std::string_view::npos
                            ? HomeCardSize{spec->width, spec->height}
                            : ParseHomeCardSize(item.substr(separator + 1),
                                                *spec);
                    layout.push_back(HomeCardEntry{*kind, size});
                }
            }
        }
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return layout;
}

std::string SerializeHomeLayout(const HomeLayout& layout) {
    std::string out{kHomeLayoutPrefix};
    for (const HomeCardEntry& entry : layout) {
        const HomeCardSpec* spec = FindHomeCard(entry.kind);
        if (spec == nullptr) continue;
        if (out.size() > kHomeLayoutPrefix.size()) out.push_back(',');
        out.append(spec->id);
        out.push_back(':');
        out.append(std::to_string(
            std::clamp(entry.size.width, 1, kHomeGridMaxSpan)));
        out.push_back('x');
        out.append(std::to_string(
            std::clamp(entry.size.height, 1, kHomeGridMaxSpan)));
    }
    return out;
}

HomeLayout LoadHomeLayout() {
    const std::string stored =
        store::coreStore().setting(std::string{kHomeLayoutKey}, "");
    if (stored.empty()) return DefaultHomeLayout();
    const std::string_view raw = stored;
    const bool current = raw.starts_with(kHomeLayoutPrefix);
    std::string_view body = raw;
    if (current) {
        body = raw.substr(kHomeLayoutPrefix.size());
    } else {
        for (const std::string_view legacy : {"v1:", "v2:"}) {
            if (body.starts_with(legacy)) {
                body = body.substr(legacy.size());
                break;
            }
        }
    }
    // 未知 id（平台切换或旧版本）被丢弃；整份布局都失效时才回落默认。
    HomeLayout layout = ParseHomeLayout(body);
    if (layout.empty() && !body.empty()) return DefaultHomeLayout();
    if (!current) {
        // 旧布局：保留卡片与顺序（未知 id 已在上一步丢弃），补齐这一版新增的
        // 卡片，尺寸统一刷新为本版默认。
        std::vector<HomeCardKind> order;
        order.reserve(layout.size() + std::size(kHomeCards));
        for (const HomeCardEntry& entry : layout) order.push_back(entry.kind);
        for (const HomeCardSpec& spec : kHomeCards) {
            const bool present =
                std::any_of(order.begin(), order.end(),
                            [&spec](HomeCardKind kind) {
                                return kind == spec.kind;
                            });
            if (!present) order.push_back(spec.kind);
        }
        layout.clear();
        layout.reserve(order.size());
        for (const HomeCardKind kind : order) {
            if (const HomeCardSpec* spec = FindHomeCard(kind)) {
                layout.push_back(
                    HomeCardEntry{kind, {spec->width, spec->height}});
            }
        }
    }
    return layout;
}

void SaveHomeLayout(const HomeLayout& layout) {
    store::coreStore().setSetting(std::string{kHomeLayoutKey},
                                  SerializeHomeLayout(layout));
}

HomeLayout HomeLayoutWithCard(HomeLayout layout, HomeCardKind kind) {
    const bool present =
        std::any_of(layout.begin(), layout.end(),
                    [kind](const HomeCardEntry& entry) {
                        return entry.kind == kind;
                    });
    if (!present) {
        const HomeCardSpec* spec = FindHomeCard(kind);
        if (spec != nullptr) {
            layout.push_back(HomeCardEntry{kind, {spec->width, spec->height}});
        }
    }
    return layout;
}

HomeLayout HomeLayoutWithoutCard(HomeLayout layout, HomeCardKind kind) {
    std::erase_if(layout, [kind](const HomeCardEntry& entry) {
        return entry.kind == kind;
    });
    return layout;
}

HomeLayout HomeLayoutResized(HomeLayout layout, HomeCardKind kind, int width,
                             int height) {
    for (HomeCardEntry& entry : layout) {
        if (entry.kind != kind) continue;
        entry.size.width = std::clamp(width, 1, kHomeGridMaxSpan);
        entry.size.height = std::clamp(height, 1, kHomeGridMaxSpan);
        break;
    }
    return layout;
}

// 拖到哪张卡片上就插到那张卡片原来的位置（两个方向语义一致：插在目标之前
// 意味着上行拖动落到目标位、下行拖动落到目标位之后）。
HomeLayout HomeLayoutMoved(HomeLayout layout, HomeCardKind source,
                           HomeCardKind target) {
    const auto from = std::find_if(layout.begin(), layout.end(),
                                   [source](const HomeCardEntry& entry) {
                                       return entry.kind == source;
                                   });
    if (from == layout.end() || source == target) return layout;
    const HomeCardEntry moved = *from;
    layout.erase(from);
    const auto to = std::find_if(layout.begin(), layout.end(),
                                 [target](const HomeCardEntry& entry) {
                                     return entry.kind == target;
                                 });
    if (to == layout.end()) return layout;
    layout.insert(to, moved);
    return layout;
}

// ---- 运行期状态泵 ----------------------------------------------------------

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

// ---- 通用卡片部件 ----------------------------------------------------------

// 卡片标题：所有卡片共用同一排版；卡片外壳由卡片槽统一提供。
[[huxerui::composable]] huxerui::View HomeCardHeading(const std::string& title) {
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

// 拖动预览：跟随指针的半透明浮层，由卡片槽的 DragSource 提供。
huxerui::View HomeDragPreview(const std::string& title,
                              const huxerui::ThemeSpec& theme) {
    return huxerui::Row {
        huxerui::Image(app::images::drag_indicator)
            .Fit(huxerui::ImageFit::Contain)
            .Tint(theme.colors.primary)
            .With(huxerui::Frame{.width = 18.0F, .height = 18.0F}),
        huxerui::Text(title).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kBody)
                .WithWeight(huxerui::FontWeight::SemiBold),
            theme.colors.on_surface}),
    }.With(huxerui::Frame{.width = 220.0F},
           huxerui::Padding(huxerui::EdgeInsets::Symmetric(14.0F, 12.0F)),
           huxerui::Spacing(8.0F),
           huxerui::Background(theme.colors.surface_container_highest),
           huxerui::Border(theme.colors.primary, 1.0F),
           huxerui::CornerRadius(12.0F),
           huxerui::Shadow{huxerui::Color::Rgb(0, 0, 0, 0.30F), {}, 20.0F, 2.0F},
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

// 拖动荷载：卡片 id（持久化身份）就是跨卡片传递的唯一数据。
struct HomeCardDrag {
    std::string id;
};

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
                offset_.AnimateTo(target, huxerui::TweenSpec{duration_});
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

// 流量统计：上箭头紧跟总上传、下箭头紧跟总下载（不写文字提示；实时速率在流量卡片标题行）。
[[huxerui::composable]] huxerui::View HomeTotalCard(const HomeState& s) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const huxerui::Color upColor = huxerui::Color::Rgb(245, 158, 11);  // 琥珀
    const huxerui::Color downColor = theme.colors.primary;

    // 只留「箭头紧跟数值」：↑ = 总上传，↓ = 总下载，不再写文字提示。
    const auto totalRow = [&theme](const char* arrow, huxerui::Color arrowColor,
                                   const std::string& value) {
        return huxerui::Row {
            huxerui::Text(arrow).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody)
                    .WithWeight(huxerui::FontWeight::Bold),
                arrowColor}),
            huxerui::Text(value).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody)
                    .WithWeight(huxerui::FontWeight::SemiBold),
                theme.colors.on_surface}),
            huxerui::Spacer(),
        }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    };

    return huxerui::Column {
        HomeCardHeading("流量统计"),
        totalRow("↑", upColor, formatBytes(s.totalUp)),
        totalRow("↓", downColor, formatBytes(s.totalDown)),
        huxerui::Spacer(),
    }.With(huxerui::Spacing(6.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// 流量卡片：标题行 + 曲线；曲线吃掉卡片剩余高度，任意高度都成立。
[[huxerui::composable]] huxerui::View HomeTrafficCard(const HomeState& s) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const huxerui::Color downColor = theme.colors.primary;
    const huxerui::Color upColor = huxerui::Color::Rgb(245, 158, 11);
    // 顶部品牌蓝光晕（primary 8% → 全透明）。
    huxerui::Color glowTop = theme.colors.primary;
    glowTop.alpha = 0.08F;
    huxerui::Color glowBottom = theme.colors.primary;
    glowBottom.alpha = 0.0F;

    return huxerui::Column {
        huxerui::Row {
            HomeCardHeading("流量"),
            huxerui::Spacer(),
            huxerui::Text("↑ " + formatRate(s.latest.up))
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption), upColor}),
            huxerui::Text("↓ " + formatRate(s.latest.down))
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption), downColor}),
        }.With(huxerui::Spacing(12.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Canvas(TrafficPainter(s.history, downColor, upColor,
                                       theme.colors.outline))
            .With(huxerui::Grow(1.0F)),
    }.With(huxerui::Spacing(10.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
           huxerui::Background(huxerui::LinearGradient{
               .start = {0.0F, 0.0F},
               .end = {0.0F, 1.0F},
               .stops = {{0.0F, glowTop}, {1.0F, glowBottom}},
           }));
}

// 出站模式：三枚等宽按钮（无标题文字）。选中指示块由 HomeSlidingSegments
// 在内容之下自绘，切换时做滑动补间。
[[huxerui::composable]] huxerui::View HomeModeCard(
    const HomeState& s, huxerui::State<std::optional<std::size_t>> modeOverride,
    huxerui::TaskScope tasks, huxerui::ToastHandle toast) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    std::size_t modeIndex = 0;
    for (std::size_t i = 0; i < kModes.size(); ++i) {
        if (s.core.mode == kModes[i]) modeIndex = i;
    }
    const std::size_t selected = modeOverride.Get().value_or(modeIndex);

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
                huxerui::Text(kModeLabels[i]).Style(huxerui::TextStyle{
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
                          .label = kModeLabels[i],
                          .selected = active})
                .OnClick([tasks, toast, modeOverride, i] {
                    if (!BeginOptimistic(modeOverride, i)) return;
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        const bool ok = co_await RunOnTaskThread(
                            [i] { return store::coreStore().applyMode(kModes[i]); });
                        EndOptimistic(modeOverride);
                        if (!ok) toast.Show("出站模式切换失败");
                    });
                })
                .Key("home-mode-" + std::to_string(i)));
    }

    return huxerui::Row(std::move(segments))
        .With(huxerui::Spacing(4.0F),
              huxerui::Background(theme.colors.surface_container_high),
              huxerui::CornerRadius(12.0F),
              huxerui::ClipChildren(),
              HomeSlidingSegments{selected, indicator, 8.0F, 0.18});
}

[[huxerui::composable]] huxerui::View HomeProfileCard(const HomeState& s) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const bool hasProfile = s.profileId != 0;
    const float progress =
        s.profileTotalBytes > 0
            ? std::clamp(static_cast<float>(s.profileUsedBytes) /
                             static_cast<float>(s.profileTotalBytes),
                         0.0F, 1.0F)
            : (hasProfile ? 1.0F : 0.0F);

    return huxerui::Column {
        HomeCardHeading("当前订阅"),
        huxerui::Text(s.profileName).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kBody), theme.colors.on_surface}),
        s.profileUpdated.empty()
            ? huxerui::View{huxerui::Row{}}
            : huxerui::View{
                  huxerui::Text(s.profileUpdated)
                      .Style(huxerui::TextStyle{
                          huxerui::Font::System(font_size::kCaption),
                          theme.colors.on_surface_variant})},
        huxerui::Spacer(),
        hasProfile ? huxerui::View{huxerui::ProgressBar(progress)
                                       .With(huxerui::Frame{.height = 3.0F})}
                   : huxerui::View{huxerui::Row{}},
    }.With(huxerui::Spacing(6.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// 内核状态文字：标题行状态图标的提示文本（内核状态不再单独占一张卡片）。
std::string HomeKernelStatusText(const HomeState& s) {
    if (s.core.state == core::CoreState::Running) {
        return "内核运行中 · " + (s.core.version.empty()
                                      ? std::string{kDefaultCoreName}
                                      : s.core.version);
    }
    return std::string{"内核"} + core::stateName(s.core.state);
}

// 标题行内核状态图标（放在编辑按钮之前）：运行中主色、启动中琥珀、失败错误色、
// 未运行次级色；详细状态走 Tooltip。
[[huxerui::composable]] huxerui::View HomeKernelStatusIcon(const HomeState& s) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    huxerui::Color color = theme.colors.on_surface_variant;
    if (s.core.state == core::CoreState::Running) {
        color = theme.colors.primary;
    } else if (s.core.state == core::CoreState::Starting) {
        color = huxerui::Color::Rgb(234, 179, 8);
    } else if (s.core.state == core::CoreState::Failed) {
        color = theme.colors.error;
    }
    return huxerui::Image(app::images::bolt)
        .Fit(huxerui::ImageFit::Contain)
        .Tint(color)
        .With(huxerui::Frame{.width = 20.0F, .height = 20.0F},
              huxerui::Tooltip(HomeKernelStatusText(s)),
              huxerui::Semantics{.role = huxerui::SemanticRole::Image,
                                 .label = HomeKernelStatusText(s)});
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
    const std::string status =
        vpnStatus == 2   ? "已连接"
        : vpnStatus == 1 ? "正在连接"
        : vpnStatus == 3 ? "启动失败"
                         : "未连接";
    const huxerui::Color statusColor =
        vpnStatus == 2   ? theme.colors.primary
        : vpnStatus == 1 ? huxerui::Color::Rgb(234, 179, 8)
        : vpnStatus == 3 ? theme.colors.error
                         : theme.colors.on_surface_variant;

    return huxerui::Column {
        HomeCardHeading("隧道状态"),
        huxerui::Row {
            huxerui::Text("状态").Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody),
                theme.colors.on_surface_variant}),
            huxerui::Spacer(),
            huxerui::Text(status).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody)
                    .WithWeight(huxerui::FontWeight::SemiBold),
                statusColor}),
        }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Row {
            huxerui::Text("当前线路").Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody),
                theme.colors.on_surface_variant}),
            huxerui::Spacer(),
            huxerui::Text(currentProxyLine(state.proxyGroups))
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kChip),
                    theme.colors.on_surface}),
        }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Spacer(),
    }.With(huxerui::Spacing(8.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// Android 后台保活：电池优化豁免状态 + 申请入口（自洽管理自己的轮询与提示）。
[[huxerui::composable]] huxerui::View AndroidHomeBackgroundCard() {
    const huxerui::ApplicationHandle application = huxerui::UseApplication();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto battery_ignored = huxerui::UseState(AndroidIsIgnoringBattery());

    // 授权页会暂时遮住 Activity；回到前台立即重读系统状态。
    application.OnLifecycleChange(
        [battery_ignored](huxerui::ApplicationLifecycleState state) {
            if (state == huxerui::ApplicationLifecycleState::Active) {
                battery_ignored = AndroidIsIgnoringBattery();
            }
        });

    huxerui::Lifecycle(
        [tasks, battery_ignored] {
            tasks.Launch([battery_ignored]() -> huxerui::Task<void> {
                co_await PollWhile(std::chrono::duration<double>{2.0},
                                   [battery_ignored] {
                                       battery_ignored =
                                           AndroidIsIgnoringBattery();
                                       return true;
                                   });
            });
            return [] {};
        },
        0);

    return huxerui::Column {
        SettingSwitchRow(
            "后台保活",
            "申请忽略电池优化，防止后台被杀；建议同时允许本应用自启动",
            huxerui::Switch(battery_ignored.Get())
                .OnChanged([battery_ignored, toast](bool on) {
                    if (on) {
                        battery_ignored = false;
                        AndroidRequestBackgroundKeepAlive();
                    } else {
                        // Android 不允许普通应用静默撤销自身的电池优化豁免，
                        // 关闭动作必须进入系统管理页完成。
                        AndroidOpenBatterySettings();
                        toast.Show("请在系统电池设置中关闭本应用的电池优化豁免");
                    }
                    battery_ignored = AndroidIsIgnoringBattery();
                })),
        huxerui::Spacer(),
    }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View AndroidHomePlatformCard(
    const HomeState& state, HomeCardKind kind) {
    if (kind == HomeCardKind::Vpn) return AndroidHomeVpnCard(state);
    if (kind == HomeCardKind::Background) return AndroidHomeBackgroundCard();
    return huxerui::Row{};
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
                        core.startCore(store::profilesStore().selectedYaml(),
                                       false, false);
                    } else {
                        core.startCore(store::profilesStore().selectedYaml(),
                                       false, true);
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

// 桌面：系统代理开关卡片（自洽管理自己的任务、乐观状态和失败提示）。
[[huxerui::composable]] huxerui::View DesktopHomeProxyCard() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto proxy_override = huxerui::UseState<std::optional<bool>>(std::nullopt);

    return huxerui::Column {
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
        huxerui::Spacer(),
    }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// 桌面：TUN 模式开关卡片（需要管理员权限；权限不足时引导装服务模式）。
[[huxerui::composable]] huxerui::View DesktopHomeTunCard(const HomeState& state) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const huxerui::ApplicationHandle application = huxerui::UseApplication();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto dialog = huxerui::UseDialog();
    auto clipboard = application.Clipboard();
    auto tun_override = huxerui::UseState<std::optional<bool>>(std::nullopt);
    const huxerui::Color text_color = theme.colors.on_surface;
    const huxerui::Color hint_color = theme.colors.on_surface_variant;

    return huxerui::Column {
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
        huxerui::Spacer(),
    }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View DesktopHomePlatformCard(
    const HomeState& state, HomeCardKind kind) {
    if (kind == HomeCardKind::Proxy) return DesktopHomeProxyCard();
    if (kind == HomeCardKind::Tun) return DesktopHomeTunCard(state);
    return huxerui::Row{};
}

// 桌面：内核启动/停止悬浮按钮（右下角，与移动端的 VPN 悬浮按钮同形制）。
// 「正式启动」是独立动作：只负责拉起/停止内核，启动时按已记录的 TUN 与
// 系统代理意图恢复；不再由各个开关隐式触发内核。
[[huxerui::composable]] huxerui::View DesktopHomeFloatingAction(
    huxerui::View page, const HomeState& state, bool compact) {
    static_cast<void>(compact);
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto busy = huxerui::UseState(false);
    auto override = huxerui::UseState<std::optional<bool>>(std::nullopt);

    huxerui::View base = page;
    const bool running = state.core.state == core::CoreState::Running;
    const bool starting = state.core.state == core::CoreState::Starting;
    const bool active = override.Get().value_or(running);
    const bool enabled = !busy.Get() && !starting;

    const auto toggle = [tasks, toast, busy, override, running] {
        if (busy.Get()) return;
        override = !running;
        busy = true;
        tasks.Launch([toast, busy, override, running]() -> huxerui::Task<void> {
            std::string error;
            try {
                const bool ok = co_await RunOnTaskThread([running] {
                    auto& core = store::coreStore();
                    if (running) return core.stopCore();
                    const bool resumeSysProxy = core.systemProxyEnabled();
                    const bool resumeTun =
                        core.setting("core.tun_enabled", "false") == "true";
                    core.startCore(store::profilesStore().selectedYaml(), false,
                                   resumeTun, resumeSysProxy);
                    return core.snapshot().state == core::CoreState::Running;
                });
                if (!ok) {
                    error = store::coreStore().snapshot().lastError;
                    if (error.empty()) error = running ? "停止内核失败" : "启动内核失败";
                }
            } catch (const std::exception& exception) {
                error = exception.what();
            }
            override = std::nullopt;
            busy = false;
            if (!error.empty()) toast.Show(error);
        });
    };

    huxerui::View floating =
        huxerui::IconButton(active ? app::images::pause : app::images::play,
                            active ? "停止内核" : "启动内核")
            .OnClick(toggle)
            .With(huxerui::Tooltip(active ? "停止内核" : "启动内核"),
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
                                     .label = active ? "停止内核" : "启动内核"});
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
    HomeCardKind kind, const HomeState& s,
    huxerui::State<std::optional<std::size_t>> modeOverride,
    huxerui::TaskScope tasks, huxerui::ToastHandle toast) {
    if (kind == HomeCardKind::Traffic) return HomeTrafficCard(s);
    if (kind == HomeCardKind::Total) return HomeTotalCard(s);
    if (kind == HomeCardKind::Mode) {
        return HomeModeCard(s, modeOverride, tasks, toast);
    }
    if (kind == HomeCardKind::Profile) return HomeProfileCard(s);
    return CLASHFLUX_HOME_PLATFORM_CARD(s, kind);
}

// ---- 编辑态部件 ------------------------------------------------------------

// 编辑态拖动提示条（拖动方式随平台不同：长按 vs 直接拖动）。
[[huxerui::composable]] huxerui::View HomeEditHint() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Row {
        huxerui::Image(app::images::drag_indicator)
            .Fit(huxerui::ImageFit::Contain)
            .Tint(theme.colors.primary)
            .With(huxerui::Frame{.width = 16.0F, .height = 16.0F}),
        huxerui::Text(std::string{kHomeDragHint})
            .Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption),
                theme.colors.on_surface_variant})
            .With(huxerui::Grow(1.0F)),
    }.With(huxerui::Spacing(6.0F),
           huxerui::Padding(huxerui::EdgeInsets::Symmetric(4.0F, 2.0F)),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

// 尺寸步进器：宽/高各 1..4 格。紧凑排布（22pt 图标按钮）以便塞进最小的卡片。
[[huxerui::composable]] huxerui::View HomeSizeStepper(
    const std::string& label, int value,
    std::function<void(int)> onChanged) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const auto step = [value, onChanged](int delta) {
        onChanged(std::clamp(value + delta, 1, kHomeGridMaxSpan));
    };
    return huxerui::Row {
        huxerui::Text(label).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kCaption),
            theme.colors.on_surface_variant}),
        huxerui::IconButton(app::images::remove, "减小" + label)
            .With(huxerui::Tooltip("减小" + label),
                  huxerui::Frame{.width = 20.0F, .height = 20.0F})
            .OnClick([step] { step(-1); }),
        huxerui::Text(std::to_string(value)).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kChip)
                .WithWeight(huxerui::FontWeight::Bold),
            theme.colors.on_surface}),
        huxerui::IconButton(app::images::add, "增大" + label)
            .With(huxerui::Tooltip("增大" + label),
                  huxerui::Frame{.width = 20.0F, .height = 20.0F})
            .OnClick([step] { step(1); }),
    }.With(huxerui::Spacing(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

// 编辑态卡片浮条：拖动把手 + 卡片名 + 宽/高步进 + 移除。浮在卡片内容之上，
// 不改变卡片在网格中占用的尺寸（编辑态与运行态所见即所得）。
[[huxerui::composable]] huxerui::View HomeCardEditBar(
    const std::string& title, HomeCardSize size,
    std::function<void(int width, int height)> onResize,
    std::function<void()> onRemove) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    huxerui::Color bar = theme.colors.surface_container_highest;
    bar.alpha = 0.94F;
    return huxerui::Row {
        huxerui::Image(app::images::drag_indicator)
            .Fit(huxerui::ImageFit::Contain)
            .Tint(theme.colors.primary)
            .With(huxerui::Frame{.width = 16.0F, .height = 16.0F}),
        huxerui::Text(title)
            .Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption)
                    .WithWeight(huxerui::FontWeight::Bold),
                theme.colors.on_surface_variant})
            .With(huxerui::Grow(1.0F)),
        HomeSizeStepper("宽", size.width,
                        [onResize, size](int width) {
                            onResize(width, size.height);
                        }),
        HomeSizeStepper("高", size.height,
                        [onResize, size](int height) {
                            onResize(size.width, height);
                        }),
        huxerui::IconButton(app::images::close, "移除" + title)
            .With(huxerui::Tooltip("移除卡片"),
                  huxerui::Frame{.width = 26.0F, .height = 26.0F})
            .OnClick(onRemove),
    }.With(huxerui::Spacing(4.0F),
           huxerui::Padding(huxerui::EdgeInsets::Symmetric(6.0F, 3.0F)),
           huxerui::Background(bar),
           huxerui::CornerRadius(10.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

// 编辑态「添加卡片」面板：只列出当前不在首页上的卡片。
[[huxerui::composable]] huxerui::View HomeEditPalette(
    const HomeLayout& layout, std::function<void(HomeCardKind)> onAdd) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    std::vector<huxerui::View> rows;
    for (const HomeCardSpec& spec : kHomeCards) {
        const bool present = std::any_of(
            layout.begin(), layout.end(), [&spec](const HomeCardEntry& entry) {
                return entry.kind == spec.kind;
            });
        if (present) continue;
        const HomeCardKind kind = spec.kind;
        rows.push_back(
            huxerui::Row {
                huxerui::Text(std::string{spec.title})
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kBody),
                        theme.colors.on_surface})
                    .With(huxerui::Grow(1.0F)),
                huxerui::IconButton(app::images::add,
                                    "添加" + std::string{spec.title})
                    .With(huxerui::Tooltip("添加到首页"))
                    .OnClick([onAdd, kind] { onAdd(kind); }),
            }.With(huxerui::Spacing(8.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
                .Key("home-palette-" + std::string{spec.id}));
    }

    if (rows.empty()) {
        return Card(huxerui::Row {
            huxerui::Text("所有卡片都已在首页")
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    theme.colors.on_surface_variant})
                .With(huxerui::Grow(1.0F)),
        });
    }

    return Card(huxerui::Column {
        HomeCardHeading("添加卡片"),
        huxerui::Column(std::move(rows))
            .With(huxerui::Spacing(2.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
    }.With(huxerui::Spacing(6.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
}

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
    // 卡片布局：初值在 UseState 之前读库（与 AppRoot 的主题偏好同一套写法，
    // settings 读取是轻量 KV 查询）。编辑态改动只落在 layout，保存才写库。
    auto layout = huxerui::UseState<HomeLayout>(LoadHomeLayout());
    auto savedLayout = huxerui::UseState<HomeLayout>(HomeLayout{});
    auto editing = huxerui::UseState(false);
    auto dropTarget = huxerui::UseState<std::string>("");

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

    // 进入 / 取消编辑：编辑态内改动只落在会话 State，取消时整份回滚快照；
    // 保存写库后同样更新快照，保证下一次取消回到已保存的布局。
    const auto toggleEdit = [layout, savedLayout, editing, dropTarget] {
        if (editing.Get()) {
            layout = savedLayout.Get();
            dropTarget = "";
            editing = false;
            return;
        }
        savedLayout = layout.Get();
        editing = true;
    };
    const auto saveLayout = [tasks, toast, layout, savedLayout, editing,
                             dropTarget] {
        if (!editing.Get()) return;
        tasks.Launch([layout, savedLayout, editing, dropTarget,
                      toast]() -> huxerui::Task<void> {
            const HomeLayout snapshot = layout.Get();
            co_await RunOnTaskThread([snapshot] { SaveHomeLayout(snapshot); });
            savedLayout = snapshot;
            dropTarget = "";
            editing = false;
            toast.Show("首页布局已保存");
        });
    };
    // 增删卡片会卸载被点击的节点（卡片/添加按钮随布局变化消失），
    // 状态写入让出一拍再执行。
    const auto removeCard = [tasks, layout](HomeCardKind kind) {
        tasks.Launch([layout, kind]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            layout = HomeLayoutWithoutCard(layout.Get(), kind);
        });
    };
    const auto addCard = [tasks, layout](HomeCardKind kind) {
        tasks.Launch([layout, kind]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            layout = HomeLayoutWithCard(layout.Get(), kind);
        });
    };
    // 改尺寸只改布局值：卡片节点被键保留，不涉及卸载，直接同步写。
    const auto resizeCard = [layout](HomeCardKind kind, int width, int height) {
        layout = HomeLayoutResized(layout.Get(), kind, width, height);
    };

    const bool isEditing = editing.Get();
    const std::string highlight = dropTarget.Get();
    const HomeLayout order = layout.Get();

    std::vector<huxerui::View> cards;
    cards.reserve(order.size());
    for (const HomeCardEntry& entry : order) {
        const HomeCardSpec* spec = FindHomeCard(entry.kind);
        if (spec == nullptr) continue;
        const std::string id{spec->id};
        const std::string title{spec->title};
        const HomeCardSize size = entry.size;
        const HomeCardKind kind = entry.kind;

        huxerui::View body =
            HomeCardContent(kind, s, modeOverride, tasks, toast);
        huxerui::View slot = Card(std::move(body));
        if (isEditing) {
            // 编辑态：卡片内容停用交互，浮条叠在内容之上（不占额外高度，
            // 卡片格子尺寸与运行态完全一致）。
            slot = huxerui::Stack {
                std::move(slot).With(
                    huxerui::Enabled(false),
                    huxerui::Align(huxerui::HorizontalAlignment::Stretch,
                                   huxerui::VerticalAlignment::Stretch)),
                huxerui::Column {
                    HomeCardEditBar(
                        title, size,
                        [resizeCard, kind](int width, int height) {
                            resizeCard(kind, width, height);
                        },
                        [removeCard, kind] { removeCard(kind); }),
                }.With(huxerui::Align(huxerui::HorizontalAlignment::Stretch,
                                      huxerui::VerticalAlignment::Start),
                       huxerui::Padding(huxerui::EdgeInsets::All(6.0F))),
            };
            if (highlight == id) {
                slot = std::move(slot)
                           .With(huxerui::Border(theme.colors.primary, 2.0F));
            }
            // 拖动源 + 放置目标同挂卡片槽：桌面鼠标直接拖，移动端长按后拖。
            slot = std::move(slot)
                       .With(huxerui::DragSource(
                                 HomeCardDrag{id},
                                 [title, theme]() -> huxerui::View {
                                     return HomeDragPreview(title, theme);
                                 },
                                 kHomeLongPressDrag
                                     ? huxerui::DragGesture{
                                           .minimum_press_duration =
                                               std::chrono::duration<double>{
                                                   0.35}}
                                     : huxerui::DragGesture{}),
                             huxerui::DropTarget::Accepts<HomeCardDrag>(
                                 [id](const HomeCardDrag& payload) {
                                     return payload.id != id;
                                 }))
                       .On<huxerui::DragSourceEvents::Ended>(
                           [dropTarget](const huxerui::DragDropResult&) {
                               dropTarget = "";
                           })
                       .On<huxerui::DragSourceEvents::Canceled>(
                           [dropTarget](const huxerui::DragEvent&) {
                               dropTarget = "";
                           })
                       .On<huxerui::DropEvents<HomeCardDrag>::Entered>(
                           [dropTarget, id](const HomeCardDrag&,
                                            const huxerui::DropEvent&) {
                               dropTarget = id;
                           })
                       .On<huxerui::DropEvents<HomeCardDrag>::Exited>(
                           [dropTarget, id](const HomeCardDrag&,
                                            const huxerui::DropEvent&) {
                               if (dropTarget.Get() == id) dropTarget = "";
                           })
                       .On<huxerui::DropEvents<HomeCardDrag>::Dropped>(
                           [tasks, layout, dropTarget,
                            id](const HomeCardDrag& payload,
                                const huxerui::DropEvent&) {
                               const std::string sourceId = payload.id;
                               dropTarget = "";
                               if (sourceId == id) return;
                               const auto source = FindHomeCard(sourceId);
                               const auto target = FindHomeCard(id);
                               if (!source.has_value() || !target.has_value()) {
                                   return;
                               }
                               // 排序会让源卡片换位（节点移动）：状态写入
                               // 让出一拍再执行，避免在事件派发中搬动节点。
                               tasks.Launch([layout, source = *source,
                                             target = *target]()
                                                -> huxerui::Task<void> {
                                   co_await huxerui::Delay(
                                       std::chrono::duration<double>{0});
                                   layout = HomeLayoutMoved(layout.Get(),
                                                            source, target);
                               });
                           });
        }
        cards.push_back(std::move(slot)
                            .LayoutValue<HomeCardSpan>(size)
                            .Key("home-card-" + id));
    }

    huxerui::View grid = cards.empty()
        ? huxerui::View{Card(huxerui::Text(
              isEditing ? "首页还没有卡片，从下面的列表里添加"
                        : "首页还没有卡片，点右上角编辑按钮添加")
              .Style(huxerui::TextStyle{
                  huxerui::Font::System(font_size::kBody),
                  theme.colors.on_surface_variant}))}
        : huxerui::View{HomeGrid(std::move(cards)).With(huxerui::Grow(1.0F))};

    huxerui::View pageBody = huxerui::Column {
        isEditing ? huxerui::View{HomeEditHint()} : huxerui::View{huxerui::Row{}},
        std::move(grid),
        isEditing ? HomeEditPalette(order, [addCard](HomeCardKind kind) {
            addCard(kind);
        })
                  : huxerui::View{huxerui::Row{}},
        // 悬浮启动按钮压在滚动内容之上，尾部留出等高的空白，避免最后一张
        // 卡片被按钮遮住（紧凑视口本来就带底部悬浮导航的空白）。
        compact ? CompactFloatingNavigationFooter()
                : huxerui::View{huxerui::Row{}.With(
                      huxerui::Frame{.height = kHomeFloatingButtonInset})},
    }.With(huxerui::Spacing(12.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    // 编辑 / 保存图标按钮固定在标题行右缘，与标题同一行居中对齐（紧凑视口
    // 也保持同一行，不折到标题下方）。
    huxerui::View headerActions = huxerui::Row {
        HomeKernelStatusIcon(s),
        huxerui::IconButton(isEditing ? app::images::close : app::images::edit,
                            isEditing ? "取消编辑" : "编辑首页")
            .With(huxerui::Tooltip(isEditing ? "取消编辑" : "编辑首页"))
            .OnClick(toggleEdit),
        huxerui::IconButton(app::images::save, "保存布局")
            .With(huxerui::Tooltip("保存布局"), huxerui::Enabled(isEditing))
            .OnClick(saveLayout),
    }.With(huxerui::Spacing(4.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));

    // 注意：首页不要在滚动内容里声明 Focusable(true)——运行时会自动把初始
    // 焦点所在的节点滚入视野，导致首页一打开就被滚到中途（且随后每次重组都可能
    // 来回滚）。可聚焦性交给内置控件自己的默认策略，分段选择器只保留语义。
    huxerui::View page = PageScaffold(
        "首页", std::move(headerActions),
        huxerui::ScrollView(std::move(pageBody)).With(huxerui::Grow(1.0F)),
        true);

    // 移动端启动/停止按钮脱离滚动内容，固定在底部悬浮导航之上的位置。
    return CLASHFLUX_HOME_FLOATING_ACTION(std::move(page), s, compact);
}

#undef CLASHFLUX_HOME_PLATFORM_CARD
#undef CLASHFLUX_HOME_FLOATING_ACTION

} // namespace clashflux::ui
