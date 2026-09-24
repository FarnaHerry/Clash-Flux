// profiles_page_card.cpp — 订阅卡片与原生连接状态展示。
#include <huxerui/huxerui.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app_resources.h"
#include "ui.h"
#include "task_bridge.h"

import clashflux.core;
import clashflux.db;
import clashflux.openvpn;
import clashflux.pptp;
import clashflux.store.core;
import clashflux.store.profiles;
import clashflux.store.vpn;
import clashflux.utils;
import clashflux.vpn;

#include "profiles_page_shared.h"

namespace clashflux::ui {

namespace {

std::optional<std::int64_t> CurrentSelectedProfile(
    huxerui::StateList<db::Profile> profiles) {
    for (const db::Profile& profile : profiles) {
        if (profile.selected) return profile.id;
    }
    return std::nullopt;
}

void SetSelectedProfile(huxerui::StateList<db::Profile> profiles,
                        std::optional<std::int64_t> selectedId) {
    for (std::size_t index = 0; index < profiles.Size(); ++index) {
        db::Profile profile = profiles[index];
        const bool selected = selectedId.has_value() &&
                              profile.id == selectedId.value();
        if (profile.selected == selected) continue;
        profile.selected = selected;
        profiles.Set(index, std::move(profile));
    }
}

} // namespace

[[huxerui::composable]] huxerui::View ProfileCard(
    const db::Profile& profile, bool compact,
    huxerui::TaskScope tasks, huxerui::ToastHandle toast,
    std::shared_ptr<AppHttpClient> http, std::function<void()> reload,
    huxerui::StateList<db::Profile> profiles,
    huxerui::State<bool> selectionPending,
    const store::PptpState& pptpState,
    const store::OpenVpnState& openVpnState, bool connectionSelected,
    const std::function<void(std::int64_t, bool)>& toggleConnection,
    const std::function<void(std::int64_t)>& openEditInfo,
    const std::function<void(std::int64_t)>& openEditRules,
    const std::function<void(std::int64_t)>& openEditFile,
    const std::function<void(std::int64_t)>& openQr,
    huxerui::DialogHandle dialog) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    // Each card owns its presentation anchor. Sharing one page-level MenuHandle
    // across compact cards mounts the same LayerAnchor on multiple Views, which
    // Android rejects during the next frame with "anchor must be mounted on only
    // one View" when the subscription page contains more than one card.
    auto menu = huxerui::UseMenu();
    const IslandTheme islands = ResolveIslandTheme(theme);
    const std::int64_t id = profile.id;
    const bool nativeVpn = isNativeVpnType(profile.type);
    const bool selected = profile.selected;

    auto action = [tasks, toast, reload](std::function<std::string()> job) {
        tasks.Launch([=]() -> huxerui::Task<void> {
            const std::string err = co_await RunOnTaskThread(std::move(job));
            if (!err.empty()) toast.Show(err);
            reload();
        });
    };

    auto activateProfile = [tasks, toast, profiles,
                            selectionPending](std::int64_t profileId) {
        if (selectionPending.Get()) return;
        const std::optional<std::int64_t> previous =
            CurrentSelectedProfile(profiles);
        if (previous == profileId) return;
        SetSelectedProfile(profiles, profileId);
        selectionPending = true;
        tasks.Launch([tasks, toast, profiles, selectionPending, previous,
                      profileId]() -> huxerui::Task<void> {
            std::string error;
            try {
                error = co_await RunOnTaskThread([profileId] {
                    auto& profileStore = store::profilesStore();
                    if (!profileStore.activate(profileId))
                        return profileStore.lastError();
                    return std::string{};
                });
            } catch (const std::exception& exception) {
                error = exception.what();
            }
            if (!error.empty()) {
                SetSelectedProfile(profiles, previous);
                toast.Show(error);
            }
            selectionPending = false;
        });
    };

    // 平台栈刷新路径（Android）：HttpClient 不许进阻塞线程池，走协程抓取。
    auto httpRefresh = [tasks, toast, reload,
                        http](std::int64_t pid) {
        tasks.Launch([toast, reload, http, pid]() -> huxerui::Task<void> {
            const std::string err =
                co_await AndroidRefreshRemote(http, pid);
            if (!err.empty()) toast.Show(err);
            reload();
        });
    };
    const ProfileRefreshAction desktopRefresh = [action](std::int64_t pid) {
        action([pid]() -> std::string {
            auto& ps = store::profilesStore();
            if (!ps.refresh(pid)) return ps.lastError();
            return "";
        });
    };
    const ProfileRefreshAction refresh = [httpRefresh, desktopRefresh](
                                             std::int64_t pid) {
        profile_detail::RefreshProfileForPlatform(pid, httpRefresh, desktopRefresh);
    };

    const auto confirmDelete = [dialog, tasks, action, id, nativeVpn,
                                openVpn = profile.type == "openvpn",
                                profileName = profile.name,
                                errorColor = theme.colors.error,
                                onErrorColor = huxerui::Color::White(),
                                textColor = theme.colors.on_surface] {
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            dialog.Show(
                [=](huxerui::DialogContext context) -> huxerui::View {
                    huxerui::ButtonStyle danger = huxerui::ButtonStyle::Default();
                    danger.background = errorColor;
                    danger.label_style = huxerui::TextStyle{
                        huxerui::Font::System(font_size::kBody), onErrorColor};
                    return DialogCard(huxerui::Column {
                        huxerui::Text("删除订阅", huxerui::TextRole::Title),
                        huxerui::Text("确定删除“" + profileName + "”吗？此操作无法撤销。")
                            .Style(huxerui::TextStyle{
                                huxerui::Font::System(font_size::kBody), textColor}),
                        huxerui::Row {
                            huxerui::Button("取消").OnClick(
                                [context] { context.Dismiss(); }),
                            huxerui::ProvideEnvironment(
                                danger,
                                huxerui::Button("删除").OnClick(
                                    [=] {
                                        context.Dismiss();
                                        action([=]() -> std::string {
                                            if (nativeVpn) {
                                                if (openVpn) {
                                                    store::vpnStore().forgetOpenVpn(id);
                                                } else {
                                                    store::vpnStore().forgetPptp(id);
                                                }
                                            }
                                            auto& ps = store::profilesStore();
                                            ps.remove(id);
                                            return ps.lastError();
                                        });
                                    })),
                        }.With(huxerui::Spacing(8.0F),
                               huxerui::MainAlign(huxerui::MainAxisAlignment::End)),
                    }.With(huxerui::Spacing(12.0F),
                           huxerui::Frame{.width = 360.0F},
                           huxerui::CrossAlign(
                               huxerui::CrossAxisAlignment::Stretch)));
                },
                huxerui::DialogOptions{});
        });
    };

    // 桌面端由右键触发，Compact 由卡片上的触控按钮触发；菜单内容只维护
    // 一份，避免移动端和桌面端的订阅操作逐渐产生行为差异。
    const auto buildMenuEntries = [action, activateProfile, refresh, openEditInfo, openEditRules,
                                   openEditFile, openQr, id,
                                   homepage = profile.homepage, url = profile.url,
                                   selected, nativeVpn,
                                   confirmDelete, errorColor = theme.colors.error] {
        std::vector<huxerui::MenuEntry> entries;
        if (!selected && !nativeVpn) {
            entries.push_back(huxerui::MenuItem("使用", [activateProfile, id] {
                activateProfile(id);
            }));
        }
        if (!nativeVpn) {
            entries.push_back(huxerui::MenuItem("更新", [refresh, id] {
                refresh(id);
            }));
        }
        if (!nativeVpn && !homepage.empty()) {
            entries.push_back(huxerui::MenuItem("首页", [action, homepage] {
                action([homepage]() -> std::string {
                    core::openInBrowser(homepage);
                    return "";
                });
            }));
        }
        if (!nativeVpn && !url.empty()) {
            entries.push_back(huxerui::MenuItem("分享二维码", [openQr, id] {
                openQr(id);
            }));
        }
        // 原生 VPN 卡片没有“使用/更新”等前置菜单项，不能在菜单开头
        // 插入分隔线；HuxerUI 要求 MenuSection 必须夹在两个菜单项之间。
        if (!entries.empty()) entries.push_back(huxerui::MenuSection{});
        entries.push_back(huxerui::MenuItem("编辑信息", [openEditInfo, id] {
            openEditInfo(id);
        }));
        if (!nativeVpn) {
            entries.push_back(huxerui::MenuItem("编辑规则", [openEditRules, id] {
                openEditRules(id);
            }));
            entries.push_back(huxerui::MenuItem("编辑文件", [openEditFile, id] {
                openEditFile(id);
            }));
        }
        entries.push_back(huxerui::MenuSection{});
        entries.push_back(
            huxerui::MenuItem(app::images::trash, "删除", confirmDelete)
                .IconTint(errorColor));
        return entries;
    };

    // 右上角刷新图标：裸 Image + Tint 着色（IconButton 不着色矢量资源；
    // 同 apitab 标题栏齿轮配方），自绘 24pt 正方形热区。
    huxerui::View refreshButton = huxerui::Row{};
    if (!nativeVpn) {
        refreshButton = huxerui::Row {
            huxerui::Image(app::images::refresh)
                .Fit(huxerui::ImageFit::Contain)
                .Align(huxerui::HorizontalAlignment::Center,
                       huxerui::VerticalAlignment::Center)
                .Tint(theme.colors.on_surface_variant)
                .With(huxerui::Frame{.width = 14.0F, .height = 14.0F}),
        }
            .With(huxerui::Padding(5.0F),
                  huxerui::CornerRadius(islands.nested_radius),
                  huxerui::Tooltip("更新订阅"),
                  huxerui::Focusable(true),
                  huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                     .label = "更新订阅"})
            .OnClick([refresh, id] { refresh(id); });
    }

    huxerui::View moreButton = huxerui::Row{};
    if (compact) {
        // 手机没有鼠标右键；使用可见按钮打开同一份卡片操作菜单。锚点挂在
        // 按钮本身，菜单在窄屏上会自动避开屏幕边缘。
        moreButton = huxerui::IconButton(app::images::more_vertical, "更多操作")
                         .With(menu.Anchor(), huxerui::Tooltip("更多操作"))
                         .OnClick([menu, buildMenuEntries] {
                             menu.Show(buildMenuEntries());
                         });
    }

    huxerui::View profileName =
        huxerui::Text(truncateOneLine(profile.name, compact ? 9 : 20)).Style(
            huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody)
                    .WithWeight(huxerui::FontWeight::SemiBold),
                theme.colors.on_surface});
    if (compact) profileName = std::move(profileName).With(huxerui::Grow(1.0F));

    const std::string primaryLine = [&] {
        if (!nativeVpn) {
            return profile.url.empty() ? std::string("本地导入") : profile.url;
        }
        if (profile.type == "openvpn") return std::string("OpenVPN · sing-box endpoint");
        std::string parseError;
        const auto config = pptp::ParsePptpConfig(profile.nativeConfig, parseError);
        return config ? "PPTP · " + config->server
                      : std::string("PPTP 配置无效");
    }();

    huxerui::View card = Card(huxerui::Column {
        huxerui::Row {
            std::move(profileName),
            selected
                ? huxerui::View{
                      huxerui::Text("使用中").Style(huxerui::TextStyle{
                          huxerui::Font::System(font_size::kCaption),
                          theme.colors.on_primary})}
                      .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(
                                8.0F, 2.0F)),
                            huxerui::Background(theme.colors.primary),
                            huxerui::CornerRadius(islands.nested_radius))
                : huxerui::View{huxerui::Row{}},
            nativeVpn
                ? huxerui::View{huxerui::Checkbox(connectionSelected)
                                    .OnChanged([toggleConnection, tasks,
                                                id](bool checked) {
                                        // Checkbox 的 OnChanged 仍处于指针释放事件
                                        // 路径；延迟一拍再改 StateList，避免 VirtualGrid
                                        // 重组导致 HuxerUI 同一事件的 pointer session
                                        // 迭代器失效。
                                        tasks.Launch([toggleConnection, id,
                                                      checked]()
                                                         -> huxerui::Task<void> {
                                            co_await huxerui::Delay(
                                                std::chrono::duration<double>{0});
                                            toggleConnection(id, checked);
                                        });
                                    })}
                : huxerui::View{huxerui::Row{}},
            huxerui::Spacer(),
            std::move(refreshButton),
            std::move(moreButton),
        }.With(huxerui::Spacing(6.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Text(truncateOneLine(primaryLine, 48))
            .Style(huxerui::TextStyle{huxerui::Font::Monospace(font_size::kChip),
                                      theme.colors.on_surface_variant}),
        huxerui::Text(truncateOneLine(
                          nativeVpn
                              ? (profile.nativeRoutes.empty()
                                     ? "未设置内网路由"
                                     : "路由：" + profile.nativeRoutes)
                              : (profile.description.empty() ? "—"
                                                              : profile.description),
                          52))
            .Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption),
                nativeVpn || profile.description.empty()
                    ? theme.colors.outline
                    : theme.colors.on_surface_variant}),
        huxerui::Row {
            huxerui::Text(nativeVpn
                              ? profile.type == "openvpn" ? "OpenVPN" : "PPTP"
                              : profile.type == "local" ? "本地" : "远程")
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    theme.colors.on_surface_variant}),
            !nativeVpn && profile.autoUpdate && profile.intervalMins > 0
                ? huxerui::View{huxerui::Text(std::format("自动 {} 分钟",
                                                          profile.intervalMins))
                                    .Style(huxerui::TextStyle{
                                        huxerui::Font::System(
                                            font_size::kCaption),
                                        theme.colors.on_surface_variant})}
                : huxerui::View{huxerui::Row{}},
            huxerui::Spacer(),
            nativeVpn
                ? huxerui::View{huxerui::Text(profile.type == "openvpn"
                                                   ? openVpnStateText(openVpnState)
                                                   : pptpStateText(pptpState))
                                    .Style(huxerui::TextStyle{
                                        huxerui::Font::System(font_size::kCaption),
                                        (profile.type == "openvpn"
                                             ? openVpnState.state
                                             : pptpState.state) ==
                                                vpn::ConnectionState::Failed
                                            ? theme.colors.error
                                            : theme.colors.on_surface_variant})}
                : huxerui::View{huxerui::Row{}},
            huxerui::Text(truncateOneLine(
                              nativeVpn
                                  ? ""
                                  : profile.error.empty()
                                  ? (profile.updatedAt > 0
                                         ? "更新于 " + formatTime(profile.updatedAt)
                                         : "未拉取")
                                  : "错误：" + profile.error,
                              52))
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    nativeVpn || profile.error.empty()
                        ? theme.colors.on_surface_variant
                        : theme.colors.error}),
        }
            .With(huxerui::Spacing(8.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        // 流量行 + 进度条（订阅响应头 subscription-userinfo；total 未知不显示）。
        !nativeVpn && profile.totalBytes > 0
            ? huxerui::View{huxerui::Column {
                  huxerui::Row {
                      huxerui::Text("已用 " + formatBytes(profile.usedBytes) +
                                    " / " + formatBytes(profile.totalBytes))
                          .Style(huxerui::TextStyle{
                              huxerui::Font::System(font_size::kCaption),
                              theme.colors.on_surface_variant}),
                      huxerui::Spacer(),
                      huxerui::Text(std::format(
                          "{}%", static_cast<int>(
                                     std::clamp(
                                         static_cast<double>(profile.usedBytes) /
                                             static_cast<double>(
                                                 profile.totalBytes),
                                         0.0, 1.0) *
                                     100.0)))
                          .Style(huxerui::TextStyle{
                              huxerui::Font::System(font_size::kCaption),
                              theme.colors.on_surface_variant}),
                  }
                      .With(huxerui::CrossAlign(
                          huxerui::CrossAxisAlignment::Center)),
                  huxerui::ProgressBar(std::clamp(
                                           static_cast<float>(
                                               profile.usedBytes) /
                                               static_cast<float>(
                                                   profile.totalBytes),
                                           0.0F, 1.0F))
                      .With(huxerui::Frame{.height = 4.0F}),
              }
                                .With(huxerui::Spacing(4.0F),
                                      huxerui::CrossAlign(
                                          huxerui::CrossAxisAlignment::Stretch))}
            : huxerui::View{huxerui::Row{}},
    }.With(huxerui::Spacing(6.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));

    // 矩形卡统一高度；宽度由虚拟网格的轨道均分，铺满页面而不固定尺寸
    // （内容已单行截断，ClipChildren 兜底）。
    card = std::move(card).With(huxerui::Frame{.height = kCardHeight},
                                huxerui::ClipChildren());
    // 选中（使用中）状态：primary 描边，与「使用中」徽标呼应。
    if (selected) {
        card = std::move(card).With(
            huxerui::Border(theme.colors.primary, 2.0F));
    }
    return std::move(card)
        // 远程/本地代理订阅暂时保持单选：点击哪张卡片，哪张就是当前订阅。
        // PPTP 仍使用系统接口；OpenVPN 已由 sing-box endpoint 统一承载。
        .OnClick([activateProfile, id, selected, nativeVpn] {
                if (selected || nativeVpn) return;
                activateProfile(id);
            })
        // 右键上下文菜单（跟随点击位置弹出）。
        .On<huxerui::ViewEvents::ContextMenuRequested>(
            [menu, tasks, buildMenuEntries](huxerui::Point pos) {
                // ContextMenuRequested 仍处于鼠标释放事件路径；同步创建菜单
                // 会重组浮层树，使 HuxerUI 正在清理的 pointer session 迭代器
                // 失效。延迟到下一帧再挂载菜单。
                tasks.Launch([menu, pos, entries = buildMenuEntries()]()
                                 mutable -> huxerui::Task<void> {
                    co_await huxerui::Delay(std::chrono::duration<double>{0});
                    menu.ShowAt(pos, std::move(entries));
                });
            })
        .Key(id);
}

} // namespace clashflux::ui
