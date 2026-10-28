// profiles_page_card.cpp — 订阅卡片与原生连接状态展示。
#include <huxerui/huxerui.h>

#include <chrono>
#include <functional>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "app_resources.h"
#include "ui.h"
#include "action_menu.h"
#include "task_bridge.h"
#include "pptp_connection_progress.h"

import clashflux.core;
import clashflux.db;
import clashflux.pptp;
import clashflux.store.core;
import clashflux.store.profiles;
import clashflux.store.vpn;
import clashflux.utils;
import clashflux.vpn;

#include "profiles_page_shared.h"
#include "core_model.h"
#include "profiles_model.h"

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

bool HasKnownExhaustedQuota(const db::Profile& profile) {
    return profile.type != "pptp" && profile.totalBytes > 0 &&
           profile.usedBytes >= profile.totalBytes;
}

void ShowKnownExhaustedQuota(FluxToastHandle toast,
                             const std::string& profileName) {
    toast.Show(LocalizedFormat("订阅「{}」上次更新的流量数据已用尽",
                               profileName),
               huxerui::ToastOptions{6.0});
}

huxerui::StringVariant CompactProfileStatus(const db::Profile& profile, bool participating) {
    return profile.selected ? Localized("主订阅")
        : participating ? Localized("按规则") : Localized("未参与");
}

[[huxerui::composable]] huxerui::View CompactProfileQuota(
    const db::Profile& profile, huxerui::Color muted) {
    const bool hasTraffic = profile.usedBytes > 0 || profile.totalBytes > 0;
    const std::string traffic = !hasTraffic ? "" : huxerui::UseString(profile.totalBytes > 0
        ? LocalizedFormat("{} / {}", formatBytes(profile.usedBytes), formatBytes(profile.totalBytes))
        : LocalizedFormat("{} / 不限量", formatBytes(profile.usedBytes)));
    const std::string expiry = profile.expireAt <= 0 ? ""
        : huxerui::UseString(LocalizedFormat("到期 {}", formatDate(profile.expireAt)));
    return CompactProfileTrafficContent(traffic, expiry,
        ProfileTrafficProgress(profile.usedBytes, profile.totalBytes), muted);
}

[[huxerui::composable]] huxerui::View ProfileDragPreview(
    const db::Profile& profile, bool compact, bool participating, float width) {
    const auto& theme = huxerui::UseTheme();
    const bool nativeVpn = isNativeVpnType(profile.type);
    const auto colors = ResolveSelectableTileColors(theme, profile.selected);
    const std::string primaryLine = [&] {
        if (!nativeVpn)
            return profile.url.empty() ? huxerui::UseString(Localized("本地导入")) : profile.url;
        std::string error;
        const auto config = pptp::ParsePptpConfig(profile.nativeConfig, error);
        return config ? "PPTP · " + config->server
                      : huxerui::UseString(Localized("PPTP 配置无效"));
    }();
    const std::string detailLine = nativeVpn
        ? (profile.nativeRoutes.empty()
               ? huxerui::UseString(Localized("未设置内网路由"))
               : huxerui::UseString(LocalizedFormat("路由：{}", profile.nativeRoutes)))
        : (profile.description.empty() ? "—" : profile.description);
    const std::string statusLine = nativeVpn
        ? "PPTP"
        : profile.selected ? huxerui::UseString(Localized("主订阅"))
        : participating ? huxerui::UseString(Localized("次订阅 · 按规则参与"))
                        : huxerui::UseString(Localized("已保存 · 未参与"));

    huxerui::View previewContent;
    if (compact) {
        previewContent = CompactProfileCardContent(profile.name,
            nativeVpn ? "" : huxerui::UseString(CompactProfileStatus(profile, participating)), colors,
            huxerui::Image(app::images::drag_indicator).Tint(colors.muted)
                .With(huxerui::Frame{.width = 18.0F, .height = 18.0F}, huxerui::Padding(1.0F)),
            huxerui::Image(app::images::more_vertical).Tint(colors.muted)
                .With(huxerui::Frame{.width = 20.0F, .height = 20.0F},
                      huxerui::Padding(nativeVpn ? 6.0F : 10.0F)),
            nativeVpn ? FittedSingleLineLabel(primaryLine, huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption), colors.muted})
                .With(huxerui::Frame{.height = 32.0F})
                : CompactProfileQuota(profile, colors.muted), nativeVpn);
    } else {
        previewContent = huxerui::Column {
            huxerui::Row {
                huxerui::Image(app::images::drag_indicator)
                    .Fit(huxerui::ImageFit::Contain)
                    .Tint(colors.muted)
                    .With(huxerui::Frame{.width = 18.0F, .height = 18.0F},
                          huxerui::Padding(1.0F)),
                huxerui::Text(truncateOneLine(profile.name, compact ? 9 : 20))
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kBody)
                            .WithWeight(huxerui::FontWeight::SemiBold),
                        colors.fg}),
                huxerui::Spacer(),
                nativeVpn ? huxerui::View{huxerui::Row{}}
                          : huxerui::View{huxerui::Image(app::images::refresh)
                                .Fit(huxerui::ImageFit::Contain)
                                .Tint(colors.muted)
                                .With(huxerui::Frame{.width = 20.0F, .height = 20.0F},
                                      huxerui::Padding(4.0F))},
            }.With(huxerui::Spacing(6.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
            huxerui::Text(truncateOneLine(primaryLine, 48))
                .Style(huxerui::TextStyle{
                    huxerui::Font::Monospace(font_size::kChip), colors.muted}),
            huxerui::Text(truncateOneLine(detailLine, 52))
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption), colors.muted}),
            huxerui::Row {
                huxerui::Text(statusLine)
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kCaption), colors.muted}),
                huxerui::Spacer(),
                huxerui::Text(nativeVpn ? "" : huxerui::UseString(
                    FormatRelativeTime(profile.updatedAt)))
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kCaption), colors.muted}),
            }.With(huxerui::Spacing(8.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
            huxerui::Spacer(),
            nativeVpn ? huxerui::View{huxerui::Row{}}
                      : huxerui::View{huxerui::ProgressBar(std::clamp(
                            profile.totalBytes > 0
                                ? static_cast<float>(profile.usedBytes) /
                                      static_cast<float>(profile.totalBytes)
                                : 1.0F,
                            0.0F, 1.0F)).With(huxerui::Frame{.height = 3.0F})},
        }.With(huxerui::Spacing(6.0F), huxerui::Grow(1.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }
    huxerui::View preview = SelectableTile(previewContent,
        profile.selected, profile.name, {}, SelectableTileInteraction::ReadOnly);
    huxerui::Color shadow = theme.colors.scrim;
    shadow.alpha = 0.24F;
    return std::move(preview).With(
        huxerui::Frame{.width = width, .height = ProfileCardHeight(compact)},
        huxerui::Border(theme.colors.primary, kCardEmphasisOutlineWidth),
        huxerui::CornerRadius(kSelectableTileRadius),
        huxerui::Shadow{.color = shadow, .blur_radius = 16.0F, .spread = -2.0F},
        // Compact 的 20pt 手柄在标题行中居中，补偿留白和垂直居中偏移。
        huxerui::Offset(huxerui::Point{-10.0F, -8.0F - (compact
            ? (CompactProfileCardHeadingHeight(nativeVpn) - 20.0F) * 0.5F : 0.0F)}));
}

} // namespace

[[huxerui::composable]] huxerui::View ProfileCard(
    const db::Profile& profile, bool compact,
    huxerui::TaskScope tasks, FluxToastHandle toast,
    std::shared_ptr<AppHttpClient> http, std::function<void()> reload,
    huxerui::StateList<db::Profile> profiles,
    huxerui::State<bool> selectionPending,
    const store::PptpState& pptpState,
    bool connectionPending, bool openingConnection, bool connectionActionsBusy,
    const std::function<void(std::int64_t)>& togglePptpConnection,
    const std::function<void(std::int64_t)>& openEditInfo,
    const std::function<void(std::int64_t)>& openEditRules,
    const std::function<void(std::int64_t)>& openEditFile,
    const std::function<void(std::int64_t)>& openQr,
    huxerui::DialogHandle dialog,
    ProfileReorderActions reorder) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto clipboard = huxerui::UseApplication().Clipboard();
    // Each card owns its presentation anchor. Sharing one page-level MenuHandle
    // across compact cards mounts the same LayerAnchor on multiple Views, which
    // Android rejects during the next frame with "anchor must be mounted on only
    // one View" when the subscription page contains more than one card.
    auto menu = UseActionMenu();
    auto isDragging = huxerui::UseState(false);
    const auto navigation = huxerui::UseNavigation();
    const auto desktopNavigation = huxerui::UseEnvironment<DesktopSettingsNavigation>().model;
    const auto coreModel = huxerui::UseService<CoreModel>();
    const auto& runtime = coreModel->view.Get().core;
    const IslandTheme islands = ResolveIslandTheme(theme);
    const std::int64_t id = profile.id;
    const bool nativeVpn = isNativeVpnType(profile.type);
    const auto profilesModel = huxerui::UseService<ProfilesModel>();
    const bool editableRules = profilesModel->IsRuleSource(id);
    const bool selected = profile.selected;
    // 表面/文字色与选中态都来自统一原语 SelectableTileColors（形状以代理页节点卡
    // 为标准，见 ui.h）。次级文字选中时是同色降透明度；错误信息保持 error
    // （选中与否都该醒目，不让选中态吞掉异常）。
    const SelectableTileColors tile =
        ResolveSelectableTileColors(theme, selected);
    const huxerui::Color fg = tile.fg;
    const huxerui::Color muted = tile.muted;
    const huxerui::Color faint = tile.faint;
    // 刷新忙态与实际任务同生命周期：按钮/菜单触发时置位，任务线程回到 UI
    // 线程后清除；忙态中图标换成 ProgressCircle 并忽略重复触发，不再用
    // 与真实时长无关的「点击转一圈」装饰动画。
    const bool refreshBusy = profilesModel->Refreshing(id);

    const auto applicationTasks = huxerui::UseApplicationTaskScope();
    auto action = [applicationTasks, toast, reload](std::function<std::string()> job) {
        applicationTasks.Launch([=]() -> huxerui::Task<void> {
            std::string error;
            try { error = co_await RunOnTaskThread(std::move(job)); }
            catch (const std::exception& exception) { error = exception.what(); }
            reload();
            if (!error.empty()) toast.Show(error);
        });
    };

    auto activateProfile = [applicationTasks, toast, profilesModel, profiles,
                            coreModel](std::int64_t profileId) {
        const std::optional<std::int64_t> previous =
            CurrentSelectedProfile(profiles);
        if (previous == profileId) return;
        SetSelectedProfile(profiles, profileId);
        using Outcome = std::tuple<std::string, store::CoreSnapshot, std::string>;
        LaunchProfileSelection<Outcome>(applicationTasks, profilesModel, profileId,
            [profileId](std::stop_token cancel) -> huxerui::Task<Outcome> {
                co_return co_await RunOnTaskThread([profileId, cancel] {
                    auto& profileStore = store::profilesStore();
                    std::string error, summary;
                    profileStore.activate(profileId, cancel, &error, &summary);
                    return Outcome{std::move(error), store::coreStore().snapshot(), std::move(summary)};
                });
            }, [toast, profilesModel, coreModel, profileId](
                    std::optional<Outcome> outcome, std::exception_ptr failure, std::uint64_t) {
            std::string error;
            if (failure) {
                try { std::rethrow_exception(failure); }
                catch (const std::exception& exception) { error = exception.what(); }
                catch (...) { error = "订阅切换失败"; }
            } else if (outcome) {
                error = std::get<0>(*outcome);
                coreModel->Update([&](CoreView& view) { view.core = std::get<1>(*outcome); });
            }
            profilesModel->PublishCached();
            if (!error.empty()) {
                toast.Show(error);
            } else if (outcome) {
                const auto& currentProfiles = profilesModel->list.Get();
                const auto current = std::ranges::find(
                    currentProfiles, profileId, &db::Profile::id);
                if (current != currentProfiles.end() &&
                    HasKnownExhaustedQuota(*current)) {
                    ShowKnownExhaustedQuota(toast, current->name);
                } else if (!std::get<2>(*outcome).empty()) {
                    // Reuse the validated candidate ledger; feedback must not launch
                    // another full compiler/JSON serialization request.
                    toast.Show(LocalizedFormat("已设为主订阅 · {}", std::get<2>(*outcome)), huxerui::ToastOptions{6.0});
                }
            }
        });
    };

    // Both card/menu and the rules panel observe one application-owned ID.
    const auto startRefresh = [applicationTasks, profilesModel, http, toast, reload,
                               profileName = profile.name](std::int64_t pid) {
        using Result = std::pair<std::string, std::string>;
        LaunchProfileRefresh<Result>(applicationTasks, profilesModel, pid,
            [http, pid]() -> huxerui::Task<Result> {
                auto error = co_await profile_detail::RefreshProfileForPlatform(http, pid);
                std::string summary;
                if (error.empty()) summary = co_await RunOnTaskThread([pid] { return ProfileFidelitySummary(pid); });
                co_return Result{std::move(error), std::move(summary)};
            }, [toast, reload, profilesModel, pid, profileName](
                    std::optional<Result> result, std::exception_ptr exception) {
                reload();
                if (exception) {
                    std::string message;
                    try { std::rethrow_exception(exception); }
                    catch (const std::exception& error) { message = error.what(); }
                    catch (...) { message = "订阅更新发生未知异常"; }
                    profilesModel->ReportProfileRefreshFailure(
                        pid, profileName, std::move(message), true);
                } else if (!result->first.empty()) {
                    profilesModel->ReportProfileRefreshFailure(
                        pid, profileName, result->first, true);
                } else {
                    profilesModel->ClearProfileRefreshFailure(pid);
                    if (!result->second.empty())
                        toast.Show(LocalizedFormat("订阅已更新 · {}", result->second), huxerui::ToastOptions{6.0});
                }
            });
    };

    const auto themedButtonStyle = huxerui::UseEnvironment<huxerui::ButtonStyle>();
    const auto confirmDelete = [dialog, tasks, action, id, nativeVpn,
                                profileName = profile.name,
                                errorColor = theme.colors.error,
                                onErrorColor = OnErrorColor(theme),
                                themedButtonStyle,
                                textColor = theme.colors.on_surface] {
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            dialog.Show(
                [=](huxerui::DialogContext context) -> huxerui::View {
                    huxerui::ButtonStyle danger = themedButtonStyle;
                    danger.background = errorColor;
                    danger.label_style = huxerui::TextStyle{
                        huxerui::Font::System(font_size::kBody), onErrorColor};
                    return DialogCard(huxerui::Column {
                        huxerui::Text(Localized("删除订阅"), huxerui::TextRole::Title),
                        huxerui::Text(LocalizedFormat(
                            "确定删除“{}”吗？此操作无法撤销。", profileName))
                            .Style(huxerui::TextStyle{
                                huxerui::Font::System(font_size::kBody), textColor}),
                        huxerui::Row {
                            huxerui::Button(Localized("取消")).OnClick(
                                [context] { context.Dismiss(); }),
                            huxerui::ProvideEnvironment(
                                danger,
                                huxerui::Button(Localized("删除")).OnClick(
                                    [=] {
                                        context.Dismiss();
                                        action([=]() -> std::string {
                                            if (store::coreStore().sourceParticipates(id))
                                                return std::string{"该来源是主订阅、被路由引用或仍连接中；请先更换主订阅、解除规则引用并断开连接"};
                                            if (nativeVpn) {
                                                store::vpnStore().forgetPptp(id);
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
    const auto buildMenuEntries = [action, activateProfile, startRefresh, openEditInfo, openEditRules,
                                   openEditFile, openQr, clipboard, toast, id, navigation, desktopNavigation,
                                   homepage = profile.homepage, url = profile.url,
                                   profileType = profile.type,
                                   selected, nativeVpn, editableRules,
                                   confirmDelete] {
        std::vector<ActionMenuEntry> entries;
        if (!selected && !nativeVpn) {
            entries.push_back(ActionMenuItem(app::images::check, Localized("设为主订阅"), [activateProfile, id] {
                activateProfile(id);
            }));
        }
        if (!nativeVpn) {
            entries.push_back(ActionMenuItem(app::images::refresh, Localized("更新"), [startRefresh, id] {
                startRefresh(id);
            }));
        }
        if (!nativeVpn && !homepage.empty()) {
            entries.push_back(ActionMenuItem(app::images::home, Localized("首页"), [action, homepage] {
                action([homepage]() -> std::string {
                    core::openInBrowser(homepage);
                    return "";
                });
            }));
        }
        if (!nativeVpn && !url.empty()) {
            entries.push_back(ActionMenuItem(app::images::qr_scan, Localized("分享二维码"), [openQr, id] {
                openQr(id);
            }));
            entries.push_back(ActionMenuItem(app::images::link, Localized("复制链接"), [clipboard, toast, url] {
                toast.Show(Localized(clipboard->WriteText(url) ? "已复制到剪贴板" : "复制失败"));
            }));
        }
        // 原生 VPN 没有前置操作，只有已有菜单项时才加入分区线。
        if (!entries.empty()) entries.push_back(ActionMenuSection{});
        entries.push_back(ActionMenuItem(app::images::edit, Localized("编辑信息"), [openEditInfo, id] {
            openEditInfo(id);
        }));
        if (editableRules) {
            entries.push_back(ActionMenuItem(app::images::route, Localized("编辑规则"), [openEditRules, id] {
                openEditRules(id);
            }));
        }
        if (!nativeVpn) {
            entries.push_back(ActionMenuItem(app::images::manual_config, Localized("编辑文件"), [openEditFile, id] {
                openEditFile(id);
            }));
        }
        if (profileType != "pptp") {
            entries.push_back(ActionMenuItem(app::images::shield_check, Localized("配置保真度"),
                [navigation, desktopNavigation, id] {
                    OpenProfileFidelityPage(navigation, desktopNavigation, id);
                }));
        }
        entries.push_back(ActionMenuSection{});
        entries.push_back(
            ActionMenuItem(app::images::trash, Localized("删除"), confirmDelete)
                .Danger());
        return entries;
    };

    // 右上角刷新图标：裸 Image + Tint 着色（IconButton 不着色矢量资源；
    // 同 apitab 标题栏齿轮配方），正常 20pt 尺寸与 4pt 内边距。刷新进行中换成
    // ProgressCircle——指示器的起止由真实任务驱动，而不是固定时长动画。
    huxerui::View refreshButton = huxerui::Row{};
    if (!nativeVpn && !compact) {
        huxerui::View refreshGlyph;
        if (refreshBusy) {
            refreshGlyph = huxerui::View{huxerui::ProgressCircle()}.With(
                huxerui::Frame{.width = 20.0F, .height = 20.0F});
        } else {
            refreshGlyph =
                huxerui::View{huxerui::Image(app::images::refresh)
                                  .Fit(huxerui::ImageFit::Contain)
                                  .Align(huxerui::HorizontalAlignment::Center,
                                         huxerui::VerticalAlignment::Center)
                                  .Tint(muted)}
                    .With(huxerui::Frame{.width = 20.0F, .height = 20.0F});
        }
        refreshButton = huxerui::Row {
            refreshGlyph,
        }
            .With(huxerui::Padding(4.0F),
                  huxerui::CornerRadius(islands.nested_radius),
                  huxerui::Tooltip(Localized("更新订阅")),
                  huxerui::Focusable(true),
                  huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                     .label = huxerui::UseString(
                                         Localized("更新订阅"))})
            .OnClick([startRefresh, id] { startRefresh(id); });
    }

    huxerui::View moreButton = huxerui::Row{};
    if (compact) {
        // 手机没有鼠标右键；使用可见按钮打开同一份卡片操作菜单。锚点挂在
        // 按钮本身，菜单在窄屏上会自动避开屏幕边缘。
        moreButton = huxerui::Row {
            huxerui::Image(app::images::more_vertical)
                .Fit(huxerui::ImageFit::Contain)
                .Tint(muted)
                .With(huxerui::Frame{.width = 20.0F, .height = 20.0F}),
        }
                         .With(menu.Anchor(),
                               huxerui::Padding(nativeVpn ? 6.0F : 10.0F),
                               huxerui::CornerRadius(islands.nested_radius),
                               huxerui::Tooltip(Localized("更多操作")),
                               huxerui::Focusable(true),
                               huxerui::Semantics{
                                   .role = huxerui::SemanticRole::Button,
                                   .label = huxerui::UseString(
                                       Localized("更多操作"))})
                         .OnClick([menu, buildMenuEntries] {
                             menu.Show(buildMenuEntries());
                         });
    }

    huxerui::View dragHandle =
        huxerui::Image(app::images::drag_indicator)
            .Fit(huxerui::ImageFit::Contain)
            .Tint(muted)
            .With(
                huxerui::Frame{.width = 18.0F, .height = 18.0F},
                huxerui::Padding(1.0F),
                huxerui::PointerCursor(huxerui::PointerCursorKind::Grab),
                huxerui::Tooltip(Localized("拖动排序")),
                huxerui::Focusable(false),
                huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                   .label = huxerui::UseString(
                                       Localized("拖动排序"))},
                huxerui::DragSource(
                    ProfileDragPayload{id},
                    std::function<huxerui::View()>{
                        [profile, compact, reorder, participating =
                             std::ranges::find(runtime.participatingSources,
                                               store::ProfileConnectionId(id)) !=
                             runtime.participatingSources.end()]() -> huxerui::View {
                            return ProfileDragPreview(profile, compact, participating,
                                reorder.cardWidth ? reorder.cardWidth(profile.id) : kCardWidth);
                        }
                    }
                )
            )
            .On<huxerui::DragSourceEvents::Started>(
                [isDragging, reorder, id](const huxerui::DragEvent&) {
                    isDragging = true;
                    if (reorder.begin) reorder.begin(id);
                })
            .On<huxerui::DragSourceEvents::Changed>(
                [reorder, id](const huxerui::DragEvent& event) {
                    if (reorder.movePointer) reorder.movePointer(id, event.window_position);
                })
            .On<huxerui::DragSourceEvents::Ended>(
                [isDragging, reorder, id](const huxerui::DragDropResult&) {
                    isDragging = false;
                    if (reorder.cancel) reorder.cancel(id);
                })
            .On<huxerui::DragSourceEvents::Canceled>(
                [isDragging, reorder, id](const huxerui::DragEvent&) {
                    isDragging = false;
                    if (reorder.cancel) reorder.cancel(id);
                });

    huxerui::View profileName =
        huxerui::Text(truncateOneLine(profile.name, compact ? 9 : 20)).Style(
            huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody)
                    .WithWeight(huxerui::FontWeight::SemiBold),
                fg});
    if (compact) profileName = std::move(profileName).With(huxerui::Grow(1.0F));

    const std::string primaryLine = [&] {
        if (!nativeVpn) {
            return profile.url.empty()
                       ? huxerui::UseString(Localized("本地导入"))
                       : profile.url;
        }
        std::string parseError;
        const auto config = pptp::ParsePptpConfig(profile.nativeConfig, parseError);
        return config ? "PPTP · " + config->server
                      : huxerui::UseString(Localized("PPTP 配置无效"));
    }();

    const bool hasTraffic =
        !nativeVpn && (profile.usedBytes > 0 || profile.totalBytes > 0);
    const bool hasExpiry = !nativeVpn && profile.expireAt > 0;
    const int percent = profile.totalBytes > 0
                            ? static_cast<int>(std::clamp(
                                  static_cast<double>(profile.usedBytes) /
                                      static_cast<double>(profile.totalBytes),
                                  0.0, 1.0) *
                              100.0)
                            : 0;

    huxerui::View trafficSection = huxerui::Row{};
    if (!nativeVpn) {
        std::vector<huxerui::View> subviews;
        if (hasTraffic || hasExpiry) {
            subviews.push_back(
                huxerui::Row {
                    hasTraffic
                        ? huxerui::View{huxerui::Text(
                              profile.totalBytes > 0
                                  ? LocalizedFormat(
                                        "{}% {} / {}",
                                        percent,
                                        formatBytes(profile.usedBytes),
                                        formatBytes(profile.totalBytes))
                                  : LocalizedFormat(
                                        "{} / 不限量",
                                        formatBytes(profile.usedBytes)))
                              .Style(huxerui::TextStyle{
                                  huxerui::Font::System(font_size::kCaption),
                                  muted})}
                        : huxerui::View{huxerui::Row{}},
                    huxerui::Spacer(),
                    hasExpiry
                        ? huxerui::View{huxerui::Text(
                              LocalizedFormat("到期时间 {}",
                                              formatDate(profile.expireAt)))
                              .Style(huxerui::TextStyle{
                                  huxerui::Font::System(font_size::kCaption),
                                  muted})}
                        : huxerui::View{huxerui::Row{}},
                }.With(huxerui::CrossAlign(
                    huxerui::CrossAxisAlignment::Center)));
        }
        subviews.push_back(
            huxerui::ProgressBar(std::clamp(
                profile.totalBytes > 0
                    ? static_cast<float>(profile.usedBytes) /
                          static_cast<float>(profile.totalBytes)
                    : 1.0F,
                0.0F, 1.0F))
                .With(huxerui::Frame{.height = 3.0F}));
        trafficSection = huxerui::Column(std::move(subviews))
                             .With(huxerui::Spacing(4.0F),
                                   huxerui::CrossAlign(
                                       huxerui::CrossAxisAlignment::Stretch));
    }

    if (profile.type == "pptp") {
        const bool loading = connectionPending ||
            pptpState.state == vpn::ConnectionState::Connecting;
        const bool connected = pptpState.state == vpn::ConnectionState::Connected;
        const auto actionLabel = loading ? Localized("加载中…")
                                        : connected ? Localized("关闭") : Localized("打开");
        huxerui::View actionGlyph = loading
            ? huxerui::View{huxerui::ProgressCircle()}
            : huxerui::View{huxerui::Image(connected ? app::images::pause : app::images::play)
                               .Fit(huxerui::ImageFit::Contain)
                               .Tint(muted)};
        actionGlyph = std::move(actionGlyph).With(huxerui::Frame{.width = 20.0F, .height = 20.0F});
        const auto connectionLabel = connectionPending
            ? PptpConnectionProgress(openingConnection) : pptpStateText(pptpState);
        const huxerui::TextStyle connectionStyle{huxerui::Font::System(font_size::kCaption),
            !connectionPending && pptpState.state == vpn::ConnectionState::Failed ? theme.colors.error : muted};
        huxerui::View connectionText = compact
            ? FittedSingleLineLabel(huxerui::UseString(connectionLabel) + " · " + primaryLine, connectionStyle)
            : huxerui::View{huxerui::Text(connectionLabel).Style(connectionStyle)};
        trafficSection = huxerui::Row {
          std::move(connectionText).With(huxerui::Frame{.height = 18.0F}, huxerui::Grow(1.0F),
                    huxerui::ClipChildren()),
          huxerui::Row {actionGlyph}
              .OnClick([togglePptpConnection, id] { togglePptpConnection(id); })
              .With(huxerui::Enabled(!loading && !connectionActionsBusy),
                    huxerui::Padding(6.0F),
                    huxerui::CornerRadius(islands.nested_radius),
                    huxerui::Tooltip(actionLabel),
                    huxerui::Focusable(true),
                    huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                       .label = huxerui::UseString(actionLabel)})
              .Key("pptp-connection-action"),
        }.With(huxerui::Spacing(8.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    }

    // 订阅卡复用代理页节点卡的形状原语（SelectableTile）：同一套内边距、圆角、
    // 表面色与选中态（选中 = primary_container 淡染底 + primary 描边 +
    // on_primary_container 文字）。不再自己叠描边或第二层选中背景。
    huxerui::View cardContent;
    if (compact) {
        huxerui::View busy;
        if (refreshBusy) busy = huxerui::View{huxerui::ProgressCircle()}.With(
            huxerui::Frame{.width = 12.0F, .height = 12.0F},
            huxerui::Tooltip(Localized("更新订阅")),
            huxerui::Semantics{.busy = true}).Key("profile-refresh-busy");
        cardContent = CompactProfileCardContent(profile.name,
            nativeVpn ? "" : huxerui::UseString(CompactProfileStatus(profile,
                runtime.state == core::CoreState::Running &&
                std::ranges::find(runtime.participatingSources, store::ProfileConnectionId(id)) !=
                    runtime.participatingSources.end())),
            tile, dragHandle, moreButton,
            nativeVpn ? trafficSection : CompactProfileQuota(profile, muted), nativeVpn, busy);
    } else {
        cardContent = huxerui::Column {
        huxerui::Row {
            std::move(dragHandle),
            std::move(profileName),
            huxerui::Spacer(),
            std::move(refreshButton),
            std::move(moreButton),
        }.With(huxerui::Spacing(6.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Text(truncateOneLine(primaryLine, 48))
            .Style(huxerui::TextStyle{huxerui::Font::Monospace(font_size::kChip),
                                      muted}),
        huxerui::Text(truncateOneLine(
                          nativeVpn
                              ? (profile.nativeRoutes.empty()
                                     ? huxerui::UseString(
                                           Localized("未设置内网路由"))
                                     : huxerui::UseString(LocalizedFormat(
                                           "路由：{}", profile.nativeRoutes)))
                              : (profile.description.empty() ? "—"
                                                              : profile.description),
                          52))
            .Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption),
                nativeVpn || profile.description.empty() ? faint : muted}),
        // 平台能力如实展示：Windows RAS 客户端只有一条 PPTP 会话槽位。
        profile.type == "pptp" && vpn::PptpSingleSession()
            ? huxerui::View{huxerui::Text(huxerui::UseString(Localized(
                                              "Windows 系统拨号同一时间仅支持一条 PPTP 连接")))
                                .Style(huxerui::TextStyle{
                                    huxerui::Font::System(font_size::kCaption),
                                    faint})}
            : huxerui::View{huxerui::Row{}},
        profile.type != "pptp" ? huxerui::View{huxerui::Row {
            huxerui::Text(nativeVpn
                              ? huxerui::StringVariant{"PPTP"}
                              : selected ? Localized("主订阅")
                              : (runtime.state == core::CoreState::Running &&
                                 std::ranges::find(runtime.participatingSources, store::ProfileConnectionId(id)) != runtime.participatingSources.end())
                                    ? Localized("次订阅 · 按规则参与") : Localized("已保存 · 未参与"))
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption), muted}),
            !nativeVpn && profile.autoUpdate && profile.intervalMins > 0
                ? huxerui::View{huxerui::Text(LocalizedFormat(
                                       "自动 {} 分钟", profile.intervalMins))
                                    .Style(huxerui::TextStyle{
                                        huxerui::Font::System(
                                            font_size::kCaption),
                                        muted})}
                : huxerui::View{huxerui::Row{}},
            huxerui::Spacer(),
            huxerui::Text(truncateOneLine(
                              nativeVpn ? ""
                                        : huxerui::UseString(
                                              FormatRelativeTime(profile.updatedAt)),
                              52))
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    muted}),
        }
            .With(huxerui::Spacing(8.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))} : huxerui::View{},
        huxerui::Spacer(),
        std::move(trafficSection),
    }.With(huxerui::Spacing(6.0F),
           huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }
    huxerui::View card = SelectableTile(cardContent,
        selected, profile.name,
        // 远程/本地代理订阅暂时保持单选：点击哪张卡片，哪张就是当前订阅。
        // PPTP 仍使用系统接口；OpenVPN 已由 sing-box endpoint 统一承载。
        [activateProfile, id, selected, nativeVpn, toast,
         hasKnownExhaustedQuota = HasKnownExhaustedQuota(profile),
         profileName = profile.name] {
            if (nativeVpn) {
                toast.Show(Localized(
                    "此连接配置用于内网连接，不能设为主订阅"));
                return;
            }
            if (selected) {
                if (hasKnownExhaustedQuota)
                    ShowKnownExhaustedQuota(toast, profileName);
                return;
            }
            activateProfile(id);
        }, nativeVpn ? SelectableTileInteraction::ReadOnly
                     : SelectableTileInteraction::Selectable);

    // 矩形卡统一高度；宽度由虚拟网格的轨道均分，铺满页面而不固定尺寸
    // （内容已单行截断，ClipChildren 兜底）。
    card = std::move(card).With(huxerui::Frame{.height = ProfileCardHeight(compact)},
                                huxerui::ClipChildren());
    if (isDragging.Get()) {
        card = std::move(card).With(huxerui::Opacity(0.4F));
    }
    return std::move(card)
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
