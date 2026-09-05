// app.h — Clash-Flux 应用根声明（定义在 app.cpp，HuxerUI composable 函数）。
#pragma once

#include <huxerui/huxerui.h>

namespace clashflux::ui {

// 应用根：自定义标题栏 + 左侧导航 + 页面切换。由 src/app.cpp 注册到 Application。
huxerui::View AppRoot();

} // namespace clashflux::ui
