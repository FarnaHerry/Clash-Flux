// common.cpp — 岛屿原语（ResolveIslandTheme/IslandSurface/IslandSection）、
// 页面骨架（一级岛）/ 卡片（二级岛）等跨页通用部件。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "app_resources.h"
#include "ui.h"

import nlohmann.json;
import clashflux.config;
import clashflux.core;
import clashflux.store.core;
import clashflux.store.profiles;

namespace clashflux::ui {

namespace {

bool isSelectorType(const std::string& type) {
    return type == "Selector" || type == "selector";
}

} // namespace

std::vector<ProxyGroupSnapshot> ParseProxyGroups(const std::string& body) {
    std::vector<ProxyGroupSnapshot> groups;
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (!json.is_object() || !json.contains("proxies") ||
        !json["proxies"].is_object()) {
        return groups;
    }

    const auto& proxies = json["proxies"];
    for (auto it = proxies.begin(); it != proxies.end(); ++it) {
        const auto& value = it.value();
        if (!value.is_object() || !value.contains("all") ||
            !value["all"].is_array()) {
            continue;
        }
        ProxyGroupSnapshot group;
        group.name = it.key();
        group.type = value.value("type", "");
        group.current = value.value("now", "");
        group.selectable = value.value("selectable", isSelectorType(group.type));
        for (const auto& node : value["all"]) {
            if (!node.is_string()) continue;
            const std::string name = node.get<std::string>();
            group.nodes.push_back(name);
            if (proxies.contains(name) && proxies[name].is_object()) {
                const auto& info = proxies[name];
                if (info.contains("history") && info["history"].is_array() &&
                    !info["history"].empty()) {
                    const auto& last = info["history"].back();
                    group.delays[name] = last.is_object() ? last.value("delay", 0) : 0;
                } else {
                    group.delays[name] = 0;
                }
            }
        }
        groups.push_back(std::move(group));
    }
    return groups;
}

std::vector<huxerui::MenuEntry> BuildProxyLineMenu(
    const std::vector<ProxyGroupSnapshot>& groups,
    std::function<void(const std::string&, const std::string&)> on_select) {
    std::vector<huxerui::MenuEntry> entries;
    for (const ProxyGroupSnapshot& group : groups) {
        if (!group.selectable) {
            entries.push_back(huxerui::MenuItem(
                group.name + "（自动测速，不支持手动切换）", [] {}).Enabled(false));
            continue;
        }
        std::vector<huxerui::MenuEntry> nodes;
        for (const std::string& node : group.nodes) {
            nodes.push_back(huxerui::MenuItem(
                                node,
                                [on_select, groupName = group.name, node] {
                                    on_select(groupName, node);
                                })
                                .Checked(node == group.current));
        }
        if (nodes.empty()) {
            nodes.push_back(
                huxerui::MenuItem("暂无可切换线路", [] {}).Enabled(false));
        }
        entries.push_back(
            huxerui::MenuItem(group.name, std::move(nodes)));
    }
    if (entries.empty()) {
        entries.push_back(
            huxerui::MenuItem("暂无可切换线路", [] {}).Enabled(false));
    }
    return entries;
}

std::string ProxyGroupsSnapshot() {
#if defined(__ANDROID__)
    const char* body = clashflux_android_proxy_groups();
    // Before the VPN service is started there is no libbox CommandClient, so
    // Java returns an empty group object.  Keep the page usable by falling
    // back to the native store's stopped-core subscription preview.
    if (body != nullptr && *body != '\0' &&
        std::string_view(body) != "{\"proxies\":{}}") {
        return body;
    }
    return store::coreStore().proxyGroupsSnapshot();
#else
    const auto result = store::coreStore().api().proxies();
    return result.ok ? result.body : store::coreStore().proxyGroupsSnapshot();
#endif
}

bool SelectProxyLine(const std::string& group, const std::string& name) {
#if defined(__ANDROID__)
    return store::coreStore().selectProxy(group, name);
#else
    return store::coreStore().selectProxy(group, name);
#endif
}

bool StartProxyGroupTest(const std::string& group) {
#if defined(__ANDROID__)
    try {
        // Android 的测速内核使用无 TUN 的 libbox 配置；如果当前没有已连接
        // 的 VPN 服务，先只编译当前配置，再由 Java 启动 speed-test-only
        // 服务。这样测速不会申请 VPN，也不会接管应用流量。
        if (clashflux_android_vpn_state() != 2) {
            auto& core = store::coreStore();
            core.startCore(store::profilesStore().selectedYaml(), false, false,
                           std::nullopt, true);
            if (core.snapshot().state == core::CoreState::Failed) return false;
        }
        return clashflux_android_url_test(group.c_str());
    } catch (...) {
        return false;
    }
#else
    // 桌面节点测速通过内核 delay API 完成。测速依赖内核但不需要接管流量，
    // 内核没跑时不再隐式拉起（内置启动入口只在首页右下角悬浮按钮与设置里的
    // 「启动时自动运行内核」），这里直接返回失败由页面提示先启动内核。
    if (store::coreStore().snapshot().state != core::CoreState::Running) {
        return false;
    }
    return TriggerProxyGroupTest(group);
#endif
}

void WaitForAndroidVpnStopped() noexcept {
#if defined(__ANDROID__)
    for (int attempt = 0; attempt < 60; ++attempt) {
        const int state = clashflux_android_vpn_state();
        if (state != 1 && state != 2) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#endif
}

[[huxerui::composable]] huxerui::View SettingRow(const std::string& label,
                                                 const std::string& hint,
                                                 huxerui::View control) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    huxerui::View description = huxerui::Column {
        huxerui::Text(label).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kBody), theme.colors.on_surface}),
        hint.empty()
            ? huxerui::View{huxerui::Row{}}
            : huxerui::View{huxerui::Text(hint).Style(huxerui::TextStyle{
                  huxerui::Font::System(font_size::kCaption),
                  theme.colors.on_surface_variant})},
    }.With(huxerui::Spacing(2.0F));

    if (compact) {
        return huxerui::Column {
            std::move(description),
            std::move(control),
        }.With(huxerui::Spacing(8.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }

    return huxerui::Row {
        std::move(description),
        huxerui::Spacer(),
        std::move(control),
    }.With(huxerui::Spacing(12.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

[[huxerui::composable]] huxerui::View SettingSwitchRow(
    const std::string& label, const std::string& hint, huxerui::View control,
    bool danger) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    huxerui::View text = huxerui::Column {
        huxerui::Text(label).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kBody),
            danger ? theme.colors.error : theme.colors.on_surface}),
        hint.empty()
            ? huxerui::View{huxerui::Row{}}
            : huxerui::View{huxerui::Text(hint).Style(huxerui::TextStyle{
                  huxerui::Font::System(font_size::kCaption),
                  theme.colors.on_surface_variant})},
    }.With(huxerui::Spacing(2.0F), huxerui::Grow(1.0F));
    return huxerui::Row {
        std::move(text),
        control,
    }.With(huxerui::Spacing(12.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

[[huxerui::composable]] huxerui::View UnifiedListRow(
    huxerui::View content, std::string key, bool compact, bool divider) {
    const float horizontal = compact ? 10.0F : 8.0F;
    const float vertical = compact ? 8.0F : 6.0F;
    // 平铺列表行：不逐行套圆角卡壳，行直接落在一级岛表面上，行间用细分隔
    // 线划界；divider=false 用于最后一行，避免列表尾部悬一条分隔线。
    // composable 形参被 codegen 固定为 const：拷贝到局部再走右值 With 链。
    huxerui::View row = content;
    return huxerui::Column {
        std::move(row)
            .With(huxerui::Spacing(compact ? 5.0F : 8.0F),
                  huxerui::Padding(huxerui::EdgeInsets::Symmetric(horizontal,
                                                                   vertical)),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
        divider ? huxerui::View{huxerui::Divider()}
                : huxerui::View{huxerui::Row{}},
    }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
        .Key(std::move(key));
}

[[huxerui::composable]] huxerui::View SectionTitle(const std::string& title) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Text(title).Style(huxerui::TextStyle{
        huxerui::Font::System(font_size::kChip).WithWeight(huxerui::FontWeight::Bold),
        theme.colors.primary});
}

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
    toast.Show("请在设置页的“VPN 代理”中管理 Android VPN 隧道");
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
        .page_gap = 10.0F,
        .island_padding = theme.spacing.medium,
        .island_radius = 22.0F,
        .nested_radius = 14.0F,
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
                                                   huxerui::View content,
                                                   bool inlineCompactActions) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    // 响应式：Compact(<600) 收窄一级岛内边距。
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    // 一级岛（仅桌面）：页面根本身是岛（Grow + Stretch 占满页面区块，圆角
    // 16pt，base 表面），内容在岛内部滚动；海面底色经岛间缝隙透出。
    // 移动端不再把页面套成外部卡片：去掉表面与圆角，内容直接落在窗口海面
    // 底色上（内部卡片/分组仍按各自层级表达）。
    // composable 形参被 codegen 固定为 const：拷贝到局部再走右值链。
    // 窄屏时把标题和操作区改为上下布局，避免 Select/按钮挤出页面。
    huxerui::View header;
    if (compact && !inlineCompactActions) {
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
           huxerui::Background(compact ? huxerui::Color::Transparent()
                                       : islands.base),
           huxerui::CornerRadius(compact ? 0.0F : islands.island_radius),
           huxerui::ClipChildren(),
           huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View SecondaryPageScaffold(
    huxerui::View title, huxerui::View actions, huxerui::View content,
    std::function<void()> onBack, bool hideBack) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    huxerui::View body = content;
    huxerui::View titleView = title;
    auto backAction = onBack;
    huxerui::View header = hideBack
        ? huxerui::Row {
              std::move(titleView).With(huxerui::Grow(1.0F)),
              std::move(actions),
          }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
        : huxerui::Row {
              huxerui::IconButton(app::images::arrow_back, "返回上一页")
                  .With(huxerui::Tooltip("返回上一页"))
                  .OnClick(std::move(onBack)),
              std::move(titleView).With(huxerui::Grow(1.0F)),
              std::move(actions),
          }.With(huxerui::Spacing(theme.spacing.small),
                 huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    // 与 PageScaffold 同规则：移动端二级页也直接落在海面底色上，不套外卡。
    huxerui::View scaffold = huxerui::Column {
        std::move(header),
        std::move(body).With(huxerui::Grow(1.0F)),
    }.With(huxerui::Padding(theme.spacing.medium), huxerui::Spacing(theme.spacing.medium),
           huxerui::Background(compact ? huxerui::Color::Transparent()
                                       : islands.base),
           huxerui::CornerRadius(compact ? 0.0F : islands.island_radius),
           huxerui::ClipChildren(), huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    if (backAction && !hideBack) {
        scaffold = std::move(scaffold).On<huxerui::ViewEvents::BackRequested>(
            std::move(backAction));
    }
    return scaffold;
}

[[huxerui::composable]] huxerui::View PillSearchField(
    huxerui::State<huxerui::TextEditingValue> value,
    const std::string& placeholder,
    std::function<void()> onClose) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    huxerui::TextFieldStyle inputStyle = huxerui::UseEnvironment<huxerui::TextFieldStyle>();
    inputStyle.border_width = 0.0F;
    inputStyle.focused_border_width = 0.0F;
    inputStyle.validation_border_width = 0.0F;
    inputStyle.focused_validation_border_width = 0.0F;
    inputStyle.standard.background = huxerui::Color::Transparent();
    inputStyle.standard.border = huxerui::Color::Transparent();
    inputStyle.standard.hovered_border = huxerui::Color::Transparent();
    inputStyle.standard.focused_border = huxerui::Color::Transparent();
    inputStyle.standard.disabled_border = huxerui::Color::Transparent();
    inputStyle.standard.minimum_height = 48.0F;
    inputStyle.padding = huxerui::EdgeInsets::Symmetric(0.0F, 0.0F);
    huxerui::ThemeDefinition inputTheme;
    inputTheme.Set(inputStyle);
    huxerui::View input = huxerui::Theme(
        std::move(inputTheme),
        huxerui::TextField(value.Get())
            .Placeholder(placeholder)
            .Variant(huxerui::TextFieldVariant::Standard)
            .OnChanged([value](const huxerui::TextEditingValue& next) {
                value = next;
            })
            .With(huxerui::Grow(1.0F)));

    // 胶囊体内容：搜索图标、无框输入文本框、退出搜索按钮。
    auto closeAction = onClose;
    huxerui::View pillContent = huxerui::Row {
        huxerui::Image(app::images::search)
            .Fit(huxerui::ImageFit::Contain)
            .Align(huxerui::HorizontalAlignment::Center,
                   huxerui::VerticalAlignment::Center)
            .Tint(theme.colors.on_surface_variant)
            .With(huxerui::Frame{.width = 20.0F, .height = 20.0F}),
        std::move(input),
        huxerui::IconButton(app::images::close, "退出搜索")
            .With(huxerui::Tooltip("退出搜索"))
            .OnClick(std::move(onClose)),
    }.With(huxerui::Spacing(6.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
           huxerui::Grow(1.0F));

    // 使用 Row 做背景容器，高度与标签栏一致（48dp），圆角 24dp，无边框线。
    huxerui::View pill = huxerui::Row {
        std::move(pillContent),
    }.With(huxerui::Frame{.height = 48.0F},
           huxerui::Padding(huxerui::EdgeInsets{
               .right = 4.0F,
               .left = 14.0F,
           }),
           huxerui::Background(theme.colors.surface_container_highest),
           huxerui::CornerRadius(24.0F),
           huxerui::ClipChildren(),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
           huxerui::Grow(1.0F));

    // 吸收系统返回事件：收到系统返回指令相当于触发退出搜索（叉号）。
    if (closeAction) {
        pill = std::move(pill).On<huxerui::ViewEvents::BackRequested>(
            std::move(closeAction));
    }
    return pill;
}

[[huxerui::composable]] huxerui::View Card(huxerui::View content,
                                           bool outlined) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    // 二级岛：半透明水晶卡片感的 raised 表面 + 同心圆角。移动端设置页等
    // 场景不需要描边，由同层级的 raised 表面与页面底色自然区分。
    return huxerui::Column { std::move(content) }
        .With(huxerui::Padding(islands.island_padding),
              huxerui::Background(islands.raised),
              huxerui::CornerRadius(islands.nested_radius),
              huxerui::Border(outlined ? islands.outline_soft
                                       : huxerui::Color::Transparent(),
                              outlined ? 1.0F : 0.0F),
              huxerui::ClipChildren(),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View DialogCard(huxerui::View content) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    huxerui::View card = content;
    return std::move(card).With(
        huxerui::Shadow{huxerui::Color::Rgb(0, 0, 0, 0.30F), {}, 28.0F, 2.0F},
        huxerui::Background(islands.overlay),
        huxerui::CornerRadius(islands.island_radius),
        huxerui::Border(islands.outline_soft, 1.0F),
        huxerui::ClipChildren(),
        huxerui::Padding(islands.island_padding));
}

} // namespace clashflux::ui
