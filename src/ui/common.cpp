// common.cpp — 岛屿原语（ResolveIslandTheme/IslandSurface/IslandSection）、
// 页面骨架（一级岛）/ 卡片（二级岛）等跨页通用部件。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "app_resources.h"
#include "proxies_model.h"
#include "task_bridge.h"
#include "ui.h"
#include "page_layout.h"
#include "app.h"

#include "wire_codec.h"
import clashflux.config;
import clashflux.core;
import clashflux.db;
import clashflux.persistence;
import clashflux.service;
import clashflux.stream;
import clashflux.store.core;
import clashflux.store.profiles;
import clashflux.store.vpn;

// CoreView 含 store::CoreSnapshot、ProfilesModel 含 db::Profile，必须放在模块
// 导入之后（同 profiles_cache.h）。
#include "core_model.h"
#include "profiles_model.h"
#include "settings_model.h"
#include "stream_updates.h"
#include "vpn_model.h"

namespace clashflux::ui {

namespace {

bool isSelectorType(const std::string& type) {
    return type == "Selector" || type == "selector";
}

} // namespace

DesktopModeApplyResult ApplyDesktopSystemProxy(bool enabled) {
    auto& coreStore = store::coreStore();
    if (coreStore.applySystemProxy(enabled)) {
        return {.status = DesktopModeApplyStatus::Applied};
    }
    return {.status = DesktopModeApplyStatus::Failed,
            .error = coreStore.snapshot().lastError};
}

DesktopModeApplyResult ApplyDesktopTun(bool enabled) {
    if (enabled) {
        switch (core::tunGate()) {
            case core::TunGate::Ok:
                break;
            case core::TunGate::Elevated:
                return {.status = DesktopModeApplyStatus::ElevationRequested};
            case core::TunGate::Denied:
                return {.status = DesktopModeApplyStatus::PermissionDenied};
        }
    }

    auto& coreStore = store::coreStore();
    if (coreStore.applyTun(enabled)) {
        return {.status = DesktopModeApplyStatus::Applied};
    }
    return {.status = DesktopModeApplyStatus::Failed,
            .error = coreStore.snapshot().lastError};
}

namespace {

// 只服务于 FetchProxiesSnapshot：拥有型策略组 DTO → 扁平快照。不再是公开 API——
// 消费方一律读 ProxiesModel，避免又出现第二个「各自解析」的入口。
std::vector<ProxyGroupSnapshot> ParseProxyGroups(const wire::Proxies& snapshot, const std::map<std::string, std::string>& labels) {
    std::vector<ProxyGroupSnapshot> groups;
    const auto& proxies = snapshot.entries;
    for (const auto& [name, value] : proxies) {
        if (!value.members) continue;
        ProxyGroupSnapshot group;
        group.name = name;
        group.displayName = labels.contains(name) ? labels.at(name) : name;

        group.type = value.type;
        group.current = value.now;
        group.selectable = value.selectable.value_or(isSelectorType(group.type));
        group.nodes = *value.members;
        for (const auto& node : group.nodes) {
            if (labels.contains(node)) group.nodeLabels[node] = labels.at(node);
            if (const auto found = proxies.find(node); found != proxies.end()) {
                group.delays[node] = found->second.historyDelay;
            }
        }
        groups.push_back(std::move(group));
    }
    return groups;
}

} // namespace

std::vector<huxerui::MenuEntry> BuildProxyLineMenu(
    const std::vector<ProxyGroupSnapshot>& groups,
    std::function<void(const std::string&, const std::string&)> on_select,
    const std::function<std::string(huxerui::StringVariant)>& resolveLabel) {
    std::vector<huxerui::MenuEntry> entries;
    for (const ProxyGroupSnapshot& group : groups) {
        if (!group.selectable) {
            entries.push_back(huxerui::MenuItem(
                resolveLabel(LocalizedFormat("{}（自动测速，不支持手动切换）", group.displayName)),
                [] {}).Enabled(false));
            continue;
        }
        std::vector<huxerui::MenuEntry> nodes;
        for (const std::string& node : group.nodes) {
            nodes.push_back(huxerui::MenuItem(
                                group.nodeLabels.contains(node) ? group.nodeLabels.at(node) : node,
                                [on_select, groupName = group.name, node] {
                                    on_select(groupName, node);
                                })
                                .Checked(node == group.current));
        }
        if (nodes.empty()) {
            nodes.push_back(
                huxerui::MenuItem(resolveLabel(Localized("暂无可切换线路")), [] {}).Enabled(false));
        }
        entries.push_back(
            huxerui::MenuItem(group.displayName, std::move(nodes)));
    }
    if (entries.empty()) {
        entries.push_back(
            huxerui::MenuItem(resolveLabel(Localized("暂无可切换线路")), [] {}).Enabled(false));
    }
    return entries;
}

// ---- 策略组快照：唯一来源（见 proxies_model.h）------------------------------

ProxiesSnapshot FetchProxiesSnapshot() {
    ProxiesSnapshot next;
    std::string body;
#if defined(__ANDROID__)
    const char* raw = clashflux_android_proxy_groups();
    // VPN 服务未启动时 Java 侧没有 libbox CommandClient，会返回空组对象；
    // 此时回落到原生 store 的「停止内核」订阅预览，并标明这是预览而非实时。
    if (raw != nullptr && *raw != '\0' &&
        std::string_view(raw) != "{\"proxies\":{}}") {
        body = raw;
        next.source = ProxiesSource::Live;
    } else {
        body = store::coreStore().proxyGroupsSnapshot();
        next.source = body.empty() ? ProxiesSource::Empty : ProxiesSource::Preview;
    }
#else
    const auto result = store::coreStore().api().proxies();
    if (result.ok) {
        body = result.body;
        next.source = ProxiesSource::Live;
    } else {
        // 回落是刻意的（停内核时仍要让用户能预先选节点），但把原因记下来，
        // 页面可以据此提示「当前是预览」，而不是把预览当实时数据展示。
        body = store::coreStore().proxyGroupsSnapshot();
        next.source = body.empty() ? ProxiesSource::Empty : ProxiesSource::Preview;
        next.error = result.error;
    }
#endif
    next.body = std::move(body);
    if (!next.body.empty()) {
        auto decoded = wire::DecodeProxies(next.body);
        if (decoded) {
            next.proxies = std::make_shared<const wire::Proxies>(std::move(decoded.value));
            const auto catalog = store::coreStore().proxySourceObjects(next.source == ProxiesSource::Preview);
            if (catalog) for (const auto& object : *catalog) {
                next.labels[object.tag] = object.sourceName.empty() ? object.objectId
                    : object.objectId + " · " + object.sourceName;
            }
            next.groups = ParseProxyGroups(*next.proxies, next.labels);
        } else {
            next.error = "代理快照解析失败：" + decoded.error;
            next.source = ProxiesSource::Empty;
        }
    }
    return next;
}

void InstallClashFluxUiModels(huxerui::ApplicationContext& context) {
    context.Provide(std::make_shared<ProxiesModel>());
    context.Provide(std::make_shared<CoreModel>());
    auto profiles = std::make_shared<ProfilesModel>();
    context.Provide(profiles);
    InstallProfileLinkActivation(context, profiles);
    context.Provide(std::make_shared<VpnModel>());
    context.Provide(std::make_shared<SettingsModel>());
}

void DriveProxiesModel(huxerui::TaskScope tasks,
                       std::shared_ptr<ProxiesModel> model) {
    tasks.Launch([model]() -> huxerui::Task<void> {
        for (;;) {
            const std::uint64_t before = model->refreshTick.Get();
            ProxiesSnapshot next = co_await RunOnTaskThread(FetchProxiesSnapshot);
            // 取数据在 worker，写 State 恒在 UI 线程；内容相等时 State::Write
            // 去重，不会引起订阅方重组。
            model->snapshot = std::move(next);
            if (model->refreshTick.Get() != before) continue;
            co_await huxerui::Delay(std::chrono::duration<double>{2.0});
        }
    });
}

// ---- 内核/接管状态：唯一来源（见 core_model.h）------------------------------

CoreView ReadCoreView(bool slowProbes, bool previousSystemProxyActive) {
    auto& core = store::coreStore();
    // 内核崩溃检测：Running 但进程/控制器已不在 → Failed。以前由托盘的 0.5s
    // 泵顺带做，现在集中在这里每秒做一次。
    core.checkAlive();
    CoreView view;
    view.core = core.snapshot();
    view.systemProxyIntent = core.systemProxyEnabled();
    // 系统代理实际指向在 GNOME 下是 3 次 popen：只在慢探测拍或动作请求时查，
    // 其余时候沿用上一次的结果（State 只能在 UI 线程读，故由调用方按值传入）。
    view.systemProxyActive = slowProbes ? core.systemProxyActive()
                                        : previousSystemProxyActive;
    view.ipv6Enabled = core.setting("core.ipv6_enabled", "false") == "true";
    // 局域网开关显示的是「意图」（KV），不是内核运行时的 allow-lan 回读值：
    // 内核没跑时运行时快照不含本次意图，写透后会被这一拍打回去。
    view.allowLan = core.setting("core.allow_lan", "false") == "true";
    view.serviceInstalled = service::installed();
    return view;
}

void DriveCoreModel(huxerui::TaskScope tasks, std::shared_ptr<CoreModel> model) {
    tasks.Launch([model]() -> huxerui::Task<void> {
        // 昂贵探测（系统代理实际指向）每 kSlowProbeInterval 拍做一次；显式的
        // RequestRefresh 也会立刻做一次（动作完成后要让权威值尽快跟上）。
        constexpr int kSlowProbeInterval = 15;
        int slowCountdown = 0;
        for (;;) {
            const std::uint64_t before = model->refreshTick.Get();
            const bool slowProbes = slowCountdown <= 0;
            const CoreView previous = model->view.Get();
            CoreView next = co_await RunOnTaskThread(
                [slowProbes, active = previous.systemProxyActive] {
                    return ReadCoreView(slowProbes, active);
                });
            model->view = std::move(next);
            slowCountdown = slowProbes ? kSlowProbeInterval : slowCountdown - 1;
            if (model->refreshTick.Get() != before) continue;
            co_await huxerui::Delay(std::chrono::duration<double>{1.0});
        }
    });
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

[[huxerui::composable]] huxerui::View SettingItemIcon(huxerui::ImageResource icon, bool danger) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Image(icon)
        .Fit(huxerui::ImageFit::Contain)
        .Tint(danger ? theme.colors.error : theme.colors.on_surface_variant)
        .With(huxerui::Frame{.width = 20.0F, .height = 20.0F});
}

[[huxerui::composable]] huxerui::View SettingRow(huxerui::StringVariant label,
                                                 huxerui::StringVariant hint,
                                                 huxerui::View control,
                                                 std::optional<huxerui::ImageResource> icon) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    huxerui::View description = huxerui::Column {
        huxerui::Text(label).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kBody), theme.colors.on_surface}),
        huxerui::UseString(hint).empty()
            ? huxerui::View{huxerui::Row{}}
            : huxerui::View{huxerui::Text(hint).Style(huxerui::TextStyle{
                  huxerui::Font::System(font_size::kCaption),
                  theme.colors.on_surface_variant})},
    }.With(huxerui::Spacing(2.0F));
    if (icon) {
        description = huxerui::Row {
          SettingItemIcon(*icon),
          std::move(description).With(huxerui::Grow(1.0F)),
        }.With(huxerui::Spacing(12.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    }

    if (compact) {
        huxerui::View compactControl = control;
        if (icon) compactControl = std::move(compactControl).With(huxerui::Padding(huxerui::EdgeInsets{.left = 32.0F}));
        return huxerui::Column {
          description,
          compactControl,
        }.With(huxerui::Spacing(8.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }

    return huxerui::Row {
      std::move(description).With(huxerui::Grow(1.0F)),
      control,
    }.With(huxerui::Spacing(12.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

[[huxerui::composable]] huxerui::View SettingSwitchRow(
    huxerui::StringVariant label, huxerui::StringVariant hint, huxerui::View control,
    bool danger, std::optional<huxerui::ImageResource> icon) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    huxerui::View text = huxerui::Column {
        huxerui::Text(label).Style(huxerui::TextStyle{
            huxerui::Font::System(font_size::kBody),
            danger ? theme.colors.error : theme.colors.on_surface}),
        huxerui::UseString(hint).empty()
            ? huxerui::View{huxerui::Row{}}
            : huxerui::View{huxerui::Text(hint).Style(huxerui::TextStyle{
                  huxerui::Font::System(font_size::kCaption),
                  theme.colors.on_surface_variant})},
    }.With(huxerui::Spacing(2.0F), huxerui::Grow(1.0F));
    if (icon) {
        return huxerui::Row {
          SettingItemIcon(*icon, danger),
          text,
          control,
        }.With(huxerui::Spacing(12.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    }
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

[[huxerui::composable]] huxerui::View SectionTitle(huxerui::StringVariant title) {
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
        .Label(Localized("密码"))
        .Secure(!isVisible)
        .TrailingIcon(isVisible ? app::images::visibility_off
                                : app::images::visibility,
                      Localized(isVisible ? "隐藏密码" : "显示密码"))
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
    toast.Show(Localized("请在设置页的“VPN 代理”中管理 Android VPN 隧道"));
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
                huxerui::Text(Localized("TUN 需要安装服务模式"), huxerui::TextRole::Title),
                huxerui::Text(Localized(
                    "TUN 由内核创建虚拟网卡，需要 root 权限。应用本身"
                    "保持非 root 运行（更安全），由 root 服务托管内核。"
                    "复制指令到终端执行（pkexec 会弹出授权）后重试："))
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kCaption), hintColor}),
                huxerui::Text(Localized("安装 root 服务（内核由服务托管，TUN 开箱可用）"))
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kBody), textColor}),
                huxerui::Row {
                    huxerui::TextField(
                        huxerui::TextEditingValue{serviceCmd})
                        .Variant(huxerui::TextFieldVariant::Outlined)
                        .With(huxerui::Grow(1.0F)),
                    huxerui::Button(Localized("复制")).OnClick([clipboard, toast,
                                                     serviceCmd] {
                        if (clipboard->WriteText(serviceCmd)) {
                            toast.Show(Localized("已复制到剪贴板"));
                        } else {
                            toast.Show(Localized("复制失败"));
                        }
                    }),
                }
                    .With(huxerui::Spacing(8.0F),
                          huxerui::CrossAlign(
                              huxerui::CrossAxisAlignment::Center)),
                huxerui::Row {
                    huxerui::Button(Localized("关闭")).OnClick([ctx] { ctx.Dismiss(); }),
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

[[huxerui::composable]] huxerui::View WithoutIconButtonOutlines(
    huxerui::View content) {
    // The built-in icon-button focus ring and indication borders read these
    // scoped values; keep the rest of the theme and interaction feedback intact.
    huxerui::ThemeSpec theme = huxerui::UseTheme();
    theme.interactions.focus_ring =
        huxerui::FocusRing{huxerui::Color::Transparent(), 0.0F, 0.0F};
    const auto removeIndicationBorders = [](huxerui::Indication& indication) {
        const auto removeBorder = [](auto& layer) {
            if (layer) layer->border.reset();
        };
        removeBorder(indication.focus);
        removeBorder(indication.hover);
        removeBorder(indication.press);
    };
    removeIndicationBorders(theme.interactions.indication);

    huxerui::IconButtonStyle iconButtonStyle =
        huxerui::UseEnvironment<huxerui::IconButtonStyle>();
    if (iconButtonStyle.indication) {
        removeIndicationBorders(*iconButtonStyle.indication);
    }

    huxerui::ThemeDefinition overrides;
    overrides.Set(theme);
    overrides.Set(iconButtonStyle);
    return huxerui::Theme(overrides, content);
}

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

[[huxerui::composable]] huxerui::View PageScaffold(huxerui::StringVariant title,
                                                   huxerui::View actions,
                                                   huxerui::View content,
                                                   bool inlineCompactActions,
                                                   bool fullWidthSections,
                                                   bool windowTitle,
                                                   std::optional<float> contentSpacing,
                                                   huxerui::View leading) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    // 响应式：Compact(<600) 收窄一级岛内边距。
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    auto contentInsets = PageContentInsets(theme, compact,
        fullWidthSections ? 0.0F : contentSpacing.value_or(compact ? 4.0F : kDesktopPageHorizontalInset));
    if (contentSpacing) contentInsets.top = *contentSpacing;
    if (kPageTitlesInWindow && !compact) {
        // 桌面动作直接挂在本页标题栏中，不经共享 State 转交 View 或复制回调。
        huxerui::View desktopBody = huxerui::View{content}.With(huxerui::Grow(1.0F));
        if (fullWidthSections) desktopBody = huxerui::ProvideEnvironment(
            SectionTabContentInsets{kSectionCardSpacing}, desktopBody);
        desktopBody = huxerui::ProvideEnvironment(
            SectionTabPickerInsets{kDesktopTitleBarHeight + 1.0F}, std::move(desktopBody));
        return huxerui::Column{
            huxerui::WindowTitleBar{
                huxerui::Row{leading}.With(huxerui::Padding(huxerui::EdgeInsets{
                    .left = leading ? kDesktopPageHorizontalInset : 0.0F})),
                PageTitleText(theme, title).With(huxerui::Padding(huxerui::EdgeInsets{
                    .left = leading ? 0.0F : kDesktopPageHorizontalInset})),
                huxerui::Spacer{}.With(huxerui::Grow(1.0F)),
                huxerui::Row{actions}.With(
                    huxerui::Padding(huxerui::EdgeInsets{.right = 8.0F}),
                    huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
            }.With(huxerui::Frame{.height = kDesktopTitleBarHeight}, huxerui::Spacing(0.0F)),
            huxerui::Row{}.With(huxerui::Frame{.height = 1.0F},
                                huxerui::Background(theme.colors.outline)),
            huxerui::Column{std::move(desktopBody)}.With(
                huxerui::Padding(contentInsets),
                huxerui::Grow(1.0F),
                huxerui::Background(islands.base),
                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
        }.With(huxerui::Spacing(0.0F), huxerui::Grow(1.0F),
               huxerui::ClipChildren(),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }
    static_cast<void>(windowTitle);
    // Compact 共用页内标题与动作；桌面窗口控件由外壳独立保留。
    huxerui::View titleView = PageTitleText(theme, title);
    if (leading) titleView = huxerui::Row{leading, titleView}.With(
        huxerui::Spacing(theme.spacing.small),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    huxerui::View header = PageHeaderLayout(theme,
        titleView,
        std::move(actions), compact && !inlineCompactActions);
    const float horizontal = fullWidthSections
        ? kSectionCardSpacing
        : (compact ? 4.0F : kDesktopPageHorizontalInset);
    huxerui::View body = huxerui::View{content}.With(huxerui::Grow(1.0F));
    if (fullWidthSections) {
        if (header) header = huxerui::View{header}.With(
            huxerui::Padding(huxerui::EdgeInsets::Symmetric(
                compact ? horizontal : kDesktopPageHorizontalInset, 0.0F)));
        body = huxerui::ProvideEnvironment(SectionTabContentInsets{horizontal}, body);
    }
    std::vector<huxerui::View> children;
    if (header) children.push_back(std::move(header));
    children.push_back(std::move(body));
    return huxerui::Column(std::move(children)).With(huxerui::Padding(contentInsets),
           huxerui::Spacing(contentSpacing.value_or(theme.spacing.medium)),
           huxerui::Background(islands.base),
           huxerui::CornerRadius(kPageTitlesInWindow || compact ? 0.0F : islands.island_radius),
           huxerui::ClipChildren(),
           huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View SecondaryPageScaffold(
    huxerui::View title, huxerui::View actions, huxerui::View content,
    std::function<void()> onBack, bool hideBack, bool fullWidthSections) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    huxerui::View body = huxerui::View{content}.With(huxerui::Grow(1.0F));
    huxerui::View titleView = title;
    auto backAction = onBack;
    huxerui::View header = hideBack
        ? huxerui::Row {
              std::move(titleView).With(huxerui::Grow(1.0F)),
              std::move(actions),
          }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
        : huxerui::Row {
              huxerui::IconButton(app::images::arrow_back, Localized("返回上一页"))
                  .With(huxerui::Tooltip(Localized("返回上一页")))
                  .OnClick(std::move(onBack)),
              std::move(titleView).With(huxerui::Grow(1.0F)),
              std::move(actions),
          }.With(huxerui::Spacing(theme.spacing.small),
                 huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    header = std::move(header).With(huxerui::Frame{.min_height = kPageHeaderHeight});
    const float horizontal = fullWidthSections ? kSectionCardSpacing
        : (compact ? 4.0F : kDesktopPageHorizontalInset);
    if (fullWidthSections) {
        header = huxerui::View{header}.With(
            huxerui::Padding(huxerui::EdgeInsets::Symmetric(
                compact ? horizontal : kDesktopPageHorizontalInset, 0.0F)));
        body = huxerui::ProvideEnvironment(SectionTabContentInsets{horizontal}, body);
    }
    // 一级/二级页共用内容层底色，保持与窗口外框的颜色区分，不套外卡。
    huxerui::View scaffold = huxerui::Column {
        std::move(header),
        std::move(body),
    }.With(huxerui::Padding(PageContentInsets(theme, compact,
                   fullWidthSections ? 0.0F : horizontal)),
           huxerui::Spacing(theme.spacing.medium),
           huxerui::Background(islands.base),
           huxerui::CornerRadius(kPageTitlesInWindow || compact ? 0.0F : islands.island_radius),
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
    huxerui::StringVariant placeholder,
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
    inputStyle.standard.minimum_height = kPageTitlesInWindow ? kDesktopTitleBarHeight : 48.0F;
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
        huxerui::IconButton(app::images::close, Localized("退出搜索"))
            .With(huxerui::Tooltip(Localized("退出搜索")))
            .OnClick(std::move(onClose)),
    }.With(huxerui::Spacing(6.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
           huxerui::Grow(1.0F));

    // 桌面搜索框适配标题栏高度，手机保持 48dp 的页内搜索框。
    huxerui::View pill = huxerui::Row {
        std::move(pillContent),
    }.With(huxerui::Frame{.height = kPageTitlesInWindow ? kDesktopTitleBarHeight : 48.0F},
           huxerui::Padding(huxerui::EdgeInsets{
               .right = 4.0F,
               .left = 14.0F,
           }),
           huxerui::Background(theme.colors.surface_container_highest),
           huxerui::CornerRadius(kPageTitlesInWindow ? kDesktopTitleBarHeight / 2.0F : 24.0F),
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
    //
    // 表面/圆角/描边必须在**建树时**落在这个 Column 上：composable 返回的是 hcg
    // 生成的 Scope 包装节点，调用方在 `Card(...)` 的返回值上再补
    // `.With(Background/Border/CornerRadius)` 只会画在整棵子树背后，从圆角差里漏出
    // 一圈底色（首页「当前订阅」卡、订阅卡都踩过：深灰卡 + 蓝色圆角边）。需要另一种
    // 表面就再加一个建树型 composable（如 `SelectableTile`），不要事后装修。
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

// 可选中卡片的形状常量（代理页节点卡为标准，见 ui.h 的说明）。
constexpr float kSelectableTileRadius = 8.0F;

SelectableTileColors ResolveSelectableTileColors(const huxerui::ThemeSpec& theme,
                                                 bool selected) {
    SelectableTileColors colors;
    colors.surface =
        selected ? theme.colors.primary : ResolveIslandTheme(theme).active;
    colors.fg = selected ? theme.colors.on_primary : theme.colors.on_surface;
    colors.muted =
        selected ? theme.colors.on_primary : theme.colors.on_surface_variant;
    colors.faint = selected ? theme.colors.on_primary : theme.colors.outline;
    if (selected) {
        // 选中态是实心底：次级/更弱信息用同色降透明度，保持可读层次。
        colors.muted.alpha = 0.78F;
        colors.faint.alpha = 0.62F;
    }
    return colors;
}

[[huxerui::composable]] huxerui::View SelectableTile(
    huxerui::View content, bool selected, std::string semantics_label,
    std::function<void()> on_click) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const SelectableTileColors colors =
        ResolveSelectableTileColors(theme, selected);
    const bool interactive = static_cast<bool>(on_click);
    // 表面与圆角在建树时就落在 Column 上（不能在返回值外面补修饰符，原因见 Card
    // 的注释）：未选中 islands.active、选中 primary 实心底。
    huxerui::View tile =
        huxerui::Column {std::move(content)}
            .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 8.0F)),
                  huxerui::Background(colors.surface),
                  huxerui::CornerRadius(kSelectableTileRadius),
                  huxerui::ClipChildren(),
                  huxerui::Grow(1.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                  huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                     .label = std::move(semantics_label)},
                  huxerui::Focusable(interactive),
                  huxerui::Enabled(interactive));
    if (interactive) {
        tile = std::move(tile).OnClick(std::move(on_click));
    }
    return tile;
}

// 选中容器（primary 实心底）里的进度条：ProgressBar 的轨道/指示色来自主题，
// 铺在 primary 底上会看不见，因此选中态改用 on_primary 前后景自绘同尺寸圆角条。
// 未选中的容器继续用 ProgressBar（主题色正常显示）。
huxerui::View SelectedProgressBar(float progress, huxerui::Color foreground,
                                  float height) {
    const float ratio = std::clamp(progress, 0.0F, 1.0F);
    huxerui::Color track = foreground;
    track.alpha = 0.28F;
    return huxerui::Canvas(
               [ratio, foreground, track](huxerui::PaintContext& paint,
                                          huxerui::Size size) {
                   if (size.width <= 0.0F || size.height <= 0.0F) return;
                   const huxerui::CornerRadii radii{size.height / 2.0F};
                   paint.FillPath(
                       huxerui::Path::RoundedRect(
                           {0.0F, 0.0F, size.width, size.height}, radii),
                       track);
                   const float filled = size.width * ratio;
                   if (filled > 0.0F) {
                       paint.FillPath(
                           huxerui::Path::RoundedRect(
                               {0.0F, 0.0F, filled, size.height}, radii),
                           foreground);
                   }
               })
        .With(huxerui::Frame{.height = height});
}

[[huxerui::composable]] huxerui::View DialogCard(huxerui::View content) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    huxerui::View card = content;
    return std::move(card).With(
        huxerui::Shadow{ThemeShadowColor(theme, 0.30F), {}, 28.0F, 2.0F},
        huxerui::Background(islands.overlay),
        huxerui::CornerRadius(islands.island_radius),
        huxerui::Border(islands.outline_soft, 1.0F),
        huxerui::ClipChildren(),
        huxerui::Padding(islands.island_padding));
}


// ---- 订阅列表：唯一来源（见 profiles_model.h）-------------------------------

void DriveProfilesModel(huxerui::TaskScope tasks,
                        std::shared_ptr<ProfilesModel> model) {
    tasks.Launch([model]() -> huxerui::Task<void> {
        std::uint64_t seenRevision = 0;
        std::uint64_t seenSync = 0;
        bool first = true;
        for (;;) {
            const std::uint64_t before = model->syncTick.Get();
            const std::uint64_t revision =
                clashflux::persistence::persistence().profilesRevision();
            // 修订号只在三个写入口自增，hydrate 不动它；首帧那次读取早于
            // hydrate 时列表会是空的，此时唯一的补救是调用方 hydrate 之后的
            // RequestSync——所以 syncTick 变化必须强制拉一次，否则订阅列表
            // （以及镜像它的订阅页/规则页下拉/托盘菜单）永远停在空表。
            if (first || revision != seenRevision || before != seenSync) {
                first = false;
                seenRevision = revision;
                seenSync = before;
                auto next = co_await RunOnTaskThread(
                    [] { return store::profilesStore().list(); });
                // 乐观选中期间不发布；等页面确认后再由修订号驱动同步。
                if (!model->selectionPending.Get()) {
                    model->list = std::move(next);
                }
            }
            if (model->syncTick.Get() != before) continue;
            co_await huxerui::Delay(std::chrono::duration<double>{1.0});
        }
    });
}

// ---- 应用级设置：唯一读取点（见 settings_model.h）---------------------------

void DriveSettingsModel(huxerui::TaskScope tasks,
                        std::shared_ptr<SettingsModel> model) {
    tasks.Launch([model]() -> huxerui::Task<void> {
        for (;;) {
            const std::uint64_t before = model->syncTick.Get();
            SettingsView next;
            auto& core = store::coreStore();
            next.ready = clashflux::persistence::persistence().ready();
            const std::string themeMode = core.setting("ui.theme_mode", "1");
            next.themeMode = themeMode == "0" ? 0 : themeMode == "2" ? 2 : 1;
            next.themeColor = core.setting("ui.theme_color", "blue");
            next.customThemeColors = core.setting("ui.custom_theme_colors", "");
            next.autoStart = core.setting("app.autostart", "false") == "true";
            next.autoRun = core.setting("app.auto_run", "false") == "true";
            next.trayEnabled = core.setting("tray.enabled", "true") == "true";
            next.startMinimized =
                core.setting("tray.start_minimized", "false") == "true";
            next.envShell = core.setting("ui.env_shell", "");
            next.language = core.setting("ui.language", "system");
            // 都是内存读；State 按 operator== 去重，只有真的变了才通知订阅者。
            model->view = std::move(next);
            if (model->syncTick.Get() != before) continue;
            co_await huxerui::Delay(std::chrono::duration<double>{1.0});
        }
    });
}

// ---- 原生连接状态：唯一来源（见 vpn_model.h）-------------------------------

void DriveVpnModel(huxerui::TaskScope tasks, std::shared_ptr<VpnModel> model) {
    tasks.Launch([model]() -> huxerui::Task<void> {
        for (;;) {
            const std::uint64_t before = model->syncTick.Get();
            auto next = co_await RunOnTaskThread([] {
                return std::pair{store::vpnStore().states(),
                                 store::vpnStore().openVpnStates()};
            });
            // 快照很小：逐字段比较判脏，变了才写 State（State 本身也按
            // operator== 去重，这里比较是为了不产生无谓的临时拷贝）。
            if (model->pptp.Get() != next.first) {
                model->pptp = std::move(next.first);
            }
            if (model->openvpn.Get() != next.second) {
                model->openvpn = std::move(next.second);
            }
            if (model->syncTick.Get() != before) continue;
            co_await huxerui::Delay(std::chrono::duration<double>{1.0});
        }
    });
}

// ---- 推送流变化 → UI 线程（见 stream_updates.h）-----------------------------

std::uint64_t SubscribeStreamUpdates(
    huxerui::TaskScope tasks,
    std::function<void(stream::StreamKind kind)> handler) {
    auto shared =
        std::make_shared<std::function<void(stream::StreamKind)>>(std::move(handler));
    return stream::addStreamUpdateObserver(
        [tasks, shared](stream::StreamKind kind) {
            // 推送线程：只 Post（scope 已关闭时 Post 被忽略），不在本线程碰 State。
            tasks.Post([shared, kind] {
                if (*shared) (*shared)(kind);
                stream::acknowledgeStreamUpdate(kind);
            });
        });
}

void UnsubscribeStreamUpdates(std::uint64_t id) {
    stream::removeStreamUpdateObserver(id);
}

} // namespace clashflux::ui
