// app.cpp — Clash-Flux 应用声明：HuxerUI Application 单例。
// 平台入口 main() 在 platform/<platform>/main.cpp（HuxerUI CLI 生成格式）；
// UI 内容在 src/ui/*.cpp（composable 普通源，经 huxerui_add_app 的 codegen 处理）。
#include <huxerui/huxerui.h>
#include <huxerui/camera.h>

#include "ui/app.h"
#include "ui/ui.h"
#include "ui/section_tab_picker.h"
#include "ui/app_http_client.h"
#include "ui/proxies_model.h"
#if defined(__ANDROID__)
#include "ui/qr_photo_decoder.h"
#endif

const huxerui::Application application{
    clashflux::ui::AppRoot,
    huxerui::AppOptions{
        .window = {
            .title = "Clash-Flux",
            .initial_size = {1080.0F, 720.0F},
            // 允许桌面窗口缩到手机竖屏尺寸，内容按视口宽度响应。
            .minimum_size = huxerui::Size{320.0F, 480.0F},
#if defined(__ANDROID__)
            // 壳层在完整视口绘制背景，再用 SafeAreaPadding 保护交互内容。
            .content_mode = huxerui::WindowContentMode::EdgeToEdge,
#endif
            .chrome_mode = huxerui::WindowChromeMode::Custom,
            .title_bar_height = clashflux::ui::kDesktopTitleBarHeight,
        },
        .application_hooks = {
            huxerui::camera::Install,
            clashflux::ui::InstallAppHttpClient,
            clashflux::ui::InstallClashFluxUiModels,
#if defined(__ANDROID__)
            clashflux::ui::InstallQrPhotoDecoder,
#endif
        },
        .window_hooks = {clashflux::ui::InstallSectionPickerLayers},
    },
};
