// ui.h — Clash-Flux HuxerUI 前端内部声明（UI 层是普通 C++ 源，经 huxerui_add_app
// codegen；composable 定义只在 .cpp，见 .claude/skills/huxerui-app-development）。
#pragma once

#include <huxerui/huxerui.h>

#include <cstdint>
#include <string>
#include <vector>

namespace clashflux::ui {

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

// TUN 权限引导弹窗（core::tunGate()==Denied 时调用，UI 线程）：Linux 引导安装
// 服务模式（应用保持非 root，root 只在服务侧），展示可复制终端指令。纯函数
// 无钩子，可在任务协程续体里调。
void ShowTunGuideDialog(huxerui::DialogHandle dialog, huxerui::Color textColor,
                        huxerui::Color hintColor);

} // namespace clashflux::ui
