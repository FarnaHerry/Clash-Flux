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
import clashflux.persistence;
import clashflux.store.core;
import clashflux.store.profiles;
import clashflux.store.vpn;
import clashflux.stream;
import clashflux.vpn;

// 模型头用到模块类型（store::CoreSnapshot / db::Profile / store::*State），
// 必须在模块导入之后包含（同 profiles_cache.h 的约定）。
#include "core_model.h"
#include "profiles_cache.h"
#include "profiles_model.h"
#include "vpn_model.h"

namespace clashflux::ui {
namespace {

const std::vector<SectionTab> kRuleTabs{{"subscription", "订阅规则"},
                                        {"global", "全局路由"}};
const std::vector<std::string> kMatchKinds{
    "全部", "精确域名", "域名后缀", "精确 IP", "IPv4 网段"};

const std::vector<std::string> kTierNames{"来源分流", "全局覆盖"};
const std::vector<std::string> kUnavailableNames{"阻断", "主默认出口", "直连"};
const std::vector<std::string> kTargetKindNames{"默认出口", "策略组", "节点"};

huxerui::View RuleOptions(huxerui::State<std::size_t> tier,
    huxerui::State<std::size_t> unavailable, huxerui::State<std::size_t> kind,
    huxerui::State<huxerui::TextEditingValue> object, huxerui::State<bool> enabled) {
    return huxerui::Column {
      huxerui::Text("规则层级"),
      huxerui::Select(kTierNames, tier.Get(), [](const std::string& text) { return huxerui::Text(text); })
          .OnChanged([tier](std::size_t index) { tier = index; }),
      huxerui::Text("目标对象（原生连接使用默认出口）"),
      huxerui::Select(kTargetKindNames, kind.Get(), [](const std::string& text) { return huxerui::Text(text); })
          .OnChanged([kind](std::size_t index) { kind = index; }),
      kind.Get() == 0 ? huxerui::View{huxerui::Row {}} : huxerui::View{
          huxerui::TextField(object.Get()).Label("来源内的原始组名或节点名")
              .Variant(huxerui::TextFieldVariant::Outlined)
              .OnChanged([object](const huxerui::TextEditingValue& value) { object = value; })},
      huxerui::Text("目标不可用时"),
      huxerui::Select(kUnavailableNames, unavailable.Get(), [](const std::string& text) { return huxerui::Text(text); })
          .OnChanged([unavailable](std::size_t index) { unavailable = index; }),
      huxerui::Row {
        huxerui::Text("启用规则"),
        huxerui::Spacer(),
        huxerui::Switch(enabled.Get()).OnChanged([enabled](bool value) { enabled = value; }),
      }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
    }.With(huxerui::Spacing(8.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

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

std::string ConnectionName(const huxerui::StateList<db::Profile>& profiles,
                           std::string_view connectionId) {
    for (const db::Profile& profile : profiles) {
        if (store::ProfileConnectionId(profile.id) == connectionId) {
            return profile.name;
        }
    }
    return connectionId.empty() ? "未设置" : std::string(connectionId);
}

struct RuleSourceChoice {
    std::int64_t id; std::string name;
    bool operator==(const RuleSourceChoice&) const = default;
};
std::vector<RuleSourceChoice> TargetProfiles(const huxerui::StateList<db::Profile>& profiles) {
    std::vector<RuleSourceChoice> result;
    for (const auto& profile : profiles) {
        if (!vpn::SupportsMultiProxySources() && !profile.selected &&
            profile.type != "pptp" && profile.type != "openvpn") continue;
        result.push_back({profile.id, profile.name});
    }
    return result;
}
std::vector<std::string> TargetNames(const std::vector<RuleSourceChoice>& profiles) {
    std::vector<std::string> result;
    for (const auto& profile : profiles) result.push_back(profile.name);
    return result;
}

// 当前"生效中"的连接：主连接（内核在跑且订阅已选中）+ 已连上的原生连接。
// 纯函数、只读模型值——由 Lifecycle 以模型 State 为依赖驱动，不再每秒读 store。
std::vector<std::string> ActiveRuleConnections(
    const store::CoreSnapshot& core,
    const std::vector<db::Profile>& profiles,
    const std::vector<store::PptpState>& pptpStates,
    const std::vector<store::OpenVpnState>& openVpnStates) {
    static_cast<void>(profiles); static_cast<void>(pptpStates); static_cast<void>(openVpnStates);
    return core.state == core::CoreState::Running ? core.participatingSources : std::vector<std::string>{};
}

bool ValidateRuleInput(vpn::RouteRule& rule, std::string& error) {
    if (!vpn::ValidatePolicyRules(std::vector<vpn::RouteRule>{rule}, error)) return false;
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

[[huxerui::composable]] huxerui::View RulesPage(
    ProfilesCache profilesCache, std::function<void()> onBack, bool active) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    auto tasks = huxerui::UseTaskScope();
    auto dialog = huxerui::UseDialog();
    auto toast = huxerui::UseToast();
    // 共享数据一律来自 application service（见 *_model.h）。
    const auto coreModel = huxerui::UseService<CoreModel>();
    const auto profilesModel = huxerui::UseService<ProfilesModel>();
    const auto vpnModel = huxerui::UseService<VpnModel>();
    auto section = huxerui::UseState<std::size_t>(0);
    auto subscriptionRules = huxerui::UseStateList<SubscriptionRuleRow>();
    auto globalRules = huxerui::UseStateList<vpn::RouteRule>();
    // 订阅列表来自 ProfilesModel 的镜像（唯一来源见 profiles_model.h），
    // 本页不再自维护副本。
    auto profiles = profilesCache.list;
    auto activeConnections = huxerui::UseStateList<std::string>();
    auto refreshTick = huxerui::UseState(0);
    auto refreshSpin = huxerui::UseState(0);
    // 分区滑动切换的手势状态（处理器与阈值见 common.cpp SectionTabSwipeHandler）。
    auto swipeOrigin = huxerui::UseState<huxerui::Point>(huxerui::Point{0.0F, 0.0F});
    auto swipeOwned = huxerui::UseState(false);

    // 编辑器状态归页面持有，弹窗只负责渲染；不会在每一行里创建 hook。
    auto editMatch = huxerui::UseState<std::size_t>(1);
    auto editIndex = huxerui::UseState(-1);
    auto editPattern = huxerui::UseState(huxerui::TextEditingValue{});
    auto editTarget = huxerui::UseState<std::size_t>(0);
    auto editSources = huxerui::UseState<std::vector<RuleSourceChoice>>({});
    auto editPriority = huxerui::UseState(
        huxerui::TextEditingValue::FromText("100"));
    auto globalEditorOpen = huxerui::UseState(false);
    auto editTier = huxerui::UseState<std::size_t>(0);
    auto editUnavailable = huxerui::UseState<std::size_t>(0);
    auto editKind = huxerui::UseState<std::size_t>(0);
    auto editObject = huxerui::UseState(huxerui::TextEditingValue{});
    auto editEnabled = huxerui::UseState(true);
    auto policySaving = huxerui::UseState(false);
    auto loadGeneration = huxerui::UseState<std::uint64_t>(0);

    auto persistGlobalPolicy = [tasks, globalRules, toast, refreshTick, policySaving, loadGeneration, coreModel] {
        if (policySaving.Get()) return;
        policySaving = true;
        loadGeneration = loadGeneration.Get() + 1;
        vpn::VpnPolicy policy;
        policy.rules.reserve(globalRules.Size());
        for (const vpn::RouteRule& rule : globalRules) {
            policy.rules.push_back(rule);
        }
        tasks.Launch([policy = std::move(policy), toast, refreshTick, policySaving, coreModel]() mutable
                         -> huxerui::Task<void> {
            std::string error;
            bool ok = false;
            try { const auto outcome = co_await RunOnTaskThread([policy = std::move(policy),
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
                const bool saved = store::vpnStore().saveGlobalPolicy(std::move(policy), error);
                return std::pair{saved, store::coreStore().snapshot()};
            });
            ok = outcome.first;
            coreModel->Update([&](CoreView& view) { view.core = outcome.second; });
            } catch (const std::exception& e) { error = e.what(); }
            policySaving = false;
            toast.Show(ok ? "编排规则已保存"
                          : std::format("全局规则保存失败：{}", error));
            refreshTick = refreshTick.Get() + 1;
        });
    };

    // 规则加载：以「订阅列表 + 手动刷新计数」为依赖——订阅变化或用户点刷新时
    // 重载一次（订阅模型在 hydrate 完成后会发布，天然覆盖启动竞态），不再靠
    // 每秒轮询。存储读取在 worker，State 写回在 UI 线程。
    huxerui::Lifecycle(
        [tasks, subscriptionRules, globalRules, refreshTick, profilesModel, loadGeneration] {
            const auto ticket = loadGeneration.Get() + 1;
            loadGeneration = ticket;
            // 选中的主连接先从模型取（UI 线程、借用引用），再按值带进 worker，
            // 避免 worker 里再整表拷贝一次。
            std::string selectedMainId;
            for (const db::Profile& profile : profilesModel->list.Get()) {
                if (profile.selected && profile.type != "pptp" &&
                    profile.type != "openvpn") {
                    selectedMainId = store::ProfileConnectionId(profile.id);
                    break;
                }
            }
            tasks.Launch([subscriptionRules, globalRules,
                          selectedMainId, loadGeneration, ticket]() -> huxerui::Task<void> {
                const auto loaded = co_await RunOnTaskThread(
                    [selectedMainId] {
                        const auto rules = LoadSubscriptionRules();
                        auto policy = store::vpnStore().globalPolicy();
                        return std::tuple{rules, policy};
                    });
                if (loadGeneration.Get() != ticket) co_return;
                ReplaceStateList(subscriptionRules,
                                 std::move(std::get<0>(loaded)));
                const vpn::VpnPolicy& policy = std::get<1>(loaded);
                ReplaceStateList(globalRules, policy.rules);
            });
            return [] {};
        },
        profilesModel->list, refreshTick);

    // 生效中的连接：模型依赖驱动（内核状态 / 订阅列表 / 两条原生连接状态），
    // 不再是每秒一次 store 读取。
    huxerui::Lifecycle(
        [activeConnections, coreModel, profilesModel, vpnModel] {
            ReplaceStateList(
                activeConnections,
                ActiveRuleConnections(coreModel->view.Get().core,
                                      profilesModel->list.Get(),
                                      vpnModel->pptp.Get(),
                                      vpnModel->openvpn.Get()));
            return [] {};
        },
        coreModel->view, profilesModel->list, vpnModel->pptp, vpnModel->openvpn);

    // 不可见时只保留本页 State/Lifecycle，不构建内容：桌面 IndexedPages 让七个
    // 一级页同帧参与测量，隐藏页（日志/连接有推送流更新）的重子树会拖慢每一次渲染。
    if (!active) return huxerui::View{huxerui::Row{}}.Key("rules-idle");

    const auto mono = [](const std::string& text, huxerui::Color color) {
        return huxerui::Text(text).Style(huxerui::TextStyle{
            huxerui::Font::Monospace(font_size::kMonoBody), color});
    };

    auto openGlobalRuleEditor = [compact, dialog, profiles, editMatch, editPattern,
                                 editTarget, editPriority, globalRules,
                                 persistGlobalPolicy, globalEditorOpen, theme,
                                 editIndex, toast, editTier, editUnavailable, editKind,
                                 editObject, editEnabled, policySaving, editSources](int index) {
        if (policySaving.Get()) return;
        const auto targetProfiles = TargetProfiles(profiles);
        editSources = targetProfiles;
        editTier = 0; editUnavailable = 0; editKind = 0; editEnabled = true;
        editObject = huxerui::TextEditingValue{};
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
            editTier = rule.tier == vpn::RuleTier::UserOverride ? 1U : 0U;
            editUnavailable = static_cast<std::size_t>(rule.unavailable);
            editKind = static_cast<std::size_t>(rule.targetKind);
            editObject = huxerui::TextEditingValue::FromText(rule.targetObject);
            editEnabled = rule.enabled;
            editTarget = targetProfiles.size();
            for (std::size_t i = 0; i < targetProfiles.size(); ++i)
                if (store::ProfileConnectionId(targetProfiles[i].id) == rule.connectionId) editTarget = i;
        }
        if (compact) {
            globalEditorOpen = true;
            return;
        }
        dialog.Show(
            [profiles, editMatch, editPattern, editTarget, editPriority,
             globalRules, persistGlobalPolicy, theme, editIndex, toast, targetProfiles,
             editTier, editUnavailable, editKind, editObject, editEnabled, policySaving](huxerui::DialogContext ctx)
                -> huxerui::View {
                auto targets = TargetNames(targetProfiles);
                if (!targets.empty() && editTarget.Get() >= targets.size()) targets.push_back("目标来源已失效，请重新选择");
                return DialogCard(huxerui::ScrollView(
                    huxerui::Column {
                        huxerui::Text(editIndex.Get() < 0 ? "添加全局路由规则" : "编辑全局路由规则",
                                      huxerui::TextRole::Title),
                        huxerui::Text(
                            "显式分流先于主订阅；无规则引用的次来源不参与。URL 按主机名匹配。")
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
                            .Label("层内优先级（数字越大越先匹配）")
                            .Variant(huxerui::TextFieldVariant::Outlined)
                            .OnChanged([editPriority](
                                           const huxerui::TextEditingValue& value) {
                                editPriority = value;
                            }),
                        RuleOptions(editTier, editUnavailable, editKind, editObject, editEnabled),
                        huxerui::Row{
                            huxerui::Button("取消").OnClick(
                                [ctx] { ctx.Dismiss(); }),
                            huxerui::Button("保存").OnClick([=] {
                                if (policySaving.Get()) return;
                                if (targets.empty() || editTarget.Get() >= targetProfiles.size()) {
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
                                    std::min(editTarget.Get(), targetProfiles.size() - 1);
                                vpn::RouteRule rule{
                                    .match = *match,
                                    .pattern = *match == vpn::MatchKind::Any ? "" : Trim(editPattern.Get().text),
                                    .connectionId = store::ProfileConnectionId(
                                        targetProfiles[targetIndex].id),
                                    .priority = priority,
                                    .tier = editTier.Get() == 0 ? vpn::RuleTier::SourcePolicy : vpn::RuleTier::UserOverride,
                                    .enabled = editEnabled.Get(),
                                    .unavailable = static_cast<vpn::UnavailablePolicy>(editUnavailable.Get()),
                                    .targetKind = static_cast<vpn::TargetKind>(editKind.Get()),
                                    .targetObject = editKind.Get() == 0 ? "" : Trim(editObject.Get().text),
                                };
                                if (editIndex.Get() >= 0 && static_cast<std::size_t>(editIndex.Get()) < globalRules.Size())
                                    rule.id = globalRules[static_cast<std::size_t>(editIndex.Get())].id;
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
                                  huxerui::CrossAxisAlignment::Stretch)))
                    .With(huxerui::Frame{.max_width = 520.0F, .max_height = 640.0F}, huxerui::ScrollBar()));
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
                    // VirtualList 在测量阶段调用 factory，此时没有组合上下文。
                    // 文本解析等组合期操作延迟到行 Scope 挂载后执行。
                    return huxerui::Scope([=]() -> huxerui::View {
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
                });
            subscriptionList = std::move(subscriptionList)
                                   .EstimatedItemExtent(compact ? 86.0F : 42.0F)
                                   .With(huxerui::Grow(1.0F), huxerui::ScrollBar());
            // 分区组件默认支持滑动（见 ui.h）：列表区左右滑动切到「全局路由」。
            if (kSectionTabsSwipeDefault) {
                subscriptionList =
                    std::move(subscriptionList)
                        .On<huxerui::ViewEvents::PointerIntercept>(
                            SectionTabSwipeHandler(
                                swipeOrigin, swipeOwned, nullptr,
                                [section] { section = 1; }));
            }
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
                 persistGlobalPolicy, openGlobalRuleEditor, activeConnections, policySaving, coreModel](std::size_t index) -> huxerui::View {
                    // 与订阅规则相同：只在测量 factory 中声明 Scope。
                    return huxerui::Scope([=]() -> huxerui::View {
                      if (compact && index == count) {
                          return CompactFloatingNavigationFooter().Key("compact-floating-footer");
                      }
                      const auto& rule = globalRules[index];
                      const auto catalog = coreModel->view.Get().core.sourceObjects;
                      const bool objectExists = rule.targetKind == vpn::TargetKind::Default || (catalog &&
                          std::ranges::any_of(*catalog, [&](const auto& object) {
                              return object.sourceId == rule.connectionId && object.kind == rule.targetKind && object.objectId == rule.targetObject;
                          }));
                      const bool active = rule.enabled && objectExists && std::ranges::find(activeConnections, rule.connectionId) != activeConnections.end();
                      const auto key = rule.id.empty() ? "pending-rule-" + std::to_string(index) : rule.id;
                      return UnifiedListRow(
                          huxerui::Column {
                            mono(rule.match == vpn::MatchKind::Any ? "全部流量" : rule.pattern, theme.colors.on_surface),
                            mono(std::format("{} · {} · 优先级 {}", vpn::MatchKindName(rule.match),
                                             ConnectionName(profiles, rule.connectionId) + (rule.targetObject.empty() ? "" : " / " + rule.targetObject), rule.priority),
                                 theme.colors.on_surface_variant),
                            huxerui::Flow {
                              huxerui::Text(!rule.enabled ? "已禁用" : active ? "已参与编排" :
                                  "目标不可用/待应用 · " + kUnavailableNames[static_cast<std::size_t>(rule.unavailable)])
                                  .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption),
                                      active ? theme.colors.primary : theme.colors.on_surface_variant}),
                              huxerui::Text(rule.tier == vpn::RuleTier::UserOverride ? "全局覆盖" : "来源分流"),
                              huxerui::Button(rule.enabled ? "禁用" : "启用").With(huxerui::Enabled(!policySaving.Get()))
                                  .OnClick([globalRules, index, persistGlobalPolicy] {
                                      auto changed = globalRules[index]; changed.enabled = !changed.enabled;
                                      globalRules.Set(index, std::move(changed)); persistGlobalPolicy();
                                  }),
                              huxerui::Button("上移").OnClick([globalRules, index, persistGlobalPolicy, policySaving] {
                                  if (policySaving.Get() || index == 0) return;
                                  auto previous = globalRules[index - 1]; auto current = globalRules[index];
                                  globalRules.Set(index - 1, std::move(current)); globalRules.Set(index, std::move(previous));
                                  persistGlobalPolicy();
                              }).With(huxerui::Enabled(!policySaving.Get() && index > 0)),
                              huxerui::Button("下移").OnClick([globalRules, index, persistGlobalPolicy, policySaving] {
                                  if (policySaving.Get() || index + 1 >= globalRules.Size()) return;
                                  auto next = globalRules[index + 1]; auto current = globalRules[index];
                                  globalRules.Set(index + 1, std::move(current)); globalRules.Set(index, std::move(next));
                                  persistGlobalPolicy();
                              }).With(huxerui::Enabled(!policySaving.Get() && index + 1 < count)),
                              huxerui::Button("编辑").With(huxerui::Enabled(!policySaving.Get())).OnClick([openGlobalRuleEditor, index] {
                                  openGlobalRuleEditor(static_cast<int>(index));
                              }),
                              huxerui::Button("删除").With(huxerui::Enabled(!policySaving.Get())).OnClick([globalRules, index, persistGlobalPolicy] {
                                  if (index < globalRules.Size()) {
                                      globalRules.Erase(index);
                                      persistGlobalPolicy();
                                  }
                              }),
                            }.With(huxerui::Spacing(8.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
                          }.With(huxerui::Spacing(6.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
                          key, true, index + 1 < count);
                    });
                });
            globalList = std::move(globalList)
                             .EstimatedItemExtent(116.0F)
                             .With(huxerui::Grow(1.0F), huxerui::ScrollBar());
            // 分区组件默认支持滑动（见 ui.h）：列表区左右滑动切回「订阅规则」。
            if (kSectionTabsSwipeDefault) {
                globalList =
                    std::move(globalList)
                        .On<huxerui::ViewEvents::PointerIntercept>(
                            SectionTabSwipeHandler(
                                swipeOrigin, swipeOwned,
                                [section] { section = 0; }, nullptr));
            }
            body = huxerui::Column{std::move(globalList)}
                       .With(huxerui::Spacing(8.0F),
                             huxerui::CrossAlign(
                                 huxerui::CrossAxisAlignment::Stretch),
                             huxerui::Grow(1.0F));
        }
    }

    // 分区切换与代理页/订阅页共用同一下划线标签栏（SectionTabBar）。
    huxerui::View sectionSwitch = SectionTabBar(
        kRuleTabs, section.Get() == 0 ? "subscription" : "global",
        [section](const std::string& key) { section = key == "global" ? 1 : 0; });
    huxerui::View addRule =
        section.Get() == 1
            ? huxerui::View{
                  huxerui::IconButton(app::images::add, "添加规则")
                      .With(huxerui::Tooltip("添加规则"))
                      .OnClick([openGlobalRuleEditor] { openGlobalRuleEditor(-1); })}
            : huxerui::View{huxerui::Row{}};
    huxerui::View refresh =
        huxerui::IconButton(app::images::refresh, "刷新规则")
            .With(huxerui::Tooltip("刷新规则"),
                  huxerui::Rotation(huxerui::AnimateTo(
                      static_cast<float>(refreshSpin.Get()) * 360.0F,
                      huxerui::TweenSpec{.duration = 0.6,
                                         .easing = huxerui::Easing::Linear})))
            .OnClick([refreshTick, refreshSpin] {
                refreshSpin = refreshSpin.Get() + 1;
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
    const auto editorProfiles = editSources.Get();
    auto editorTargets = TargetNames(editorProfiles);
    if (!editorTargets.empty() && editTarget.Get() >= editorTargets.size()) editorTargets.push_back("目标来源已失效，请重新选择");
    const auto saveResponsiveRule = [=] {
        if (policySaving.Get()) return;
        if (editorTargets.empty() || editTarget.Get() >= editorProfiles.size()) {
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
            std::min(editTarget.Get(), editorProfiles.size() - 1);
        vpn::RouteRule rule{
            .match = *match,
            .pattern = *match == vpn::MatchKind::Any ? "" : Trim(editPattern.Get().text),
            .connectionId = store::ProfileConnectionId(
                editorProfiles[targetIndex].id),
            .priority = priority,
            .tier = editTier.Get() == 0 ? vpn::RuleTier::SourcePolicy : vpn::RuleTier::UserOverride,
            .enabled = editEnabled.Get(),
            .unavailable = static_cast<vpn::UnavailablePolicy>(editUnavailable.Get()),
            .targetKind = static_cast<vpn::TargetKind>(editKind.Get()),
            .targetObject = editKind.Get() == 0 ? "" : Trim(editObject.Get().text),
        };
        if (editIndex.Get() >= 0 && static_cast<std::size_t>(editIndex.Get()) < globalRules.Size())
            rule.id = globalRules[static_cast<std::size_t>(editIndex.Get())].id;
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
                        "显式分流先于主订阅；无规则引用的次来源不参与。URL 按主机名匹配。"),
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
                        .Label("层内优先级（数字越大越先匹配）")
                        .Variant(huxerui::TextFieldVariant::Outlined)
                        .OnChanged([editPriority](
                                       const huxerui::TextEditingValue& value) {
                            editPriority = value;
                        }),
                    RuleOptions(editTier, editUnavailable, editKind, editObject, editEnabled),
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
