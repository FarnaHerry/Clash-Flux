#pragma once

#include <span>
#include <string_view>
#include <vector>
#include <huxerui/huxerui.h>

namespace clashflux::ui {

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
    std::string_view id;     // 稳定的视图身份
    std::string_view title;
    int width = 1;           // 固定列跨度
    int height = 1;          // 固定行跨度
};

inline constexpr HomeCardSpec kMobileHomeCards[] = {
    // Lib-Charts 的紧凑绘图面最小 240×96，适配 2 行（188pt）的卡片。
    {HomeCardKind::Traffic, "traffic", "流量曲线", 2, 2},
    {HomeCardKind::Total, "total", "流量统计", 1, 2},
    // 出站模式是纵向单选列表（标题 + 3 个选项），占 1x2。
    {HomeCardKind::Mode, "mode", "出站模式", 1, 2},
    {HomeCardKind::Profile, "profile", "当前订阅", 1, 2},
    // 内网 IP 紧跟当前订阅，1x2：两行高度可容纳更多接口明细。
    {HomeCardKind::LanIp, "lanip", "内网 IP", 1, 2},
};

inline constexpr HomeCardSpec kDesktopHomeCards[] = {
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

inline constexpr std::span<const HomeCardSpec> HomeCardCatalog(bool desktop) {
    if (desktop) return kDesktopHomeCards;
    return kMobileHomeCards;
}

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

inline constexpr int kHomeGridMaxSpan = 4;   // 卡片宽高上限（格）
inline constexpr float kHomeGridGap = 12.0F; // 卡片间距，同时用于首页顶部与左右留白
// 一行单位高度：h=1 的卡片刚好容纳标题 + 一行控件。
inline constexpr float kHomeGridUnitHeight = 88.0F;
inline constexpr float kHomeGridFallbackWidth = 360.0F;

// 两列与页面的 Compact 切换同步；Medium/Expanded 至少三列，
// 更宽的内容区仍可扩展到五列。
inline constexpr int HomeGridColumns(float width, bool compact) {
    if (compact) return 2;
    if (width < 1200.0F) return 3;
    return 5;
}

// 自定义网格布局：按声明顺序把每张卡片放进第一个放得下的空位（左上紧凑），
// 宽度按当前列数均分。卡片尺寸由布局给（不是内容撑开），因此加载态与运行态
// 占据完全相同的大小。
class HomeGrid final : public huxerui::Layout<HomeGrid> {
public:
    using Layout::Layout;

    static huxerui::LayoutResult Measure(huxerui::LayoutContext& context,
                                         huxerui::ViewNode& node,
                                         huxerui::Constraints constraints);
};


// Only the card interior changes during hydration. The owner uses the same
// Card shell, key, HomeCardSpan and HomeGrid for loading and ready content.
huxerui::View HomeCardSkeleton(HomeCardKind kind, const huxerui::ThemeSpec& theme);

} // namespace clashflux::ui
