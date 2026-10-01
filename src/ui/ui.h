// ui.h — Clash-Flux HuxerUI 前端内部声明（UI 层是普通 C++ 源，经 huxerui_add_app
// codegen；composable 定义只在 .cpp，见 .claude/skills/huxerui-app-development）。
#pragma once

#include <huxerui/huxerui.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app_http_client.h"

namespace clashflux::ui {

// 桌面图标栏和标题栏 logo 共用几何，保证两者中心线由同一宽度定义。
inline constexpr float kTopNavigationIndicatorSize = 44.0F;
inline constexpr float kTopNavigationItemHeight = 48.0F;
inline constexpr float kTopNavigationSpacing = 12.0F;
inline constexpr float kTopNavigationItemVerticalMargin =
    (kTopNavigationSpacing -
     (kTopNavigationItemHeight - kTopNavigationIndicatorSize)) / 2.0F;
inline constexpr float kTopNavigationRailWidth =
    kTopNavigationIndicatorSize + 2.0F * kTopNavigationSpacing;

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
inline constexpr bool ProxyTestUsesNativeGroup() noexcept { return true; }
#else
inline void AndroidStartVpn() noexcept {}
inline void AndroidStopVpn() noexcept {}
inline void AndroidRequestBackgroundKeepAlive() noexcept {}
inline void AndroidRequestIgnoreBattery() noexcept {}
inline void AndroidOpenBatterySettings() noexcept {}
inline bool AndroidIsIgnoringBattery() noexcept { return false; }
inline int AndroidVpnState() noexcept { return 0; }
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
    std::string name; // 内核 tag，始终用于 API 与持久化
    std::string displayName;
    std::map<std::string, std::string> nodeLabels;
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

std::vector<huxerui::MenuEntry> BuildProxyLineMenu(
    const std::vector<ProxyGroupSnapshot>& groups,
    std::function<void(const std::string&, const std::string&)> on_select);

// 策略组快照不在本层暴露取数接口：唯一来源是 ProxiesModel（proxies_model.h），
// 由 AppRoot 驱动刷新，页面/托盘只读它的 State。下面两个调用均可能触发阻塞
// REST，调用方必须经 RunOnTaskThread 执行；Android 的切换由 libbox bridge 提供
// 同名能力。
bool SelectProxyLine(const std::string& group, const std::string& name);
bool StartProxyGroupTest(const std::string& group);

// 指定订阅的保真度一行摘要（"跳过 3 个节点 · 降级 1 个组"）。阻塞：会为了拿账本
// 编译一次订阅原文（不 spawn、不写盘、不联网），调用方必须经 RunOnTaskThread；
// 无降级或无法编译时返回空串。见 docs/singbox-layers-and-fidelity.md §2。
std::string ProfileFidelitySummary(std::int64_t profileId);


// 桌面端系统代理/TUN 开关共用的操作结果。实际切换包含阻塞的系统或内核
// 操作，调用方必须把 ApplyDesktop* 放到 RunOnTaskThread 中执行。
enum class DesktopModeApplyStatus {
    Applied,
    ElevationRequested,
    PermissionDenied,
    Failed,
};

struct DesktopModeApplyResult {
    DesktopModeApplyStatus status = DesktopModeApplyStatus::Failed;
    std::string error;
};

DesktopModeApplyResult ApplyDesktopSystemProxy(bool enabled);
DesktopModeApplyResult ApplyDesktopTun(bool enabled);

// Android stop requests only enqueue the service shutdown. Call this from a
// task thread when a caller needs to wait for the optimistic UI state to settle.
void WaitForAndroidVpnStopped() noexcept;

// 全项目统一字号阶梯（pt）：控件/正文跟随 SDK 默认 14，不再散落硬编码字面量。
namespace font_size {
inline constexpr float kCaption = 11.0F;  // 徽标、状态小字
inline constexpr float kChip = 12.0F;     // 紧凑部件文字
inline constexpr float kBody = 14.0F;     // 正文/按钮/输入框（SDK 默认）
inline constexpr float kMonoBody = 13.0F; // 等宽正文（日志、连接元数据）
inline constexpr float kBodySmall = 13.0F; // 列表项标题（比正文小一号）
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

// 订阅列表的页面句柄：唯一来源是应用级 ProfilesModel（见 profiles_model.h），
// 这里的 StateList 只是它的镜像（AppRoot 以模型 State 为依赖同步）。订阅页、
// 规则页（含「添加全局路由规则」的目标连接列表）等消费同一份，不再各自向存储
// 层要快照。selection_pending 与模型共享：为 true 时模型暂停发布，避免未确认
// 的选中态被闪回。
// 定义在 profiles_cache.h（成员是 StateList<db::Profile>，属于
// clashflux.model 的模块内类型，只能在 import 之后见到，故不能在本头文件
// 定义）；这里只前向声明，函数声明按值传不完整类型是合法的，用到定义的
// 翻译单元在 import clashflux.db 之后包含 profiles_cache.h。
struct ProfilesCache;

// ---- 页面（定义在各自 .cpp，均为 [[huxerui::composable]]）----
// active=false 表示该一级页当前不可见（Pager/IndexedPages 的其它页）：页面仍
// 会被挂载以保住自身 State 与 Lifecycle，但只返回空占位，不构建重子树。
huxerui::View HomePage(huxerui::State<std::size_t> navPage, bool active); // 首页（概览）
huxerui::View ProfilesPage(ProfilesCache profiles, bool active);   // 订阅
huxerui::View ProxiesPage(bool active);    // 代理
huxerui::View RulesPage(ProfilesCache profiles,
                        std::function<void()> onBack = {},
                        bool active = true);                      // 规则
huxerui::View ConnectionsPage(std::function<void()> onBack = {},
                              bool active = true);                // 连接
huxerui::View LogsPage(std::function<void()> onBack = {},
                       bool active = true);                       // 日志
// 手机端二级页：由设置页「更多」入口 push 到 NavigationStack，页面自带的
// 进入/返回动画与一级页切换动画互相独立；返回箭头与系统返回键统一弹栈。
#if defined(__ANDROID__)
huxerui::View AndroidProfilesPage(
    huxerui::NavigationController navigation, ProfilesCache profiles,
    bool active = true);
huxerui::View AndroidRulesPage(ProfilesCache profiles);
huxerui::View AndroidConnectionsPage();
huxerui::View AndroidLogsPage();
#endif
// 设置页持有主题模式 State（AppRoot 传入）。
huxerui::View SettingsPage(huxerui::State<int> themeMode,
                           huxerui::State<std::size_t> navPage,
                           ProfilesCache profiles, bool active);

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

huxerui::View WithoutIconButtonOutlines(huxerui::View content);

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

// 可选中卡片（全项目统一的列表项原语，形状以代理页节点卡为标准）：
// 8pt 圆角、Symmetric(10, 8) 内边距、未选中 islands.active 表面 / 选中 primary
// 实心底、整卡可点、Semantics 为 Button。`on_click` 为空 = 不可交互（去掉焦点与点击，
// 例如 urltest 组里的节点）。
//
// 文字色由 ResolveSelectableTileColors() 给出同一套：主文字 fg、次级 muted、更弱
// faint；选中时一律 on_primary（次级用降透明度），错误色仍由调用方用
// theme.colors.error。解析函数用 Resolve 前缀：结构体与工厂同名会被函数名遮蔽。
struct SelectableTileColors {
    huxerui::Color surface;
    huxerui::Color fg;
    huxerui::Color muted;
    huxerui::Color faint;
};

SelectableTileColors ResolveSelectableTileColors(
    const huxerui::ThemeSpec& theme, bool selected);
huxerui::View SelectableTile(huxerui::View content, bool selected,
                             std::string semantics_label,
                             std::function<void()> on_click);

// 注意（踩过的坑）：**不要在 composable 的返回值上补 `Background/Border/CornerRadius`**。
// composable 返回的是 hcg 生成的 Scope 包装节点，那些 paint 修饰符只会画在整棵子树
// 背后，从子节点圆角差里漏出一圈底色（首页「当前订阅」卡、订阅卡都出现过「深灰卡 +
// 蓝色圆角边」）。表面/圆角/描边必须在建树时确定：要么写进 `Card`/`SelectableTile`
// 这类建树型 composable，要么直接写在返回的 `Column{...}.With(...)` 上。

// 选中容器（primary 实心底）里的进度条：ProgressBar 的轨道/指示色来自主题，
// 铺在 primary 底上会看不见，因此选中态改用 on_primary 前后景自绘同尺寸圆角条。
// 未选中的容器继续用 ProgressBar（主题色正常显示）。
huxerui::View SelectedProgressBar(float progress, huxerui::Color foreground,
                                  float height);

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

// 二级分区标签栏（全项目统一的页内分区切换，实现与样式说明见 common.cpp）：
// 代理页分组、规则页「订阅规则/全局路由」、订阅页类型分区共用。key 参与
// 选中匹配与节点 Key，label 是展示文本；badge 是可选角标（非空时以警示色显示在
// label 之后，例如被 sing-box 降级的策略组），留空即无角标。
struct SectionTab {
    std::string key;
    std::string label;
    std::string badge;
};
huxerui::View SectionTabBar(const std::vector<SectionTab>& tabs,
                            const std::string& selectedKey,
                            std::function<void(const std::string&)> onSelect);

// 只建当前分区时使用的挂载入场过渡：direction 为 -1（左）、0（不动）、1（右）。
huxerui::View ProxyGroupPage(huxerui::View content, int direction);

// 分区内容的左右滑动切换（与 SectionTabBar 配套）：返回 PointerIntercept 处理器，
// 由页面挂到内容滚动节点上；手势状态由页面持有一对。onPrev/onNext 为空表示
// 该方向无页可切（此时不认领手势，交给外层容器，例如手机端 Pager 整页翻）。
// 阈值与「认领必须早于 Pager」的不变量见 src/ui/section_swipe.h。
//
// 是否启用滑动是分区组件自己的能力，默认值由组件自己给出（下面的编译期常量），
// 页面在调用处直接引用它——不接入应用设置、不需要用户态开关，也不要求调用方
// 从上层把开关传进来。个别页面要关闭时只改自己调用处的那一处判断。
inline constexpr bool kSectionTabsSwipeDefault = true;

std::function<bool(const huxerui::PointerEvent&)> SectionTabSwipeHandler(
    huxerui::State<huxerui::Point> origin, huxerui::State<bool> owned,
    std::function<void()> onPrev, std::function<void()> onNext);

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

// Android 订阅自动更新：任务线程列出到期订阅，应用共享 HTTP 客户端在 UI
// 协程中抓取并由 store 收尾。桌面刷新由 DesktopProfileRefreshPump 走 curl。
huxerui::Task<int> AndroidRefreshProfilesDueOnce(
    std::shared_ptr<AppHttpClient> http);

} // namespace clashflux::ui
