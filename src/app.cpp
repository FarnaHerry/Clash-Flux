// app.cpp — Clash-Flux 应用声明：HuxerUI Application 单例。
// 平台入口 main() 在 platform/<platform>/main.cpp（HuxerUI CLI 生成格式）；
// UI 内容在 src/ui/*.cpp（composable 普通源，经 huxerui_add_app 的 codegen 处理）。
#include <huxerui/huxerui.h>

#include "ui/app.h"

const huxerui::Application application{
    clashflux::ui::AppRoot,
    huxerui::AppOptions{
        .window = {
            .title = "Clash-Flux",
            .initial_size = {1080.0F, 720.0F},
            .minimum_size = huxerui::Size{760.0F, 560.0F},
            .chrome_mode = huxerui::WindowChromeMode::Custom,
            .title_bar_height = 28.0F,
        }},
};
