// ui.h — Clash-Flux HuxerUI 前端内部声明（UI 层是普通 C++ 源，经 huxerui_add_app
// codegen；composable 定义只在 .cpp，见 .claude/skills/huxerui-app-development）。
#pragma once

#include <huxerui/huxerui.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace clashflux::ui {

template <typename T>
bool BeginOptimistic(huxerui::State<std::optional<T>> state, T value) {
    if (state.Get().has_value()) return false;
    state = std::move(value);
    return true;
}

template <typename T>
void EndOptimistic(huxerui::State<std::optional<T>> state) {
    state = std::nullopt;
}

// Android VPN 隧道开关（设置页 VPN 卡 → VpnService consent/前台服务）。
// 这些是 Android bridge 的平台前缀函数；桌面目标提供空操作桩，避免通用
// 组件在控件内部再写平台分支。
#ifdef __ANDROID__
extern "C" void clashflux_android_start_vpn() noexcept;
extern "C" void clashflux_android_stop_vpn() noexcept;
inline void AndroidStartVpn() noexcept { clashflux_android_start_vpn(); }
inline void AndroidStopVpn() noexcept { clashflux_android_stop_vpn(); }
extern "C" void clashflux_android_request_background_keep_alive() noexcept;
inline void AndroidRequestBackgroundKeepAlive() noexcept {
    clashflux_android_request_background_keep_alive();
}
extern "C" void clashflux_android_request_ignore_battery() noexcept;
inline void AndroidRequestIgnoreBattery() noexcept {
    clashflux_android_request_ignore_battery();
}
extern "C" void clashflux_android_open_battery_settings() noexcept;
inline void AndroidOpenBatterySettings() noexcept {
    clashflux_android_open_battery_settings();
}
extern "C" bool clashflux_android_is_ignoring_battery() noexcept;
inline bool AndroidIsIgnoringBattery() noexcept {
    return clashflux_android_is_ignoring_battery();
}
extern "C" int clashflux_android_vpn_state() noexcept;
inline int AndroidVpnState() noexcept { return clashflux_android_vpn_state(); }
extern "C" const char* clashflux_android_proxy_groups() noexcept;
extern "C" const char* clashflux_android_connections() noexcept;
extern "C" bool clashflux_android_close_connection(const char*) noexcept;
extern "C" bool clashflux_android_close_all_connections() noexcept;
extern "C" bool clashflux_android_select_outbound(const char* group,
                                                    const char* name) noexcept;
extern "C" bool clashflux_android_url_test(const char* group) noexcept;
inline bool TriggerProxyGroupTest(const std::string& group) noexcept {
    return clashflux_android_url_test(group.c_str());
}
void AndroidScanQr(
    std::function<void(std::optional<std::string>)> on_result);
inline constexpr bool ProxyTestUsesNativeGroup() noexcept { return true; }
#else
inline void AndroidStartVpn() noexcept {}
inline void AndroidStopVpn() noexcept {}
inline void AndroidRequestBackgroundKeepAlive() noexcept {}
inline void AndroidRequestIgnoreBattery() noexcept {}
inline void AndroidOpenBatterySettings() noexcept {}
inline bool AndroidIsIgnoringBattery() noexcept { return false; }
inline int AndroidVpnState() noexcept { return 0; }
inline void AndroidScanQr(
    std::function<void(std::optional<std::string>)> on_result) {
    if (on_result) on_result(std::nullopt);
}
inline const char* clashflux_android_connections() noexcept { return ""; }
inline bool clashflux_android_close_connection(const char*) noexcept { return false; }
inline bool clashflux_android_close_all_connections() noexcept { return false; }
inline bool TriggerProxyGroupTest(const std::string&) noexcept { return true; }
inline constexpr bool ProxyTestUsesNativeGroup() noexcept { return false; }
#endif

// Replace a snapshot-backed collection without storing the whole collection in
// one State value. StateList keeps the list identity stable so virtualized
// views can retain their item state across refreshes.
template <class T>
void ReplaceStateList(huxerui::StateList<T> list, std::vector<T> values) {
    list.Clear();
    for (auto& value : values) {
        list.PushBack(std::move(value));
    }
}

// 当前运行内核公开的可切换策略组快照。桌面来自 clash_api，Android 来自
// libbox CommandClient；UI 只依赖这份跨平台模型，不直接知道平台 API。
struct ProxyGroupSnapshot {
    std::string name;
    std::string type;
    std::string current;
    std::vector<std::string> nodes;
    std::map<std::string, int> delays;
    // sing-box URLTest/Fallback groups are readable but cannot be changed
    // with SelectOutbound; only selector groups are manually selectable.
    bool selectable = false;

    bool operator==(const ProxyGroupSnapshot&) const = default;
};

inline bool IsDarkTheme(const huxerui::ThemeSpec& theme) noexcept {
    const huxerui::Color background = theme.colors.background;
    const float brightness = 0.2126F * background.red +
                             0.7152F * background.green +
                             0.0722F * background.blue;
    return brightness < 0.5F;
}

inline huxerui::Color SemanticSuccessColor(const huxerui::ThemeSpec& theme) {
    return IsDarkTheme(theme) ? huxerui::Color::Rgb(108, 202, 145)
                              : huxerui::Color::Rgb(47, 128, 86);
}

inline huxerui::Color SemanticWarningColor(const huxerui::ThemeSpec& theme) {
    return IsDarkTheme(theme) ? huxerui::Color::Rgb(230, 184, 102)
                              : huxerui::Color::Rgb(144, 96, 8);
}

inline huxerui::Color DelayLevelColor(const huxerui::ThemeSpec& theme, int delay,
                                      bool unavailable = false) {
    if (unavailable || delay <= 0) return theme.colors.error;
    if (delay < 300) return SemanticSuccessColor(theme);
    if (delay < 600) return theme.colors.primary;
    if (delay < 1000) return SemanticWarningColor(theme);
    return theme.colors.error;
}

std::vector<ProxyGroupSnapshot> ParseProxyGroups(const std::string& body);
std::vector<huxerui::MenuEntry> BuildProxyLineMenu(
    const std::vector<ProxyGroupSnapshot>& groups,
    std::function<void(const std::string&, const std::string&)> on_select);

// 这两个调用均可能触发阻塞 REST，调用方必须经 RunOnTaskThread 执行；Android
// 的快照/切换由 libbox bridge 提供同名能力。
std::string ProxyGroupsSnapshot();
bool SelectProxyLine(const std::string& group, const std::string& name);
bool StartProxyGroupTest(const std::string& group);
// Android stop requests only enqueue the service shutdown. Call this from a
// task thread when a caller needs to wait for the optimistic UI state to settle.
void WaitForAndroidVpnStopped() noexcept;

// 全项目统一字号阶梯（pt）：控件/正文跟随 SDK 默认 14，不再散落硬编码字面量。
namespace font_size {
inline constexpr float kCaption = 11.0F;  // 徽标、状态小字
inline constexpr float kChip = 12.0F;     // 紧凑部件文字
inline constexpr float kBody = 14.0F;     // 正文/按钮/输入框（SDK 默认）
inline constexpr float kMonoBody = 13.0F; // 等宽正文（日志、连接元数据）
inline constexpr float kTitle = 20.0F;    // 页面/弹窗标题
} // namespace font_size

// Compact 悬浮导航覆盖在内容岛底部时，滚动内容需要一个真正属于滚动内容
// 的尾部空白，确保最后一项可以滑到悬浮岛上方，而不是被悬浮层遮住。
inline constexpr float kCompactFloatingNavigationInset = 96.0F;

inline huxerui::View CompactFloatingNavigationFooter() {
    return huxerui::Spacer{}.With(
        huxerui::Frame{.height = kCompactFloatingNavigationInset});
}

// ---- 岛屿结构（对齐 apitab island 模型）----
// 语义层级：页面通过层级选表面，不直接依赖 Material 的 surface_container_* 命名；
// 颜色仍由当前 ThemeSpec 派生，深浅主题共用组件。
enum class IslandLevel {
    Base,    // 一级岛（页面根）
    Raised,  // 二级岛（卡片/分组）
    Active,  // 选中/强调面
    Overlay, // 浮动面（弹层、胶囊）
    Danger,  // 危险面（error 半透明）
};

struct IslandTheme {
    float page_gap;        // 岛间缝隙（透出窗口底色「海面」）
    float island_padding;  // 一级岛内边距
    float island_radius;   // 一级岛圆角 16pt
    float nested_radius;   // 二级岛/浮动菜单圆角 8pt
    huxerui::Color ocean;   // 海面（窗口背景）
    huxerui::Color base;    // 一级岛表面
    huxerui::Color raised;  // 二级岛表面
    huxerui::Color active;
    huxerui::Color overlay;
    huxerui::Color outline_soft;
};

IslandTheme ResolveIslandTheme(const huxerui::ThemeSpec& theme);

// 内层同心圆角：inner = outer − inset，下限 4pt。
float ConcentricRadius(float outer_radius, float inset);

// 岛屿原语：Surface 负责语义表面/圆角/内边距；Section 在其上提供标题+内容排版。
huxerui::View IslandSurface(huxerui::View content, IslandLevel level = IslandLevel::Base);
huxerui::View IslandSection(std::string title, huxerui::View content);

// ---- 页面（定义在各自 .cpp，均为 [[huxerui::composable]]）----
huxerui::View HomePage(huxerui::State<std::size_t> navPage); // 首页（概览）
huxerui::View ProfilesPage();   // 订阅
huxerui::View ProxiesPage();    // 代理
huxerui::View RulesPage(std::function<void()> onBack = {});       // 规则
huxerui::View ConnectionsPage(std::function<void()> onBack = {}); // 连接
huxerui::View LogsPage(std::function<void()> onBack = {});        // 日志
// 手机端二级页：由设置页「更多」入口 push 到 NavigationStack，页面自带的
// 进入/返回动画与一级页切换动画互相独立；返回箭头与系统返回键统一弹栈。
#if defined(__ANDROID__)
huxerui::View AndroidProfilesPage(
    huxerui::NavigationController navigation);
huxerui::View AndroidRulesPage();
huxerui::View AndroidConnectionsPage();
huxerui::View AndroidLogsPage();
#endif
// 设置页持有主题模式 State（AppRoot 传入）。
huxerui::View SettingsPage(huxerui::State<int> themeMode,
                           huxerui::State<std::size_t> navPage);

// 设置页的三个固定模块：通用 / 内核 / 关于。平台宏只在模块入口选择
// 平台函数；函数自己管理平台相关状态、任务、权限和控件。
huxerui::View DesktopGeneralSettings();
huxerui::View AndroidGeneralSettings();
huxerui::View DesktopKernelSettings();
huxerui::View AndroidKernelSettings();

// 应用壳层的平台边界：AppRoot 仅在调用点用宏选择，不持有托盘/窗口等平台状态。
void AndroidPreparePlatformDataDirectory(
    const huxerui::ApplicationHandle& application);
void DesktopPreparePlatformDataDirectory(
    const huxerui::ApplicationHandle& application);
huxerui::View AndroidProfileRefreshPump();
huxerui::View DesktopProfileRefreshPump();
huxerui::View AndroidApplicationEffects(
    const huxerui::ApplicationHandle& application,
    const huxerui::ThemeSpec& rootSpec);
huxerui::View DesktopApplicationEffects(
    const huxerui::ApplicationHandle& application,
    const huxerui::ThemeSpec& rootSpec);
huxerui::View AndroidAppContent(huxerui::View mainRow,
                               const huxerui::ThemeSpec& rootSpec);
huxerui::View DesktopAppContent(huxerui::View mainRow,
                                const huxerui::ThemeSpec& rootSpec);

// ---- 通用部件（common.cpp）----

huxerui::View PasswordField(
    huxerui::State<huxerui::TextEditingValue> password);

// 页面骨架（一级岛）：标题行（标题 + 右缘动作）+ 内容区，整体为 16pt 圆角岛，
// 落在窗口海面底色上（岛间缝隙经壳层 Spacing 透出）。
huxerui::View PageScaffold(const std::string& title, huxerui::View actions,
                           huxerui::View content,
                           bool inlineCompactActions = false);
huxerui::View SecondaryPageScaffold(huxerui::View title,
                                    huxerui::View actions, huxerui::View content,
                                    std::function<void()> onBack,
                                    bool hideBack = false);
huxerui::View PillSearchField(huxerui::State<huxerui::TextEditingValue> value,
                              const std::string& placeholder,
                              std::function<void()> onClose);

// 卡片容器（二级岛）：raised 表面 + 8pt 圆角 + 内边距。outlined=false 去掉
// 1pt 描边，仅靠表面层级区分卡片（移动端设置页等无边框场景）。
huxerui::View Card(huxerui::View content, bool outlined = true);

// 日志、连接、规则等信息列表的统一行容器。内容由页面自己组织，容器统一
// 内边距；行平铺在一级岛表面上（不逐行套卡），行间画细分隔线，divider=false
// 用于最后一行。
huxerui::View UnifiedListRow(huxerui::View content, std::string key,
                             bool compact = false, bool divider = true);

// 通用设置排版部件；这里不做任何平台判断。
huxerui::View SettingRow(const std::string& label, const std::string& hint,
                         huxerui::View control);
// Switch 专用设置行：主名称与小字描述在左侧列，开关固定在右侧。
huxerui::View SettingSwitchRow(const std::string& label,
                               const std::string& hint,
                               huxerui::View control, bool danger = false);
huxerui::View SectionTitle(const std::string& title);

// 自定义内容弹窗的卡片包裹：SDK 的 dialog.Show(ViewFactory/DialogFactory) 不给
// 内容加底板（只有标题+消息的内置形态才有 DialogStyle），统一包一层：
// overlay 表面 + 阴影 + 描边 + 16pt 圆角 + 内边距。
huxerui::View DialogCard(huxerui::View content);

// TUN 权限引导弹窗（core::tunGate()==Denied 时调用，UI 线程）：Linux 引导安装
// 服务模式（应用保持非 root，root 只在服务侧），展示终端指令 + 一键复制
// （成功/失败经 toast 提示；clipboard 从 UseApplication().Clipboard() 获取）。
// 纯函数无钩子，可在任务协程续体里调。
void ShowTunGuideDialog(huxerui::DialogHandle dialog,
                        std::shared_ptr<huxerui::Clipboard> clipboard,
                        huxerui::ToastHandle toast, huxerui::Color textColor,
                        huxerui::Color hintColor);

// Android 订阅自动更新：任务线程列出到期订阅，HuxerUI HttpClient 在 UI
// 协程中抓取并由 store 收尾。桌面刷新由 DesktopProfileRefreshPump 走 curl。
huxerui::Task<int> AndroidRefreshProfilesDueOnce(
    std::shared_ptr<huxerui::HttpClient> http);

} // namespace clashflux::ui
