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
            // 桌面不承载手机布局：最小宽度始终落在 Medium 视口。
            .minimum_size = huxerui::Size{720.0F, 520.0F},
            .chrome_mode = huxerui::WindowChromeMode::Custom,
            .title_bar_height = 24.0F,
        }},
};
