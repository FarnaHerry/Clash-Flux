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

#include "ui.h"
#include "task_bridge.h"

import clashflux.db;
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
                     store::profilesStore().yamlOf(profile.id))) {
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

} // namespace

[[huxerui::composable]] huxerui::View RulesPage() {
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
    auto refreshTick = huxerui::UseState(0);

    // 编辑器状态归页面持有，弹窗只负责渲染；不会在每一行里创建 hook。
    auto editMatch = huxerui::UseState<std::size_t>(0);
    auto editPattern = huxerui::UseState(huxerui::TextEditingValue{});
    auto editTarget = huxerui::UseState<std::size_t>(0);
    auto editPriority = huxerui::UseState(
        huxerui::TextEditingValue::FromText("100"));

    auto persistGlobalPolicy = [tasks, globalRules, toast] {
        vpn::VpnPolicy policy;
        policy.rules.reserve(globalRules.Size());
        for (const vpn::RouteRule& rule : globalRules) {
            policy.rules.push_back(rule);
        }
        tasks.Launch([policy = std::move(policy), toast]() mutable
                         -> huxerui::Task<void> {
            std::string error;
            const bool ok = co_await RunOnTaskThread([policy = std::move(policy),
                                                       &error]() mutable {
                // 未命中的流量始终跟随当前启用的 mihomo 订阅；原生 PPTP/
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
        });
    };

    huxerui::Lifecycle(
        [tasks, subscriptionRules, globalRules, profiles, refreshTick] {
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
                    co_await huxerui::Delay(
                        std::chrono::duration<double>{0.25});
                }
            });
            return [] {};
        },
        0);

    const auto mono = [](const std::string& text, huxerui::Color color) {
        return huxerui::Text(text).Style(huxerui::TextStyle{
            huxerui::Font::Monospace(font_size::kMonoBody), color});
    };

    auto openGlobalRuleEditor = [dialog, profiles, editMatch, editPattern,
                                 editTarget, editPriority, globalRules,
                                 persistGlobalPolicy, theme] {
        editMatch = 0;
        editPattern = huxerui::TextEditingValue{};
        editTarget = 0;
        editPriority = huxerui::TextEditingValue::FromText("100");
        dialog.Show(
            [profiles, editMatch, editPattern, editTarget, editPriority,
             globalRules, persistGlobalPolicy, theme](huxerui::DialogContext ctx)
                -> huxerui::View {
                const std::vector<std::string> targets = TargetNames(profiles);
                return DialogCard(
                    huxerui::Column{
                        huxerui::Text("添加全局路由规则",
                                      huxerui::TextRole::Title),
                        huxerui::Text(
                            "按匹配类型把流量交给某个订阅连接；未命中时跟随当前订阅。")
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
                        huxerui::TextField(editPattern.Get())
                            .Label("匹配内容（全部类型可留空）")
                            .Variant(huxerui::TextFieldVariant::Outlined)
                            .OnChanged([editPattern](
                                           const huxerui::TextEditingValue& value) {
                                editPattern = value;
                            }),
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
                                if (targets.empty()) return;
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
                                    return;
                                }
                                const auto match = vpn::ParseMatchKind(
                                    kMatchKinds[editMatch.Get()]);
                                if (!match.has_value()) return;
                                const std::size_t targetIndex =
                                    std::min(editTarget.Get(), profiles.Size() - 1);
                                vpn::RouteRule rule{
                                    .match = *match,
                                    .pattern = Trim(editPattern.Get().text),
                                    .connectionId = store::ProfileConnectionId(
                                        profiles[targetIndex].id),
                                    .priority = priority,
                                };
                                if (rule.match == vpn::MatchKind::Any) {
                                    rule.pattern.clear();
                                } else if (rule.pattern.empty()) {
                                    return;
                                }
                                globalRules.PushBack(std::move(rule));
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
                    if (compact) {
                        // 紧凑窗口不再复用桌面表格的固定列宽。固定列会把
                        // VirtualList 的最小宽度推过页面岛，导致右侧内容和
                        // 操作区一起溢出屏幕；卡片字段可以在有限宽度内换行。
                        return huxerui::Column{
                                   mono(rule.profile, theme.colors.primary),
                                   mono(std::format("{} · 走向：{}", rule.type,
                                                    rule.target),
                                        theme.colors.on_surface_variant),
                                   mono(payload, theme.colors.on_surface),
                               }
                            .With(huxerui::Spacing(4.0F),
                                  huxerui::Padding(
                                      huxerui::EdgeInsets::Symmetric(8.0F, 6.0F)),
                                  huxerui::Background(
                                      theme.colors.surface_container_high),
                                  huxerui::CornerRadius(12.0F),
                                  huxerui::ClipChildren(),
                                  huxerui::CrossAlign(
                                      huxerui::CrossAxisAlignment::Stretch))
                            .Key(std::format("{}:{}:{}", rule.profileId,
                                             rule.ordinal, rule.type));
                    }
                    return huxerui::Row{
                               mono(rule.profile, theme.colors.primary)
                                   .With(huxerui::Frame{.width = 180.0F}),
                               mono(rule.type, theme.colors.on_surface)
                                   .With(huxerui::Frame{.width = 145.0F}),
                               mono(payload, theme.colors.on_surface)
                                   .With(huxerui::Grow(1.0F)),
                               mono(rule.target, theme.colors.on_surface_variant)
                                   .With(huxerui::Frame{.width = 180.0F}),
                           }
                        .With(huxerui::Spacing(8.0F),
                              huxerui::Padding(
                                  huxerui::EdgeInsets::Symmetric(4.0F, 5.0F)))
                        .Key(std::format("{}:{}:{}", rule.profileId,
                                         rule.ordinal, rule.type));
                });
            subscriptionList = std::move(subscriptionList)
                                   .EstimatedItemExtent(compact ? 76.0F : 28.0F)
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
                 persistGlobalPolicy](std::size_t index) -> huxerui::View {
                    if (compact && index == count) {
                        return CompactFloatingNavigationFooter()
                            .Key("compact-floating-footer");
                    }
                    const vpn::RouteRule& rule = globalRules[index];
                    const std::string pattern =
                        rule.pattern.empty() ? "全部" : rule.pattern;
                    const std::string connection =
                        ConnectionName(profiles, rule.connectionId);
                    if (compact) {
                        return huxerui::Column{
                                   mono(std::format(
                                            "{} · 优先级：{}",
                                            vpn::MatchKindName(rule.match),
                                            rule.priority),
                                        theme.colors.primary),
                                   mono(std::format("匹配：{}", pattern),
                                        theme.colors.on_surface),
                                   mono(std::format("连接：{}", connection),
                                        theme.colors.on_surface_variant),
                                   huxerui::Row{
                                       huxerui::Spacer(),
                                       huxerui::Button("删除").OnClick(
                                           [globalRules, index,
                                            persistGlobalPolicy] {
                                               if (index < globalRules.Size()) {
                                                   globalRules.Erase(index);
                                                   persistGlobalPolicy();
                                               }
                                           }),
                                   }
                                       .With(huxerui::CrossAlign(
                                           huxerui::CrossAxisAlignment::Stretch)),
                               }
                            .With(huxerui::Spacing(4.0F),
                                  huxerui::Padding(
                                      huxerui::EdgeInsets::Symmetric(8.0F, 6.0F)),
                                  huxerui::Background(
                                      theme.colors.surface_container_high),
                                  huxerui::CornerRadius(12.0F),
                                  huxerui::ClipChildren(),
                                  huxerui::CrossAlign(
                                      huxerui::CrossAxisAlignment::Stretch))
                            .Key(std::format("{}:{}:{}", rule.connectionId,
                                             rule.pattern, rule.priority));
                    }
                    return huxerui::Row{
                               mono(std::string(vpn::MatchKindName(rule.match)),
                                    theme.colors.primary)
                                   .With(huxerui::Frame{.width = 125.0F}),
                               mono(pattern, theme.colors.on_surface)
                                   .With(huxerui::Grow(1.0F)),
                               mono(connection, theme.colors.on_surface_variant)
                                   .With(huxerui::Frame{.width = 180.0F}),
                               mono(std::to_string(rule.priority),
                                    theme.colors.on_surface_variant)
                                   .With(huxerui::Frame{.width = 70.0F}),
                               huxerui::Button("删除").OnClick(
                                   [globalRules, index, persistGlobalPolicy] {
                                       if (index < globalRules.Size()) {
                                           globalRules.Erase(index);
                                           persistGlobalPolicy();
                                       }
                                   }),
                           }
                        .With(huxerui::Spacing(8.0F),
                              huxerui::Padding(
                                  huxerui::EdgeInsets::Symmetric(4.0F, 5.0F)))
                        .Key(std::format("{}:{}:{}", rule.connectionId,
                                         rule.pattern, rule.priority));
                });
            globalList = std::move(globalList)
                             .EstimatedItemExtent(compact ? 112.0F : 34.0F)
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
                  huxerui::Button("添加规则").OnClick(openGlobalRuleEditor)}
            : huxerui::View{huxerui::Row{}};
    huxerui::View refresh = huxerui::Button("刷新").OnClick([refreshTick] {
        refreshTick = refreshTick.Get() + 1;
    });
    huxerui::View actions;
    if (compact) {
        // Flow 受父级有限宽度约束，按钮不足一行时自动换行；不要再用
        // Spacer + 固定 Row 把两个按钮推到岛屿右边界之外。
        huxerui::View compactActions;
        if (section.Get() == 1) {
            compactActions = huxerui::Flow{
                                 std::move(addRule), std::move(refresh)}
                .With(huxerui::Spacing(8.0F),
                      huxerui::MainAlign(huxerui::MainAxisAlignment::End),
                      huxerui::CrossAlign(
                          huxerui::CrossAxisAlignment::Center));
        } else {
            compactActions = huxerui::Flow{std::move(refresh)}.With(
                huxerui::MainAlign(huxerui::MainAxisAlignment::End),
                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
        }
        actions = huxerui::Column{
                      std::move(sectionSwitch), std::move(compactActions)}
                      .With(huxerui::Spacing(8.0F),
                            huxerui::CrossAlign(
                                huxerui::CrossAxisAlignment::Stretch));
    } else {
        actions = huxerui::Row{
                      std::move(sectionSwitch),
                      huxerui::Spacer(),
                      std::move(addRule),
                      std::move(refresh),
                  }
                      .With(huxerui::Spacing(8.0F),
                            huxerui::CrossAlign(
                                huxerui::CrossAxisAlignment::Center));
    }

    return PageScaffold("规则", std::move(actions), std::move(body));
}

} // namespace clashflux::ui
