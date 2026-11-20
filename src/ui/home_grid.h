#pragma once

#include <span>
#include <string_view>
#include <vector>
#include <huxerui/huxerui.h>

namespace clashflux::ui {

enum class HomeCardKind {
    Download,   // 下载实时速率与曲线
    Upload,     // 上传实时速率与曲线
    Total,      // 流量统计（总上传 / 总下载）
    Mode,       // 出站模式
    Profile,    // 当前订阅
    Proxy,      // 系统代理（桌面）
    Tun,        // 虚拟网卡（桌面）
    LanIp,      // 内网 IP（当前默认路由的本机地址）
    NetCheck,   // 网络检测（公网出口 IP）
};

struct HomeCardSpec {
    HomeCardKind kind;
    std::string_view id;     // 稳定的视图身份
    std::string_view title;
    int width = 1;           // 固定列跨度
    int height = 1;          // 固定行跨度
};

inline constexpr HomeCardSpec kMobileHomeCards[] = {
    {HomeCardKind::Download, "download", "下载", 1, 2},
    {HomeCardKind::Upload, "upload", "上传", 1, 2},
    {HomeCardKind::Total, "total", "流量统计", 1, 2},
    // 出站模式是纵向单选列表（标题 + 3 个选项），占 1x2。
    {HomeCardKind::Mode, "mode", "出站模式", 1, 2},
    {HomeCardKind::Profile, "profile", "当前订阅", 1, 2},
    // 内网 IP 与网络检测在订阅卡右侧各占 1x1。
    {HomeCardKind::LanIp, "lanip", "内网 IP", 1, 1},
    {HomeCardKind::NetCheck, "netcheck", "网络检测", 1, 1},
};

inline constexpr HomeCardSpec kDesktopHomeCards[] = {
    {HomeCardKind::Download, "download", "下载", 1, 2},
    {HomeCardKind::Upload, "upload", "上传", 1, 2},
    {HomeCardKind::Total, "total", "流量统计", 1, 2},
    {HomeCardKind::Mode, "mode", "出站模式", 1, 2},
    {HomeCardKind::Profile, "profile", "当前订阅", 1, 2},
    // 内网 IP 紧跟当前订阅，1x1：只显示当前路由的一条地址。
    {HomeCardKind::LanIp, "lanip", "内网 IP", 1, 1},
    {HomeCardKind::NetCheck, "netcheck", "网络检测", 1, 1},
    {HomeCardKind::Proxy, "proxy", "系统代理", 1, 1},
    {HomeCardKind::Tun, "tun", "虚拟网卡", 1, 1},
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
inline constexpr float kHomeGridGap = 12.0F; // 卡片间距；页面边缘留白取卡片圆角
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
// 宽度按当前列数均分。卡片尺寸由布局给，不随字段更新而改变。
class HomeGrid final : public huxerui::Layout<HomeGrid> {
public:
    using Layout::Layout;

    static huxerui::LayoutResult Measure(huxerui::LayoutContext& context,
                                         huxerui::ViewNode& node,
                                         huxerui::Constraints constraints);
};

} // namespace clashflux::ui
