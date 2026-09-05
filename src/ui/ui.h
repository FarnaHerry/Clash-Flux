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
inline constexpr float kTitleBarContentHeight = 28.0F;

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

// 页面骨架：标题行（标题 + 右缘动作）+ 内容区（Grow 吃满）。
huxerui::View PageScaffold(const std::string& title, huxerui::View actions,
                           huxerui::View content);

// 卡片容器：surface_container 底 + 圆角 + 内边距。
huxerui::View Card(huxerui::View content);

// 内核状态胶囊：圆点 + 状态文字（标题栏用）。独立 composable，内部自订阅轮询，
// 避免 AppRoot 每拍重组。
huxerui::View CoreStatusPill();

} // namespace clashflux::ui
