// ui.h — Clash-Flux HuxerUI 前端内部声明（UI 层是普通 C++ 源，经 huxerui_add_app
// codegen；composable 定义只在 .cpp，见 .claude/skills/huxerui-app-development）。
#pragma once

#include <huxerui/huxerui.h>

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace clashflux::ui {

// 订阅下载通道：Android 的 vendored curl 无 TLS（NDK 无 OpenSSL，https 订阅
// 报 Unsupported protocol），订阅导入/手动刷新/自动更新全走 HuxerUI
// HttpClient（平台原生栈：系统 TLS/证书/代理）；桌面 curl 支持订阅级代理/
// 无效证书选项，保持不变。
#ifdef __ANDROID__
inline constexpr bool kHuxerHttpDownload = true;
#else
inline constexpr bool kHuxerHttpDownload = false;
#endif

// 平台/设备判断集中在应用层：HuxerUI 的公开环境只提供视口分级，平台宏
// 负责识别编译目标，视口再负责把 Android 区分为手机或平板。
enum class PlatformKind {
    Android,
    Linux,
    Windows,
    MacOS,
    Unknown,
};

enum class FormFactor {
    Phone,
    Tablet,
    Desktop,
    Unknown,
};

struct PlatformInfo {
    PlatformKind platform;
    FormFactor form_factor;
    huxerui::ViewportClass viewport;

    constexpr bool IsAndroid() const noexcept {
        return platform == PlatformKind::Android;
    }
    constexpr bool IsDesktop() const noexcept {
        return form_factor == FormFactor::Desktop;
    }
    constexpr bool IsPhone() const noexcept {
        return form_factor == FormFactor::Phone;
    }
    constexpr bool IsTablet() const noexcept {
        return form_factor == FormFactor::Tablet;
    }
};

struct PlatformCapabilities {
    bool tun = false;
    bool system_proxy = false;
    bool service = false;
    bool tray = false;
    bool pptp = false;
    bool openvpn = false;
};

inline constexpr PlatformKind CompileTimePlatform() noexcept {
#if defined(__ANDROID__)
    return PlatformKind::Android;
#elif defined(_WIN32)
    return PlatformKind::Windows;
#elif defined(__APPLE__)
    return PlatformKind::MacOS;
#elif defined(__linux__)
    return PlatformKind::Linux;
#else
    return PlatformKind::Unknown;
#endif
}

inline constexpr FormFactor ResolveFormFactor(
    PlatformKind platform, huxerui::ViewportClass viewport) noexcept {
    if (platform == PlatformKind::Android) {
        return viewport == huxerui::ViewportClass::Compact
                   ? FormFactor::Phone
                   : FormFactor::Tablet;
    }
    if (platform == PlatformKind::Linux || platform == PlatformKind::Windows ||
        platform == PlatformKind::MacOS) {
        return FormFactor::Desktop;
    }
    return FormFactor::Unknown;
}

inline constexpr PlatformInfo ResolvePlatformInfo(
    huxerui::ViewportClass viewport) noexcept {
    constexpr PlatformKind platform = CompileTimePlatform();
    return PlatformInfo{
        .platform = platform,
        .form_factor = ResolveFormFactor(platform, viewport),
        .viewport = viewport,
    };
}

inline constexpr PlatformCapabilities ResolvePlatformCapabilities(
    PlatformKind platform) noexcept {
    const bool desktop = platform == PlatformKind::Linux ||
                         platform == PlatformKind::Windows ||
                         platform == PlatformKind::MacOS;
    return PlatformCapabilities{
        .tun = desktop,
        .system_proxy = desktop,
        .service = platform == PlatformKind::Linux,
        .tray = desktop,
        .pptp = platform == PlatformKind::Linux ||
                platform == PlatformKind::Windows,
        .openvpn = platform == PlatformKind::Linux,
    };
}

inline constexpr std::string_view PlatformName(PlatformKind platform) noexcept {
    switch (platform) {
    case PlatformKind::Android: return "Android";
    case PlatformKind::Linux: return "Linux";
    case PlatformKind::Windows: return "Windows";
    case PlatformKind::MacOS: return "macOS";
    case PlatformKind::Unknown: return "Unknown";
    }
    return "Unknown";
}

// 平台代码既描述目标平台/设备形态，也描述该平台真正提供的能力。
// UI 只声明允许哪些代码，具体代码列表由 PlatformControl 统一生成，避免
// 每个页面各自维护 Android/桌面/运行时能力的组合判断。
enum class PlatformCode {
    Android,
    Linux,
    Windows,
    MacOS,
    Phone,
    Tablet,
    Desktop,
    CoreTun,
    SystemProxy,
    CoreService,
    SystemTray,
    PptpEngine,
    OpenVpnEngine,
};

using PlatformCodeList = std::vector<PlatformCode>;

inline PlatformCodeList ResolvePlatformCodes(const PlatformInfo& info,
                                             bool system_proxy_supported) {
    PlatformCodeList codes;
    switch (info.platform) {
    case PlatformKind::Android: codes.push_back(PlatformCode::Android); break;
    case PlatformKind::Linux: codes.push_back(PlatformCode::Linux); break;
    case PlatformKind::Windows: codes.push_back(PlatformCode::Windows); break;
    case PlatformKind::MacOS: codes.push_back(PlatformCode::MacOS); break;
    case PlatformKind::Unknown: break;
    }

    switch (info.form_factor) {
    case FormFactor::Phone: codes.push_back(PlatformCode::Phone); break;
    case FormFactor::Tablet: codes.push_back(PlatformCode::Tablet); break;
    case FormFactor::Desktop: codes.push_back(PlatformCode::Desktop); break;
    case FormFactor::Unknown: break;
    }

    const PlatformCapabilities capabilities =
        ResolvePlatformCapabilities(info.platform);
    if (capabilities.tun) codes.push_back(PlatformCode::CoreTun);
    if (capabilities.system_proxy && system_proxy_supported) {
        codes.push_back(PlatformCode::SystemProxy);
    }
    if (capabilities.service) codes.push_back(PlatformCode::CoreService);
    if (capabilities.tray) codes.push_back(PlatformCode::SystemTray);
    if (capabilities.pptp) codes.push_back(PlatformCode::PptpEngine);
    if (capabilities.openvpn) codes.push_back(PlatformCode::OpenVpnEngine);
    return codes;
}

inline bool HasPlatformCode(const PlatformCodeList& current,
                            PlatformCode expected) noexcept {
    for (const PlatformCode code : current) {
        if (code == expected) return true;
    }
    return false;
}

inline bool MatchesPlatformCode(const PlatformCodeList& current,
                                std::initializer_list<PlatformCode> allowed) noexcept {
    for (const PlatformCode code : allowed) {
        if (HasPlatformCode(current, code)) return true;
    }
    return false;
}

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

// 全项目统一字号阶梯（pt）：控件/正文跟随 SDK 默认 14，不再散落硬编码字面量。
namespace font_size {
inline constexpr float kCaption = 11.0F;  // 徽标、状态小字
inline constexpr float kChip = 12.0F;     // 紧凑部件文字
inline constexpr float kBody = 14.0F;     // 正文/按钮/输入框（SDK 默认）
inline constexpr float kMonoBody = 13.0F; // 等宽正文（日志、连接元数据）
inline constexpr float kTitle = 20.0F;    // 页面/弹窗标题
} // namespace font_size

// 标题栏内容统一高度（= AppOptions.window.title_bar_height）。
inline constexpr float kTitleBarContentHeight = 24.0F;

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
huxerui::View HomePage();       // 首页（概览）
huxerui::View ProfilesPage();   // 订阅
huxerui::View ProxiesPage();    // 代理
huxerui::View RulesPage();      // 规则
huxerui::View ConnectionsPage();// 连接
huxerui::View LogsPage();       // 日志
// 设置页持有主题模式 State（AppRoot 传入）。
huxerui::View SettingsPage(huxerui::State<int> themeMode);

// ---- 通用部件（common.cpp）----

huxerui::View PasswordField(
    huxerui::State<huxerui::TextEditingValue> password);

// 页面骨架（一级岛）：标题行（标题 + 右缘动作）+ 内容区，整体为 16pt 圆角岛，
// 落在窗口海面底色上（岛间缝隙经壳层 Spacing 透出）。
huxerui::View PageScaffold(const std::string& title, huxerui::View actions,
                           huxerui::View content);

// 卡片容器（二级岛）：raised 表面 + 8pt 圆角 + 内边距。
huxerui::View Card(huxerui::View content);

// 自定义内容弹窗的卡片包裹：SDK 的 dialog.Show(ViewFactory/DialogFactory) 不给
// 内容加底板（只有标题+消息的内置形态才有 DialogStyle），统一包一层：
// overlay 表面 + 阴影 + 描边 + 16pt 圆角 + 内边距。
huxerui::View DialogCard(huxerui::View content);

// 内核状态胶囊：圆点 + 状态文字（标题栏用）。独立 composable，内部自订阅轮询，
// 避免 AppRoot 每拍重组。
huxerui::View CoreStatusPill();

// 平台依赖控件：只在当前平台代码列表命中时创建子组合树；不匹配时直接返回
// 空 View，工厂不会执行。允许列表是 OR 关系，可同时表达“桌面能力”和“设备形态”。
// Scope 让匹配后的工厂延迟到独立子组合中执行，避免把不适用控件先拼进页面。
[[huxerui::composable]] huxerui::View PlatformControl(
    std::initializer_list<PlatformCode> allowed,
    huxerui::ViewFactory content_factory);

// TUN 权限引导弹窗（core::tunGate()==Denied 时调用，UI 线程）：Linux 引导安装
// 服务模式（应用保持非 root，root 只在服务侧），展示终端指令 + 一键复制
// （成功/失败经 toast 提示；clipboard 从 UseApplication().Clipboard() 获取）。
// 纯函数无钩子，可在任务协程续体里调。
void ShowTunGuideDialog(huxerui::DialogHandle dialog,
                        std::shared_ptr<huxerui::Clipboard> clipboard,
                        huxerui::ToastHandle toast, huxerui::Color textColor,
                        huxerui::Color hintColor);

// 订阅自动更新泵的一次迭代（kHuxerHttpDownload 通道）：任务线程列出到期
// 订阅，逐个经 HuxerUI HttpClient 抓取并由 store 收尾。返回更新条数。
// HttpClient 必须在 UI 线程任务协程里 co_await（禁入阻塞线程池）。
huxerui::Task<int> ProfilesRefreshDueOnce(
    std::shared_ptr<huxerui::HttpClient> http);

} // namespace clashflux::ui
