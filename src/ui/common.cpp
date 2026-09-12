// common.cpp — 岛屿原语（ResolveIslandTheme/IslandSurface/IslandSection）、
// 页面骨架（一级岛）/ 卡片（二级岛）/ 内核状态胶囊等跨页通用部件。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "app_resources.h"
#include "ui.h"
#include "task_bridge.h"

import clashflux.core;
import clashflux.config;
import clashflux.store.core;

namespace clashflux::ui {

// 所有密码输入统一走 HuxerUI 的受控 TextField 显隐能力：Secure 负责安全
// 输入策略，TrailingIcon 负责内置眼睛操作。调用方只提供受控的完整
// TextEditingValue，秘密值不会被组件复制到日志、卡片或错误文本中。
[[huxerui::composable]] huxerui::View PasswordField(
    huxerui::State<huxerui::TextEditingValue> password) {
    auto visible = huxerui::UseState(false);
    const bool isVisible = visible.Get();
    return huxerui::TextField(password.Get())
        .Label("密码")
        .Secure(!isVisible)
        .TrailingIcon(isVisible ? app::images::visibility_off
                                : app::images::visibility,
                      isVisible ? "隐藏密码" : "显示密码")
        .OnTrailingIconClick([visible] { visible = !visible.Get(); })
        .Variant(huxerui::TextFieldVariant::Outlined)
        .OnChanged([password](const huxerui::TextEditingValue& value) {
            password = value;
        });
}

[[huxerui::composable]] huxerui::View PlatformControl(
    std::initializer_list<PlatformCode> allowed,
    huxerui::ViewFactory content_factory) {
    const PlatformInfo info = ResolvePlatformInfo(huxerui::UseViewportClass());
    // 运行时探测只在编译目标具备这项通道时执行；Android 不会触碰桌面
    // sysproxy 实现，也不会把不可用的开关误加入当前代码列表。
    const PlatformCapabilities capabilities =
        ResolvePlatformCapabilities(info.platform);
    const bool system_proxy_supported =
        capabilities.system_proxy && store::coreStore().systemProxySupported();
    if (!MatchesPlatformCode(
            ResolvePlatformCodes(info, system_proxy_supported), allowed)) {
        return {};
    }

    // Scope 将匹配后的工厂放进独立子组合；不匹配时工厂根本不会执行。
    return huxerui::Scope(std::move(content_factory));
}

// TUN 权限引导弹窗（见 ui.h）。Linux 只引导安装服务模式：应用自身保持非 root
// （更安全），root 只在服务侧。命令行 = TextField 展示 + 复制按钮（runtime
// Clipboard 服务，组合期传入）。工厂 lambda 里只消费按值捕获的值，不放钩子
// （工厂在 layer 组合期执行，未经 codegen 管理的裸钩子是脆弱写法）。
void ShowTunGuideDialog(huxerui::DialogHandle dialog,
                        std::shared_ptr<huxerui::Clipboard> clipboard,
                        huxerui::ToastHandle toast, huxerui::Color textColor,
                        huxerui::Color hintColor) {
#if defined(__ANDROID__)
    (void)dialog;
    (void)clipboard;
    (void)textColor;
    (void)hintColor;
    toast.Show("Android VPN/TUN 尚未接入");
    return;
#else
    namespace fs = std::filesystem;
    const std::string exe = [] {
        const fs::path dir = cfg::executableDir();
        if (dir.empty()) return std::string{"clash-flux"};
        return (dir / "clash-flux").string();
    }();
    const std::string serviceCmd =
        std::format("pkexec {} service install", exe);
    dialog.Show(
        [clipboard, toast, textColor, hintColor,
         serviceCmd](huxerui::DialogContext ctx) -> huxerui::View {
            return DialogCard(huxerui::Column {
                huxerui::Text("TUN 需要安装服务模式", huxerui::TextRole::Title),
                huxerui::Text("TUN 由内核创建虚拟网卡，需要 root 权限。应用本身"
                              "保持非 root 运行（更安全），由 root 服务托管内核。"
                              "复制指令到终端执行（pkexec 会弹出授权）后重试：")
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kCaption), hintColor}),
                huxerui::Text("安装 root 服务（内核由服务托管，TUN 开箱可用）")
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kBody), textColor}),
                huxerui::Row {
                    huxerui::TextField(
                        huxerui::TextEditingValue{serviceCmd})
                        .Variant(huxerui::TextFieldVariant::Outlined)
                        .With(huxerui::Grow(1.0F)),
                    huxerui::Button("复制").OnClick([clipboard, toast,
                                                     serviceCmd] {
                        if (clipboard->WriteText(serviceCmd)) {
                            toast.Show("已复制到剪贴板");
                        } else {
                            toast.Show("复制失败");
                        }
                    }),
                }
                    .With(huxerui::Spacing(8.0F),
                          huxerui::CrossAlign(
                              huxerui::CrossAxisAlignment::Center)),
                huxerui::Row {
                    huxerui::Button("关闭").OnClick([ctx] { ctx.Dismiss(); }),
                }.With(huxerui::MainAlign(
                    huxerui::MainAxisAlignment::End)),
            }
                              .With(huxerui::Spacing(12.0F),
                                    huxerui::Frame{.width = 460.0F},
                                    huxerui::CrossAlign(
                                        huxerui::CrossAxisAlignment::Stretch)));
        },
        huxerui::DialogOptions{});
#endif
}

IslandTheme ResolveIslandTheme(const huxerui::ThemeSpec& theme) {
    return IslandTheme{
        .page_gap = theme.spacing.small,
        .island_padding = theme.spacing.medium,
        .island_radius = 16.0F,
        .nested_radius = 8.0F,
        .ocean = theme.colors.background,
        .base = theme.colors.surface_container_low,
        .raised = theme.colors.surface_container,
        .active = theme.colors.surface_container_high,
        .overlay = theme.colors.surface_container_highest,
        .outline_soft = theme.colors.outline,
    };
}

float ConcentricRadius(float outer_radius, float inset) {
    // 内层半径 = 外层半径 − inset，保下限 4pt：更小半径与父级圆角几乎相切，
    // 视觉上出现反同心（子角比父角“尖”）。
    constexpr float kMinRadius = 4.0F;
    return std::max(outer_radius - inset, kMinRadius);
}

namespace {

huxerui::Color IslandColor(const IslandTheme& islands, const huxerui::ThemeSpec& theme,
                           IslandLevel level) {
    switch (level) {
        case IslandLevel::Base: return islands.base;
        case IslandLevel::Raised: return islands.raised;
        case IslandLevel::Active: return islands.active;
        case IslandLevel::Overlay: return islands.overlay;
        case IslandLevel::Danger: {
            huxerui::Color danger = theme.colors.error;
            danger.alpha = 0.10F;
            return danger;
        }
    }
    return islands.base;
}

} // namespace

[[huxerui::composable]] huxerui::View IslandSurface(huxerui::View content,
                                                    IslandLevel level) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    // composable 形参被 codegen 固定为 const：拷贝到局部再走右值 With 链。
    huxerui::View surface = content;
    return std::move(surface).With(huxerui::Background(IslandColor(islands, theme, level)),
                                   huxerui::CornerRadius(islands.island_radius),
                                   huxerui::Padding(islands.island_padding));
}

[[huxerui::composable]] huxerui::View IslandSection(std::string title,
                                                    huxerui::View content) {
    return IslandSurface(
        huxerui::Column {
            huxerui::Text(std::move(title), huxerui::TextRole::Title),
            std::move(content),
        }.With(huxerui::Spacing(12.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
        IslandLevel::Base);
}

[[huxerui::composable]] huxerui::View PageScaffold(const std::string& title,
                                                   huxerui::View actions,
                                                   huxerui::View content) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    // 响应式：Compact(<600) 收窄一级岛内边距。
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    // 一级岛：页面根本身是岛（Grow + Stretch 占满页面区块，圆角 16pt，
    // base 表面），内容在岛内部滚动；海面底色经岛间缝隙透出。
    // composable 形参被 codegen 固定为 const：拷贝到局部再走右值链。
    // 窄屏时把标题和操作区改为上下布局，避免 Select/按钮挤出岛屿。
    huxerui::View header;
    if (compact) {
        header = huxerui::Column {
            huxerui::Text(title, huxerui::TextRole::Title),
            std::move(actions),
        }.With(huxerui::Spacing(theme.spacing.small),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    } else {
        header = huxerui::Row {
            huxerui::Text(title, huxerui::TextRole::Title),
            huxerui::Spacer(),
            std::move(actions),
        }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    }
    huxerui::View body = content;
    return huxerui::Column {
        std::move(header),
        std::move(body).With(huxerui::Grow(1.0F)),
    }.With(huxerui::Padding(compact ? theme.spacing.medium
                                    : theme.spacing.large),
           huxerui::Spacing(theme.spacing.medium),
           huxerui::Background(islands.base),
           huxerui::CornerRadius(islands.island_radius),
           huxerui::ClipChildren(),
           huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View Card(huxerui::View content) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    // 二级岛：raised 表面（比一级岛高一层级）+ 8pt 同心圆角。
    return huxerui::Column { std::move(content) }
        .With(huxerui::Padding(islands.island_padding),
              huxerui::Background(islands.raised),
              huxerui::CornerRadius(islands.nested_radius),
              huxerui::ClipChildren(),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View DialogCard(huxerui::View content) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    huxerui::View card = content;
    return std::move(card).With(
        huxerui::Shadow{huxerui::Color::Rgb(0, 0, 0, 0.24F), {}, 24.0F, 0.0F},
        huxerui::Background(islands.overlay),
        huxerui::CornerRadius(islands.island_radius),
        huxerui::Border(islands.outline_soft, 1.0F),
        huxerui::ClipChildren(),
        huxerui::Padding(islands.island_padding));
}

[[huxerui::composable]] huxerui::View CoreStatusPill() {    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto snap = huxerui::UseState<store::CoreSnapshot>({});

    huxerui::Lifecycle(
        [tasks, snap] {
            tasks.Launch([snap]() -> huxerui::Task<void> {
                co_await PollWhile(std::chrono::duration<double>{0.5}, [snap] {
                    snap = store::coreStore().snapshot();
                    return true;
                });
            });
            return [] {};
        },
        0);

    const store::CoreSnapshot s = snap.Get();
    huxerui::Color dot = theme.colors.on_surface_variant;
    std::string label = "已停止";
    if (s.binaryPath.empty()) {
        label = "内核未安装";
    } else if (s.state == core::CoreState::Running) {
        dot = huxerui::Color::Rgb(34, 197, 94);   // 绿
        label = s.version.empty() ? "运行中" : s.version;
    } else if (s.state == core::CoreState::Starting) {
        dot = huxerui::Color::Rgb(245, 158, 11);  // 琥珀
        label = "启动中";
    } else if (s.state == core::CoreState::Failed) {
        dot = theme.colors.error;
        label = "内核异常";
    }

    return huxerui::Row {
        huxerui::Text("●").Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kCaption), dot}),
        huxerui::Text(label).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kCaption),
            theme.colors.on_surface_variant}),
    }.With(huxerui::Spacing(6.0F),
           huxerui::Padding(huxerui::EdgeInsets::Symmetric(8.0F, 3.0F)),
           huxerui::Background(ResolveIslandTheme(theme).overlay),
           huxerui::CornerRadius(ResolveIslandTheme(theme).nested_radius),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
           huxerui::Frame{.height = kTitleBarContentHeight},
           huxerui::Tooltip("mihomo 内核状态"));
}

} // namespace clashflux::ui
