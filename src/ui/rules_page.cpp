// rules_page.cpp — 多重规则页。
//
// 订阅规则是每个 Profile 自己携带的 rules/nativeRoutes；全局规则是独立
// 持久化的 VpnPolicy，用来把目标交给具体订阅连接。两种列表都使用
// StateList + VirtualList，避免大订阅在重组时复制整张表。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "app_resources.h"
#include "ui.h"
#include "task_bridge.h"

import clashflux.db;
import clashflux.core;
import clashflux.store.core;
import clashflux.store.profiles;
import clashflux.store.vpn;
import clashflux.vpn;

namespace clashflux::ui {
namespace {

const std::vector<huxerui::StringVariant> kRuleSections{"订阅规则", "全局路由"};
const std::vector<std::string> kMatchKinds{
    "全部", "精确域名", "域名后缀", "精确 IP", "IPv4 网段"};

struct SubscriptionRuleRow {
    std::int64_t profileId = 0;
    std::string profile;
    std::string type;
    std::string payload;
    std::string target;
    std::size_t ordinal = 0;

    bool operator==(const SubscriptionRuleRow&) const = default;
};

std::string Trim(std::string value) {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

SubscriptionRuleRow ParseSubscriptionRule(const db::Profile& profile,
                                           std::string raw,
                                           std::size_t ordinal) {
    raw = Trim(std::move(raw));
    const std::size_t first = raw.find(',');
    const std::size_t second = first == std::string::npos
                                   ? std::string::npos
                                   : raw.find(',', first + 1);
    const std::size_t third = second == std::string::npos
                                  ? std::string::npos
                                  : raw.find(',', second + 1);
    const std::string type = Trim(raw.substr(
        0, first == std::string::npos ? std::string::npos : first));
    const std::string payload =
        first == std::string::npos
            ? ""
            : Trim(raw.substr(first + 1,
                              second == std::string::npos
                                  ? std::string::npos
                                  : second - first - 1));
    const std::string target =
        second == std::string::npos
            ? ""
            : Trim(raw.substr(second + 1,
                              third == std::string::npos
                                  ? std::string::npos
                                  : third - second - 1));
    return SubscriptionRuleRow{
        .profileId = profile.id,
        .profile = profile.name,
        .type = type.empty() ? "规则" : type,
        .payload = payload,
        .target = target,
        .ordinal = ordinal,
    };
}

std::vector<std::string> SplitRoutes(const std::string& text) {
    std::vector<std::string> routes;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t end = text.find_first_of(",\n", begin);
        const std::string route = Trim(text.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin));
        if (!route.empty()) routes.push_back(route);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return routes;
}

std::vector<SubscriptionRuleRow> LoadSubscriptionRules() {
    std::vector<SubscriptionRuleRow> rows;
    const std::vector<db::Profile> profiles = store::profilesStore().list();
    for (const db::Profile& profile : profiles) {
        std::size_t ordinal = 0;
        if (profile.type == "pptp" || profile.type == "openvpn") {
            for (const std::string& route : SplitRoutes(profile.nativeRoutes)) {
                rows.push_back({
                    .profileId = profile.id,
                    .profile = profile.name,
                    .type = "IPv4 网段",
                    .payload = route,
                    .target = "本连接",
                    .ordinal = ordinal++,
                });
            }
        } else {
            for (const std::string& rule : store::parseRules(
                     store::profilesStore().yamlOf(profile))) {
                rows.push_back(ParseSubscriptionRule(profile, rule, ordinal++));
            }
        }
        // 空规则也保留订阅组，用户能明确看到“该订阅没有内置规则”，而不是
        // 误以为订阅没有加载成功。
        if (ordinal == 0) {
            rows.push_back({
                .profileId = profile.id,
                .profile = profile.name,
                .type = "—",
                .payload = "未配置内置规则",
                .target = (profile.type == "pptp" || profile.type == "openvpn")
                              ? "内网路由"
                              : "规则为空",
                .ordinal = 0,
            });
        }
    }
    return rows;
}

std::vector<db::Profile> LoadProfiles() {
    return store::profilesStore().list();
}

std::string ConnectionName(const huxerui::StateList<db::Profile>& profiles,
                           std::string_view connectionId) {
    for (const db::Profile& profile : profiles) {
        if (store::ProfileConnectionId(profile.id) == connectionId) {
            return profile.name;
        }
    }
    return connectionId.empty() ? "未设置" : std::string(connectionId);
}

std::vector<std::string> TargetNames(
    const huxerui::StateList<db::Profile>& profiles) {
    std::vector<std::string> names;
    names.reserve(profiles.Size());
    for (const db::Profile& profile : profiles) names.push_back(profile.name);
    return names;
}

std::vector<std::string> ActiveRuleConnections() {
    std::vector<std::string> ids;
    if (store::coreStore().snapshot().state == core::CoreState::Running) {
        for (const auto& profile : LoadProfiles()) {
            if (profile.selected && profile.type != "pptp" && profile.type != "openvpn")
                ids.push_back(store::ProfileConnectionId(profile.id));
        }
    }
    for (const auto& state : store::vpnStore().states()) {
        if (state.state == vpn::ConnectionState::Connected && !state.interfaceName.empty())
            ids.push_back(store::ProfileConnectionId(state.profileId));
    }
    for (const auto& state : store::vpnStore().openVpnStates()) {
        if (state.state == vpn::ConnectionState::Connected && !state.interfaceName.empty())
            ids.push_back(store::ProfileConnectionId(state.profileId));
    }
    return ids;
}

bool ValidateRuleInput(vpn::RouteRule& rule, std::string& error) {
    if (rule.match == vpn::MatchKind::Any) {
        if (!rule.pattern.empty()) {
            error = "指定了匹配内容，请选择精确域名、域名后缀或 IP 类型";
            return false;
        }
    } else if (rule.match == vpn::MatchKind::ExactDomain ||
               rule.match == vpn::MatchKind::DomainSuffix) {
        const auto domain = vpn::NormalizeRuleDomain(rule.pattern);
        if (!domain) {
            error = "请输入有效域名或 HTTP/HTTPS URL";
            return false;
        }
        rule.pattern = *domain;
    } else if (rule.pattern.empty()) {
        error = "匹配内容不能为空";
        return false;
    }
    return true;
}

} // namespace

[[huxerui::composable]] huxerui::View RulesPage(std::function<void()> onBack) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    auto tasks = huxerui::UseTaskScope();
    auto dialog = huxerui::UseDialog();
    auto toast = huxerui::UseToast();
    auto section = huxerui::UseState<std::size_t>(0);
    auto subscriptionRules = huxerui::UseStateList<SubscriptionRuleRow>();
    auto globalRules = huxerui::UseStateList<vpn::RouteRule>();
    auto profiles = huxerui::UseStateList<db::Profile>();
    auto activeConnections = huxerui::UseStateList<std::string>();
    auto refreshTick = huxerui::UseState(0);

    // 编辑器状态归页面持有，弹窗只负责渲染；不会在每一行里创建 hook。
    auto editMatch = huxerui::UseState<std::size_t>(1);
    auto editIndex = huxerui::UseState(-1);
    auto editPattern = huxerui::UseState(huxerui::TextEditingValue{});
    auto editTarget = huxerui::UseState<std::size_t>(0);
    auto editPriority = huxerui::UseState(
        huxerui::TextEditingValue::FromText("100"));
    auto globalEditorOpen = huxerui::UseState(false);

    auto persistGlobalPolicy = [tasks, globalRules, toast, refreshTick] {
        vpn::VpnPolicy policy;
        policy.rules.reserve(globalRules.Size());
        for (const vpn::RouteRule& rule : globalRules) {
            policy.rules.push_back(rule);
        }
        tasks.Launch([policy = std::move(policy), toast, refreshTick]() mutable
                         -> huxerui::Task<void> {
            std::string error;
            const bool ok = co_await RunOnTaskThread([policy = std::move(policy),
                                                       &error]() mutable {
                // 未命中的流量始终跟随当前启用的代理订阅；原生 PPTP/
                // OpenVPN 只通过全局路由规则接管自己的目标网段。
                for (const db::Profile& profile : store::profilesStore().list()) {
                    if (profile.selected && profile.type != "pptp" &&
                        profile.type != "openvpn") {
                        policy.defaultMainId =
                            store::ProfileConnectionId(profile.id);
                        break;
                    }
                }
                return store::vpnStore().saveGlobalPolicy(std::move(policy),
                                                          error);
            });
            toast.Show(ok ? "全局规则已保存"
                          : std::format("全局规则保存失败：{}", error));
            refreshTick = refreshTick.Get() + 1;
        });
    };

    huxerui::Lifecycle(
        [tasks, subscriptionRules, globalRules, profiles, refreshTick, activeConnections] {
            tasks.Launch([=]() -> huxerui::Task<void> {
                int lastTick = -1;
                for (;;) {
                    if (refreshTick.Get() != lastTick) {
                        lastTick = refreshTick.Get();
                        const auto loaded = co_await RunOnTaskThread([] {
                            const auto rules = LoadSubscriptionRules();
                            const auto allProfiles = LoadProfiles();
                            auto policy = store::vpnStore().globalPolicy();
                            std::string selectedMainId;
                            for (const auto& profile : allProfiles) {
                                if (profile.selected && profile.type != "pptp" &&
                                    profile.type != "openvpn") {
                                    selectedMainId =
                                        store::ProfileConnectionId(profile.id);
                                    break;
                                }
                            }
                            if (policy.defaultMainId != selectedMainId) {
                                policy.defaultMainId = std::move(selectedMainId);
                                std::string ignored;
                                store::vpnStore().saveGlobalPolicy(policy,
                                                                    ignored);
                            }
                            return std::tuple{rules, allProfiles, policy};
                        });
                        ReplaceStateList(subscriptionRules,
                                         std::move(std::get<0>(loaded)));
                        ReplaceStateList(profiles,
                                         std::move(std::get<1>(loaded)));
                        const vpn::VpnPolicy& policy = std::get<2>(loaded);
                        ReplaceStateList(globalRules, policy.rules);
                    }
                    const auto active = co_await RunOnTaskThread(ActiveRuleConnections);
                    ReplaceStateList(activeConnections, active);
                    co_await huxerui::Delay(
                        std::chrono::duration<double>{1.0});
                }
            });
            return [] {};
        },
        0);

    const auto mono = [](const std::string& text, huxerui::Color color) {
        return huxerui::Text(text).Style(huxerui::TextStyle{
            huxerui::Font::Monospace(font_size::kMonoBody), color});
    };

    auto openGlobalRuleEditor = [compact, dialog, profiles, editMatch, editPattern,
                                 editTarget, editPriority, globalRules,
                                 persistGlobalPolicy, globalEditorOpen, theme,
                                 editIndex, toast](int index) {
        editIndex = index;
        editMatch = 1;
        editPattern = huxerui::TextEditingValue{};
        editTarget = 0;
        editPriority = huxerui::TextEditingValue::FromText("100");
        if (index >= 0 && static_cast<std::size_t>(index) < globalRules.Size()) {
            const auto& rule = globalRules[static_cast<std::size_t>(index)];
            for (std::size_t i = 0; i < kMatchKinds.size(); ++i)
                if (kMatchKinds[i] == vpn::MatchKindName(rule.match)) editMatch = i;
            editPattern = huxerui::TextEditingValue::FromText(rule.pattern);
            editPriority = huxerui::TextEditingValue::FromText(std::to_string(rule.priority));
            editTarget = profiles.Size();
            for (std::size_t i = 0; i < profiles.Size(); ++i)
                if (store::ProfileConnectionId(profiles[i].id) == rule.connectionId) editTarget = i;
        }
        if (compact) {
            globalEditorOpen = true;
            return;
        }
        dialog.Show(
            [profiles, editMatch, editPattern, editTarget, editPriority,
             globalRules, persistGlobalPolicy, theme, editIndex, toast](huxerui::DialogContext ctx)
                -> huxerui::View {
                const std::vector<std::string> targets = TargetNames(profiles);
                return DialogCard(
                    huxerui::Column{
                        huxerui::Text(editIndex.Get() < 0 ? "添加全局路由规则" : "编辑全局路由规则",
                                      huxerui::TextRole::Title),
                        huxerui::Text(
                            "目标连接未打开时暂停规则；URL 按主机名匹配，不区分路径。")
                            .Style(huxerui::TextStyle{
                                huxerui::Font::System(font_size::kCaption),
                                theme.colors.on_surface_variant}),
                        huxerui::Select(
                            kMatchKinds, editMatch.Get(),
                            [](const std::string& value) {
                                return huxerui::Text(value);
                            })
                            .OnChanged([editMatch](std::size_t index) {
                                editMatch = index;
                            })
                            .With(huxerui::Frame{.width = 220.0F}),
                        editMatch.Get() == 0 ? huxerui::View{huxerui::Row{}} :
                        huxerui::View{huxerui::TextField(editPattern.Get())
                            .Label("匹配域名、URL 或 IP")
                            .Variant(huxerui::TextFieldVariant::Outlined)
                            .OnChanged([editPattern](
                                           const huxerui::TextEditingValue& value) {
                                editPattern = value;
                            })},
                        targets.empty()
                            ? huxerui::View{
                                  huxerui::Text("请先创建一个订阅连接")}
                            : huxerui::View{huxerui::Select(
                                  targets, editTarget.Get(),
                                  [](const std::string& value) {
                                      return huxerui::Text(value);
                                  })
                                  .OnChanged([editTarget](std::size_t index) {
                                      editTarget = index;
                                  })},
                        huxerui::TextField(editPriority.Get())
                            .Label("优先级（数字越大越先匹配）")
                            .Variant(huxerui::TextFieldVariant::Outlined)
                            .OnChanged([editPriority](
                                           const huxerui::TextEditingValue& value) {
                                editPriority = value;
                            }),
                        huxerui::Row{
                            huxerui::Button("取消").OnClick(
                                [ctx] { ctx.Dismiss(); }),
                            huxerui::Button("保存").OnClick([=] {
                                if (targets.empty() || editTarget.Get() >= profiles.Size()) {
                                    toast.Show("请选择目标连接");
                                    return;
                                }
                                int priority = 0;
                                const std::string priorityText =
                                    editPriority.Get().text;
                                const auto parsed = std::from_chars(
                                    priorityText.data(),
                                    priorityText.data() + priorityText.size(),
                                    priority);
                                if (parsed.ec != std::errc() ||
                                    parsed.ptr !=
                                        priorityText.data() + priorityText.size()) {
                                    toast.Show("优先级必须是整数");
                                    return;
                                }
                                const auto match = vpn::ParseMatchKind(
                                    kMatchKinds[editMatch.Get()]);
                                if (!match.has_value()) return;
                                const std::size_t targetIndex =
                                    std::min(editTarget.Get(), profiles.Size() - 1);
                                vpn::RouteRule rule{
                                    .match = *match,
                                    .pattern = *match == vpn::MatchKind::Any ? "" : Trim(editPattern.Get().text),
                                    .connectionId = store::ProfileConnectionId(
                                        profiles[targetIndex].id),
                                    .priority = priority,
                                };
                                std::string validationError;
                                if (!ValidateRuleInput(rule, validationError)) {
                                    toast.Show(validationError);
                                    return;
                                }
                                if (editIndex.Get() >= 0 &&
                                    static_cast<std::size_t>(editIndex.Get()) < globalRules.Size())
                                    globalRules.Set(static_cast<std::size_t>(editIndex.Get()), std::move(rule));
                                else globalRules.PushBack(std::move(rule));
                                ctx.Dismiss();
                                persistGlobalPolicy();
                            }),
                        }
                            .With(huxerui::MainAlign(
                                huxerui::MainAxisAlignment::SpaceBetween)),
                    }
                        .With(huxerui::Spacing(12.0F),
                              huxerui::CrossAlign(
                                  huxerui::CrossAxisAlignment::Stretch)));
            },
            huxerui::DialogOptions{});
    };

    huxerui::View body;
    if (section.Get() == 0) {
        const std::size_t count = subscriptionRules.Size();
        body = count == 0
                   ? huxerui::View{huxerui::Text("还没有订阅规则")
                                       .Style(huxerui::TextStyle{
                                           huxerui::Font::System(font_size::kBody),
                                           theme.colors.on_surface_variant})}
                   : huxerui::View{};
        if (count != 0) {
            auto subscriptionList = huxerui::VirtualList(
                count + (compact ? 1U : 0U),
                [subscriptionRules, mono, theme, compact, count](
                    std::size_t index) -> huxerui::View {
                    if (compact && index == count) {
                        return CompactFloatingNavigationFooter()
                            .Key("compact-floating-footer");
                    }
                    const SubscriptionRuleRow& rule = subscriptionRules[index];
                    const std::string payload =
                        rule.payload.empty() ? "—" : rule.payload;
                    const std::string key = std::format(
                        "{}:{}:{}", rule.profileId, rule.ordinal, rule.type);
                    if (compact) {
                        // 紧凑窗口不再复用桌面表格的固定列宽。固定列会把
                        // VirtualList 的最小宽度推过页面岛，导致右侧内容和
                        // 操作区一起溢出屏幕；卡片字段可以在有限宽度内换行。
                        return UnifiedListRow(
                            huxerui::Column{
                                mono(rule.profile, theme.colors.primary),
                                mono(std::format("{} · 走向：{}", rule.type,
                                                 rule.target),
                                     theme.colors.on_surface_variant),
                                mono(payload, theme.colors.on_surface),
                            },
                            key, true, index + 1 < count);
                    }
                    return UnifiedListRow(
                        huxerui::Row{
                            mono(rule.profile, theme.colors.primary)
                                .With(huxerui::Frame{.width = 180.0F}),
                            mono(rule.type, theme.colors.on_surface)
                                .With(huxerui::Frame{.width = 145.0F}),
                            mono(payload, theme.colors.on_surface)
                                .With(huxerui::Grow(1.0F)),
                            mono(rule.target, theme.colors.on_surface_variant)
                                .With(huxerui::Frame{.width = 180.0F}),
                        },
                        key, false, index + 1 < count);
                });
            subscriptionList = std::move(subscriptionList)
                                   .EstimatedItemExtent(compact ? 86.0F : 42.0F)
                                   .With(huxerui::Grow(1.0F), huxerui::ScrollBar());
            if (compact) {
                body = huxerui::Column{std::move(subscriptionList)}
                           .With(huxerui::CrossAlign(
                                     huxerui::CrossAxisAlignment::Stretch),
                                 huxerui::Grow(1.0F));
            } else {
                body = huxerui::Column{
                           huxerui::Row{
                               mono("订阅", theme.colors.on_surface_variant)
                                   .With(huxerui::Frame{.width = 180.0F}),
                               mono("类型", theme.colors.on_surface_variant)
                                   .With(huxerui::Frame{.width = 145.0F}),
                               mono("匹配内容", theme.colors.on_surface_variant)
                                   .With(huxerui::Grow(1.0F)),
                               mono("订阅走向", theme.colors.on_surface_variant)
                                   .With(huxerui::Frame{.width = 180.0F}),
                           }
                               .With(huxerui::Spacing(8.0F),
                                     huxerui::Padding(
                                         huxerui::EdgeInsets::Symmetric(4.0F,
                                                                         2.0F))),
                           huxerui::Divider(), std::move(subscriptionList)}
                    .With(huxerui::Spacing(4.0F),
                          huxerui::CrossAlign(
                              huxerui::CrossAxisAlignment::Stretch),
                          huxerui::Grow(1.0F));
            }
        }
    } else {
        const std::size_t count = globalRules.Size();
        if (count == 0) {
            body = huxerui::Column{
                       huxerui::Text("还没有全局路由规则")
                           .Style(huxerui::TextStyle{
                               huxerui::Font::System(font_size::kBody),
                               theme.colors.on_surface_variant}),
                   }
                       .With(huxerui::CrossAlign(
                                 huxerui::CrossAxisAlignment::Stretch),
                             huxerui::Grow(1.0F));
        } else {
            auto globalList = huxerui::VirtualList(
                count + (compact ? 1U : 0U),
                [globalRules, profiles, mono, theme, compact, count,
                 persistGlobalPolicy, openGlobalRuleEditor, activeConnections](std::size_t index) -> huxerui::View {
                    if (compact && index == count) {
                        return CompactFloatingNavigationFooter().Key("compact-floating-footer");
                    }
                    const auto& rule = globalRules[index];
                    const bool active = std::ranges::find(activeConnections, rule.connectionId) != activeConnections.end();
                    const auto key = std::format("{}:{}:{}:{}:{}", index, static_cast<int>(rule.match),
                                                 rule.connectionId, rule.pattern, rule.priority);
                    return UnifiedListRow(
                        huxerui::Column {
                          mono(rule.match == vpn::MatchKind::Any ? "全部流量" : rule.pattern, theme.colors.on_surface),
                          mono(std::format("{} · {} · 优先级 {}", vpn::MatchKindName(rule.match),
                                           ConnectionName(profiles, rule.connectionId), rule.priority),
                               theme.colors.on_surface_variant),
                          huxerui::Flow {
                            huxerui::Text(active ? "已生效" : "已暂停 · 目标连接未打开")
                                .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption),
                                    active ? theme.colors.primary : theme.colors.on_surface_variant}),
                            huxerui::Button("编辑").OnClick([openGlobalRuleEditor, index] {
                                openGlobalRuleEditor(static_cast<int>(index));
                            }),
                            huxerui::Button("删除").OnClick([globalRules, index, persistGlobalPolicy] {
                                if (index < globalRules.Size()) {
                                    globalRules.Erase(index);
                                    persistGlobalPolicy();
                                }
                            }),
                          }.With(huxerui::Spacing(8.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
                        }.With(huxerui::Spacing(6.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
                        key, true, index + 1 < count);
                });
            globalList = std::move(globalList)
                             .EstimatedItemExtent(116.0F)
                             .With(huxerui::Grow(1.0F), huxerui::ScrollBar());
            body = huxerui::Column{std::move(globalList)}
                       .With(huxerui::Spacing(8.0F),
                             huxerui::CrossAlign(
                                 huxerui::CrossAxisAlignment::Stretch),
                             huxerui::Grow(1.0F));
        }
    }

    huxerui::View sectionSwitch = huxerui::SegmentedButton(kRuleSections, section)
                                      .OnChanged([section](std::size_t index) {
                                          section = index;
                                      });
    huxerui::View addRule =
        section.Get() == 1
            ? huxerui::View{
                  huxerui::IconButton(app::images::add, "添加规则")
                      .With(huxerui::Tooltip("添加规则"))
                      .OnClick([openGlobalRuleEditor] { openGlobalRuleEditor(-1); })}
            : huxerui::View{huxerui::Row{}};
    huxerui::View refresh = huxerui::IconButton(app::images::refresh, "刷新规则")
        .With(huxerui::Tooltip("刷新规则"))
        .OnClick([refreshTick] {
        refreshTick = refreshTick.Get() + 1;
    });
    huxerui::View actions = huxerui::Row{
        std::move(addRule), std::move(refresh),
    }.With(huxerui::Spacing(8.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    body = huxerui::Column {
        std::move(sectionSwitch), std::move(body),
    }.With(huxerui::Spacing(8.0F), huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    huxerui::View listPage = onBack
        ? SecondaryPageScaffold(huxerui::Text("规则", huxerui::TextRole::Title), std::move(actions), std::move(body), onBack)
        : PageScaffold("规则", std::move(actions), std::move(body));
    const std::vector<std::string> editorTargets = TargetNames(profiles);
    const auto saveResponsiveRule = [=] {
        if (editorTargets.empty() || editTarget.Get() >= profiles.Size()) {
            toast.Show("请选择目标连接");
            return;
        }
        int priority = 0;
        const std::string priorityText = editPriority.Get().text;
        const auto parsed = std::from_chars(
            priorityText.data(), priorityText.data() + priorityText.size(),
            priority);
        if (parsed.ec != std::errc() ||
            parsed.ptr != priorityText.data() + priorityText.size()) {
            toast.Show("优先级必须是整数");
            return;
        }
        const auto match = vpn::ParseMatchKind(kMatchKinds[editMatch.Get()]);
        if (!match) return;
        const std::size_t targetIndex =
            std::min(editTarget.Get(), profiles.Size() - 1);
        vpn::RouteRule rule{
            .match = *match,
            .pattern = *match == vpn::MatchKind::Any ? "" : Trim(editPattern.Get().text),
            .connectionId = store::ProfileConnectionId(
                profiles[targetIndex].id),
            .priority = priority,
        };
        std::string validationError;
        if (!ValidateRuleInput(rule, validationError)) {
            toast.Show(validationError);
            return;
        }
        if (editIndex.Get() >= 0 &&
            static_cast<std::size_t>(editIndex.Get()) < globalRules.Size())
            globalRules.Set(static_cast<std::size_t>(editIndex.Get()), std::move(rule));
        else globalRules.PushBack(std::move(rule));
        persistGlobalPolicy();
        globalEditorOpen = false;
    };
    // 编辑器页在 IndexedPages 中常驻挂载；未打开时必须保持为空，否则它与
    // 桌面弹窗绑定同一组受控 State，输入法组合值会被另一实例当作外部权威值。
    huxerui::View editorPage = !globalEditorOpen.Get()
        ? huxerui::View{huxerui::Row{}}
        : PageScaffold(
        editIndex.Get() < 0 ? "添加全局路由规则" : "编辑全局路由规则",
        huxerui::Row {
            huxerui::IconButton(app::images::arrow_back, "返回")
                .With(huxerui::Tooltip("返回规则列表"))
                .OnClick([globalEditorOpen] { globalEditorOpen = false; }),
            huxerui::IconButton(app::images::save, "保存规则")
                .With(huxerui::Tooltip("保存规则"))
                .OnClick(saveResponsiveRule),
        }.With(huxerui::Spacing(8.0F)),
        huxerui::ScrollView(
            huxerui::Column {
                Card(huxerui::Column {
                    huxerui::Text(
                        "目标连接未打开时暂停规则；URL 按主机名匹配，不区分路径。"),
                    huxerui::Select(
                        kMatchKinds, editMatch.Get(),
                        [](const std::string& value) { return huxerui::Text(value); })
                        .OnChanged([editMatch](std::size_t index) {
                            editMatch = index;
                        }),
                    editMatch.Get() == 0 ? huxerui::View{huxerui::Row{}} :
                    huxerui::View{huxerui::TextField(editPattern.Get())
                        .Label("匹配域名、URL 或 IP")
                        .Variant(huxerui::TextFieldVariant::Outlined)
                        .OnChanged([editPattern](
                                       const huxerui::TextEditingValue& value) {
                            editPattern = value;
                        })},
                    editorTargets.empty()
                        ? huxerui::View{huxerui::Text("请先创建一个订阅连接")}
                        : huxerui::View{huxerui::Select(
                              editorTargets, editTarget.Get(),
                              [](const std::string& value) {
                                  return huxerui::Text(value);
                              })
                              .OnChanged([editTarget](std::size_t index) {
                                  editTarget = index;
                              })},
                    huxerui::TextField(editPriority.Get())
                        .Label("优先级（数字越大越先匹配）")
                        .Variant(huxerui::TextFieldVariant::Outlined)
                        .OnChanged([editPriority](
                                       const huxerui::TextEditingValue& value) {
                            editPriority = value;
                        }),
                }.With(huxerui::Spacing(12.0F),
                       huxerui::CrossAlign(
                           huxerui::CrossAxisAlignment::Stretch))),
                CompactFloatingNavigationFooter(),
            }.With(huxerui::Spacing(12.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
            .With(huxerui::Grow(1.0F)));
    if (compact && globalEditorOpen.Get()) {
        editorPage = std::move(editorPage).On<huxerui::ViewEvents::BackRequested>(
            [globalEditorOpen] { globalEditorOpen = false; });
    }
    return huxerui::IndexedPages(
               std::vector<huxerui::View>{listPage, editorPage},
               compact && globalEditorOpen.Get() ? 1U : 0U)
        .With(huxerui::Grow(1.0F));
}

} // namespace clashflux::ui
