// rules_page.cpp — 多重规则页。
//
// 订阅规则是每个 Profile 自己携带的 rules/nativeRoutes；全局规则是独立
// 持久化的 VpnPolicy，用来把目标交给具体订阅连接。两种列表都使用
// 不可变订阅快照 / 可编辑 StateList + VirtualList，筛选索引按内容修订缓存。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "app_resources.h"
#include "ui.h"
#include "empty_state.h"
#include "page_loading.h"
#include "search_text.h"
#include "filtered_indices.h"
#include "task_bridge.h"
#include "rule_target_picker.h"
#include "action_menu.h"
#include "profile_file_picker.h"
#include "text_document.h"

import clashflux.db;
import clashflux.config;
import clashflux.singbox;
import clashflux.core;
import clashflux.persistence;
import clashflux.store.core;
import clashflux.store.profiles;
import clashflux.store.vpn;
import clashflux.stream;
import clashflux.vpn;
import clashflux.routing;

// 模型头用到模块类型（store::CoreSnapshot / db::Profile / store::*State），
// 必须在模块导入之后包含（同 profiles_cache.h 的约定）。
#include "core_model.h"
#include "profiles_cache.h"
#include "profiles_model.h"
#include "vpn_model.h"

namespace clashflux::ui {
namespace {

const std::vector<SectionTab> kRuleTabs{
    {"subscription", Localized("订阅规则")},
    {"global", Localized("全局规则")},
    {"resources", Localized("规则集")}};
const std::vector<std::string> kMatchKinds{
    "全部", "精确域名", "域名后缀", "精确 IP", "IPv4 网段", "规则集"};

const std::vector<std::string> kTierNames{"来源分流", "全局覆盖"};
const std::vector<std::string> kUnavailableNames{"阻断", "主默认出口", "直连"};

std::string RuleSetName(const singbox::RuleSetLibrary& library, const std::string& id) {
    const auto resource = std::ranges::find(library.resources, id, &singbox::PersonalRuleSet::id);
    return resource == library.resources.end() ? id : resource->name;
}

[[huxerui::composable]] huxerui::View GlobalRulePatternField(
    huxerui::State<std::size_t> match, huxerui::State<huxerui::TextEditingValue> pattern) {
    const auto model = huxerui::UseService<ProfilesModel>();
    const auto library = model->ruleSetLibrary.Get();
    std::vector<std::string> names;
    auto selected = library->resources.size();
    for (std::size_t i = 0; i < library->resources.size(); ++i) {
        names.push_back(library->resources[i].name);
        if (library->resources[i].id == pattern.Get().text) selected = i;
    }
    if (selected == names.size()) names.push_back(huxerui::UseString(Localized("请选择规则集")));
    return match.Get() == 0 ? huxerui::View{huxerui::Row{}} :
        match.Get() == 5 ? huxerui::View{huxerui::Select(names, selected,
            [](const auto& name) { return huxerui::Text(name); }).OnChanged([=](std::size_t index) {
                if (index < library->resources.size()) pattern = huxerui::TextEditingValue::FromText(library->resources[index].id);
            }).With(huxerui::Enabled(model->ruleSetsLoaded.Get()))} :
        huxerui::View{huxerui::TextField(pattern.Get()).Label(Localized("匹配域名、URL 或 IP"))
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([pattern](const auto& value) { pattern = value; })};
}

huxerui::View RuleOptions(huxerui::State<std::size_t> tier,
    huxerui::State<std::size_t> unavailable, huxerui::State<std::size_t> kind,
    huxerui::State<huxerui::TextEditingValue> object, huxerui::State<bool> enabled,
    huxerui::State<RuleTargetCatalog> catalog,
    huxerui::State<huxerui::TextEditingValue> search) {
    return huxerui::Column {
      huxerui::Text(Localized("规则层级")),
      huxerui::Select(kTierNames, tier.Get(), [](const std::string& text) { return huxerui::Text(Localized(text)); })
          .OnChanged([tier](std::size_t index) { tier = index; }),
      RuleTargetPicker(catalog, kind, object, search),
      huxerui::Text(Localized("目标不可用时")),
      huxerui::Select(kUnavailableNames, unavailable.Get(), [](const std::string& text) { return huxerui::Text(Localized(text)); })
          .OnChanged([unavailable](std::size_t index) { unavailable = index; }),
      huxerui::Row {
        huxerui::Text(Localized("启用规则")),
        huxerui::Spacer(),
        huxerui::Switch(enabled.Get()).OnChanged([enabled](bool value) { enabled = value; }),
      }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
    }.With(huxerui::Spacing(8.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

std::string Trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? "" : value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
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
            profile.type != "pptp") continue;
        result.push_back({profile.id, profile.name});
    }
    return result;
}
std::vector<std::string> TargetNames(const std::vector<RuleSourceChoice>& profiles) {
    std::vector<std::string> result;
    for (const auto& profile : profiles) result.push_back(profile.name);
    return result;
}

// Editor inventory is read-only and independent of both running and preview plans.
// Inactive Clash subscriptions are inspectable without downloading providers.
RuleTargetCatalog LoadRuleTargetCatalog(std::int64_t id) {
    RuleTargetCatalog catalog{.profileId = id, .ready = true};
    if (!persistence::persistence().ready()) {
        catalog.error = "订阅尚未加载完成"; return catalog;
    }
    const auto profiles = store::profilesStore().list();
    const auto profile = std::ranges::find_if(profiles, [id](const auto& item) { return item.id == id; });
    if (profile == profiles.end()) {
        catalog.error = "目标来源已失效，请重新选择"; return catalog;
    }
    if (!vpn::SupportsMultiProxySources() || profile->type == "pptp") {
        catalog.defaultOnly = true; return catalog;
    }
    const auto root = std::filesystem::canonical(cfg::dataDir() / "profiles");
    const auto relative = std::filesystem::path(profile->file);
    if (relative.empty() || relative.is_absolute()) {
        catalog.error = "订阅文件路径无效"; return catalog;
    }
    const auto path = std::filesystem::canonical(root / relative);
    const auto within = path.lexically_relative(root);
    if (within.empty() || *within.begin() == "..") {
        catalog.error = "订阅文件路径越界"; return catalog;
    }
    auto content = singbox::ReadRuleProviderText(path, 8 * 1024 * 1024, catalog.error);
    if (!content) return catalog;
    const auto trimmed = Trim(*content);
    if (singbox::DetectProfileFormat(trimmed) == singbox::ProfileFormat::SingBoxJson) {
        catalog.defaultOnly = true; return catalog;
    }
    singbox::CompileOptions options;
    options.profileYaml = std::move(*content);
    options.mainConnectionId = store::ProfileConnectionId(id);
    options.mainSourceName = profile->name;
    const auto inspected = singbox::inspectProfileSourceObjects(options);
    catalog.error = inspected.error;
    if (catalog.error.empty()) {
        for (const auto& object : inspected.sourceObjects) {
            // DIRECT is a built-in, not a user-declared node. Direct has its own
            // unavailable policy; do not present it as a subscription object.
            if (object.objectId == "DIRECT") continue;
            catalog.objects.push_back({static_cast<std::size_t>(object.kind), object.objectId});
        }
        std::ranges::sort(catalog.objects, [](const auto& a, const auto& b) {
            return a.kind == b.kind ? a.name < b.name : a.kind < b.kind;
        });
    }
    return catalog;
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

struct ProfileRulesSnapshot {
    std::int64_t id = 0;
    std::string content, error;
    singbox::ProfileRuleEdits edits;
    singbox::RuleSetLibrary library;
    singbox::ProfileRules original, effective;
    std::vector<std::string> targets;
    bool selected = false, supported = false;
};
struct PersonalRuleDraft {
    singbox::ProfileRuleEdit edit;
    huxerui::TextEditingValue pattern;
    std::size_t match = 0, position = 0, target = 0, unavailable = 0, resource = 0;
};
const std::vector<std::string> kPersonalMatches{
    "domain_suffix", "domain", "ip_cidr", "domain_keyword", "domain_regex", "process_name", "rule_set"};
const std::vector<std::string> kPersonalMatchLabels{
    "域名后缀", "精确域名", "IP / CIDR", "域名关键词", "域名正则", "进程名称", "规则集"};
const std::vector<std::string> kPersonalPositions{"订阅规则之前", "订阅兜底之前"};
const std::vector<std::string> kPersonalViews{"修改后的规则", "订阅原规则", "个人修改记录"};

ProfileRulesSnapshot LoadPersonalRules(std::int64_t id) {
    ProfileRulesSnapshot result{.id = id};
    auto lock = store::coreStore().lockPlanLifecycle();
    const auto profiles = store::profilesStore().list();
    const auto found = std::ranges::find(profiles, id, &db::Profile::id);
    if (found == profiles.end()) { result.error = "订阅不存在"; return result; }
    result.selected = found->selected;
    if (found->type == "pptp") {
        for (const auto& route : routing::SplitRoutes(found->nativeRoutes))
            result.original.rules.push_back({.type = "ip_cidr", .pattern = route, .target = found->name});
        result.effective = result.original;
        return result;
    }
    result.content = store::profilesStore().yamlOf(*found);
    result.supported = singbox::DetectProfileFormat(result.content) != singbox::ProfileFormat::OpenVpn;
    if (!result.supported) return result;
    result.edits = store::profilesStore().ruleEdits(id);
    result.library = store::coreStore().ruleSetLibrary();
    result.effective = singbox::InspectProfileRules(result.content, result.edits);
    result.original.rules = result.effective.original;
    result.error = result.effective.error;
    result.targets = {"DIRECT", "REJECT", "REJECT-DROP"};
    singbox::CompileOptions options;
    options.profileYaml = result.content;
    options.mainConnectionId = store::ProfileConnectionId(id);
    options.mainSourceName = found->name;
    auto objects = singbox::DetectProfileFormat(result.content) == singbox::ProfileFormat::SingBoxJson
        ? singbox::compileConfig(options) : singbox::inspectProfileSourceObjects(options);
    for (const auto& object : objects.sourceObjects)
        if (std::ranges::find(result.targets, object.objectId) == result.targets.end())
            result.targets.push_back(object.objectId);
    return result;
}

[[huxerui::composable]] huxerui::View PersonalRuleForm(
    huxerui::State<PersonalRuleDraft> draft, std::vector<std::string> targets,
    singbox::RuleSetLibrary library, huxerui::State<bool> busy) {
    const auto value = draft.Get();
    const bool final = value.edit.operation == "final";
    std::vector<std::string> resourceNames;
    for (const auto& resource : library.resources) resourceNames.push_back(resource.name);
    if (value.resource >= resourceNames.size() && value.edit.match == "rule_set" && !value.edit.pattern.empty())
        resourceNames.push_back(value.edit.pattern);
    return huxerui::Column{
        huxerui::Text(Localized("个人修改独立保存，更新订阅后保留。")),
        final ? huxerui::View{huxerui::Text(Localized("默认出口"))} : huxerui::View{huxerui::Column{
            huxerui::Select(kPersonalMatchLabels, value.match, [](const auto& text) { return huxerui::Text(Localized(text)); })
                .OnChanged([draft](std::size_t index) { auto v = draft.Get(); v.match = index; draft = v; }),
            value.match == 6 ? (resourceNames.empty() ? huxerui::View{huxerui::Text(Localized("请先在规则集页签添加资源"))} :
                huxerui::View{huxerui::Select(resourceNames, value.resource, [](const auto& text) { return huxerui::Text(text); })
                    .OnChanged([draft](std::size_t index) { auto v = draft.Get(); v.resource = index; draft = v; })}) :
            huxerui::View{huxerui::TextField(value.pattern).Label(Localized("匹配内容"))
                .Variant(huxerui::TextFieldVariant::Outlined)
                .OnChanged([draft](const huxerui::TextEditingValue& text) { auto v = draft.Get(); v.pattern = text; draft = v; })},
        }.With(huxerui::Spacing(12.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))},
        huxerui::Text(Localized("出口")),
        huxerui::Select(targets, value.target, [](const auto& text) { return huxerui::Text(text); })
            .OnChanged([draft](std::size_t index) { auto v = draft.Get(); v.target = index; draft = v; }),
        value.edit.operation == "prepend" || value.edit.operation == "before_final" ? huxerui::View{huxerui::Column{
            huxerui::Text(Localized("插入位置")),
            huxerui::Select(kPersonalPositions, value.position, [](const auto& text) { return huxerui::Text(Localized(text)); })
                .OnChanged([draft](std::size_t index) { auto v = draft.Get(); v.position = index; draft = v; }),
        }.With(huxerui::Spacing(8.0F))} : huxerui::View{huxerui::Row{}},
        huxerui::Text(Localized("目标不可用时")),
        huxerui::Select(final ? std::vector<std::string>{"阻断", "直连"} :
            std::vector<std::string>{"阻断", "直连", "主默认出口"}, value.unavailable,
            [](const auto& text) { return huxerui::Text(Localized(text)); })
            .OnChanged([draft](std::size_t index) { auto v = draft.Get(); v.unavailable = index; draft = v; }),
        huxerui::Row{
            huxerui::Text(Localized("启用规则")), huxerui::Spacer(),
            huxerui::Switch(value.edit.enabled).OnChanged([draft](bool enabled) {
                auto v = draft.Get(); v.edit.enabled = enabled; draft = v;
            }),
        },
    }.With(huxerui::Spacing(16.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch), huxerui::Enabled(!busy.Get()));
}

[[huxerui::composable]] huxerui::View PersonalRuleEditorPage(
    huxerui::State<PersonalRuleDraft> draft, std::vector<std::string> targets,
    singbox::RuleSetLibrary library, huxerui::State<bool> busy, std::function<void(std::function<void()>)> save) {
    const auto navigation = huxerui::UseNavigation();
    auto alive = huxerui::UseState(true);
    huxerui::Lifecycle([alive] { return [alive] { alive = false; }; }, 0);
    auto back = [navigation, busy, alive] { if (alive.Get() && !busy.Get()) static_cast<void>(navigation.Pop()); };
    return SecondaryPageScaffold(huxerui::Text(Localized("编辑订阅规则")),
        huxerui::IconButton(app::images::save, Localized("保存规则"))
            .With(huxerui::Enabled(!busy.Get())).OnClick([save, back] { save(back); }),
        huxerui::ScrollView(PersonalRuleForm(draft, targets, library, busy))
            .With(huxerui::Grow(1.0F), huxerui::ScrollBar()), back);
}

[[huxerui::composable]] huxerui::View PersonalRuleRow(
    singbox::ProfileRule rule, bool editable, bool busy, std::string displayPattern,
    std::function<void(singbox::ProfileRule)> edit,
    std::function<void(singbox::ProfileRule, std::string)> change) {
    const auto& theme = huxerui::UseTheme();
    auto menu = UseActionMenu();
    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    const auto source = !rule.enabled || rule.origin == "disabled" ? Localized("已禁用") : rule.origin == "added" ? Localized("个人新增") :
        rule.origin == "replaced" ? Localized("已替换") : Localized("订阅原有");
    const auto kind = std::ranges::find(kPersonalMatches, rule.type);
    const auto label = rule.terminal ? huxerui::UseString(Localized("默认出口")) :
        kind != kPersonalMatches.end() ? huxerui::UseString(Localized(kPersonalMatchLabels[static_cast<std::size_t>(kind - kPersonalMatches.begin())])) : rule.type;
    const auto detail = rule.terminal ? label : label + " · " + displayPattern;
    auto actions = [=] {
        std::vector<ActionMenuEntry> entries;
        entries.push_back(ActionMenuItem(Localized("编辑"), [edit, rule] { edit(rule); })
            .Enabled(editable && rule.editable && !busy));
        if (rule.editId.empty() && !rule.terminal) entries.push_back(ActionMenuItem(Localized("禁用"),
            [change, rule] { change(rule, "disable"); }).Enabled(editable && rule.editable && !busy));
        if (!rule.editId.empty()) {
            entries.push_back(ActionMenuItem(Localized(rule.enabled ? "禁用" : "启用"),
                [change, rule] { change(rule, "toggle"); }).Enabled(editable && !busy));
            entries.push_back(ActionMenuItem(Localized(rule.origin == "added" ? "删除" : "恢复订阅原规则"),
                [change, rule] { change(rule, "remove"); }).Enabled(editable && !busy));
            if (rule.origin == "added" && !rule.terminal) {
                entries.push_back(ActionMenuItem(Localized("上移"), [change, rule] { change(rule, "up"); }).Enabled(!busy));
                entries.push_back(ActionMenuItem(Localized("下移"), [change, rule] { change(rule, "down"); }).Enabled(!busy));
            }
        }
        entries.push_back(ActionMenuItem(Localized("查看详情"), [change, rule] { change(rule, "details"); }));
        menu.Show(std::move(entries));
    };
    auto content = huxerui::Column{
        huxerui::Text(detail).Style({huxerui::Font::System(font_size::kBody),
            rule.enabled ? theme.colors.on_surface : theme.colors.on_surface_variant}),
        huxerui::Text(rule.target).Style({huxerui::Font::System(font_size::kCaption), theme.colors.primary}),
    }.With(huxerui::Spacing(4.0F), huxerui::Grow(1.0F));
    return DenseCard(huxerui::Row{
        std::move(content),
        huxerui::Column{
            huxerui::Text(source),
            huxerui::Text(rule.position == "prepend" ? Localized("订阅规则之前") :
                rule.position == "before_final" ? Localized("订阅兜底之前") :
                !rule.editable ? Localized("仅查看") : huxerui::StringVariant("")),
        }.With(huxerui::Frame{.width = compact ? 96.0F : 150.0F}),
        huxerui::IconButton(app::images::more_vertical, Localized("规则操作"))
            .With(menu.Anchor()).OnClick(actions),
    }.With(huxerui::Spacing(8.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)))
        .With(huxerui::Padding(huxerui::EdgeInsets{.bottom = ResolveIslandTheme(theme).dense_radius}));
}

using RuleSetResource = std::shared_ptr<const singbox::PersonalRuleSet>;

struct RuleSetDraft {
    RuleSetResource resource;
    huxerui::TextEditingValue name, url, hours;
    TextDocumentHandle content;
    std::size_t type = 0, format = 0;
};

[[huxerui::composable]] huxerui::View RuleSetForm(
    huxerui::State<RuleSetDraft> draft, huxerui::State<bool> busy) {
    const auto value = draft.Get();
    auto tasks = huxerui::UseTaskScope();
    auto ioTasks = huxerui::UseApplicationTaskScope();
    auto toast = huxerui::UseToast();
    auto picker = huxerui::UseService<huxerui::FilePicker>();
    auto alive = huxerui::UseState(true);
    huxerui::Lifecycle([alive] { return [alive] { alive = false; }; }, 0);
    const bool supportsSrs = core::SupportsRuleSetTools();
    const huxerui::FilePickerFilter filter{.name = "sing-box rule set",
        .extensions = supportsSrs ? std::vector<std::string>{"json", "srs"} : std::vector<std::string>{"json"}};
    auto chooseFile = [=] {
        tasks.Launch([=]() -> huxerui::Task<void> {
            const auto picked = co_await PickProfileConfigFile(picker, filter, toast);
            if (!picked || !alive.Get()) co_return;
            const auto file = picked->AsFile();
            if (!file) { toast.Show(Localized("无法读取规则集文件")); co_return; }
            busy = true;
            ioTasks.Launch([=, path = file->Path()]() -> huxerui::Task<void> {
                std::pair<std::string, std::string> result;
                try {
                    result = co_await RunOnTaskThread([path] {
                        std::string error;
                        const auto file = std::filesystem::path(std::u8string(path.begin(), path.end()));
                        auto extension = file.extension().string();
                        std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        const auto text = extension == ".srs" ? core::importRuleSet(cfg::singboxBinary(), cfg::coreWorkDir(), file, error) :
                            singbox::ReadRuleProviderText(file, 8 * 1024 * 1024, error);
                        return std::pair{text.value_or(""), error};
                    });
                } catch (const std::exception& exception) { result.second = exception.what(); }
                busy = false;
                if (!alive.Get()) co_return;
                if (!result.second.empty()) { toast.Show(result.second); co_return; }
                auto edited = draft.Get();
                edited.content = std::make_shared<TextDocument>(std::move(result.first));
                draft = std::move(edited);
            });
        });
    };
    return huxerui::Column{
        huxerui::Text(Localized("添加后，在订阅规则或全局规则中选择集合与出口。")),
        huxerui::TextField(value.name).Label(Localized("规则集名称"))
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([draft](const auto& text) { auto v = draft.Get(); v.name = text; draft = v; }),
        huxerui::Select(std::vector<std::string>{"远程规则集", "本地 / 手写 JSON"}, value.type,
            [](const auto& text) { return huxerui::Text(Localized(text)); })
            .OnChanged([draft](std::size_t index) {
                auto v = draft.Get();
                if (v.content) static_cast<void>(v.content->Snapshot());
                v.type = index;
                draft = std::move(v);
            }),
        value.type == 0 ? huxerui::View{huxerui::Column{
            huxerui::TextField(value.url).Label(Localized("规则集地址"))
                .Placeholder("https://...").Variant(huxerui::TextFieldVariant::Outlined)
                .OnChanged([draft](const auto& text) { auto v = draft.Get(); v.url = text; draft = v; }),
            huxerui::Select(std::vector<std::string>{"SRS（二进制）", "JSON（源格式）"}, value.format,
                [](const auto& text) { return huxerui::Text(Localized(text)); })
                .OnChanged([draft](std::size_t index) { auto v = draft.Get(); v.format = index; draft = v; }),
            huxerui::TextField(value.hours).Label(Localized("更新周期（小时）"))
                .Variant(huxerui::TextFieldVariant::Outlined)
                .OnChanged([draft](const auto& text) { auto v = draft.Get(); v.hours = text; draft = v; }),
            huxerui::Text(Localized("引用后由内核直连下载并定时更新。")),
            huxerui::Text(Localized("保存地址不代表下载成功。")),
        }.With(huxerui::Spacing(12.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))} :
        huxerui::View{huxerui::Column{
            huxerui::Button(supportsSrs ? Localized("导入 JSON / SRS 文件") : Localized("导入 JSON 文件")).OnClick(chooseFile),
            huxerui::Text(Localized("规则集 JSON")),
            DocumentTextView(value.content, false, 300.0F),
            huxerui::Text(supportsSrs ? Localized("SRS 由固定内核转换为可编辑 JSON。") : Localized("仅接受 sing-box 规则集 JSON。")),
            huxerui::Text(Localized("导入内容独立保存，不修改原文件。")),
        }.With(huxerui::Spacing(12.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))},
    }.With(huxerui::Spacing(16.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch), huxerui::Enabled(!busy.Get()));
}

[[huxerui::composable]] huxerui::View RuleSetEditorPage(
    huxerui::State<RuleSetDraft> draft, huxerui::State<bool> busy,
    std::function<void(std::function<void()>)> save) {
    const auto navigation = huxerui::UseNavigation();
    auto alive = huxerui::UseState(true);
    huxerui::Lifecycle([alive] { return [alive] { alive = false; }; }, 0);
    const auto back = [=] { if (alive.Get() && !busy.Get()) static_cast<void>(navigation.Pop()); };
    return SecondaryPageScaffold(huxerui::Text(Localized("编辑规则集")),
        huxerui::IconButton(app::images::save, Localized("保存"))
            .With(huxerui::Enabled(!busy.Get())).OnClick([=] { save(back); }),
        huxerui::ScrollView(RuleSetForm(draft, busy)).With(huxerui::Grow(1.0F), huxerui::ScrollBar()), back);
}

[[huxerui::composable]] huxerui::View RuleSetResourceRow(
    RuleSetResource resource, bool busy, std::string receipt,
    std::function<void(RuleSetResource)> edit,
    std::function<void(std::string)> remove, std::function<void(std::string)> refresh) {
    auto menu = UseActionMenu();
    return DenseCard(huxerui::Row{
        huxerui::Column{
            huxerui::Text(resource->name),
            huxerui::Text(resource->type == "remote" ? Localized("远程规则集") : Localized("本地 / 手写 JSON")),
            huxerui::Text(resource->type == "remote" ? resource->format + " · " +
                std::to_string(resource->update_hours) + "h" : "JSON"),
            receipt.empty() ? huxerui::View{huxerui::Row{}} : huxerui::View{huxerui::Text(receipt)},
        }.With(huxerui::Spacing(4.0F), huxerui::Grow(1.0F)),
        huxerui::IconButton(app::images::more_vertical, Localized("规则集操作"))
            .With(menu.Anchor()).OnClick([=] {
                std::vector<ActionMenuEntry> actions{
                    ActionMenuItem(Localized("编辑"), [=] { edit(resource); }).Enabled(!busy)};
                if (resource->type == "remote" && core::SupportsRuleSetTools()) actions.push_back(ActionMenuItem(Localized("重新下载（重启内核）"),
                    [=] { refresh(resource->id); }).Enabled(!busy));
                actions.push_back(ActionMenuItem(Localized("删除"), [=] { remove(resource->id); }).Enabled(!busy).Danger());
                menu.Show(std::move(actions));
            }),
    }.With(huxerui::Spacing(8.0F)));
}

[[huxerui::composable]] huxerui::View RuleSetLibraryPanel(
    huxerui::State<huxerui::TextEditingValue> search,
    huxerui::State<std::function<void()>> addAction) {
    const auto model = huxerui::UseService<ProfilesModel>();
    const auto coreModel = huxerui::UseService<CoreModel>();
    auto tasks = huxerui::UseApplicationTaskScope();
    auto toast = huxerui::UseToast();
    auto dialog = huxerui::UseDialog();
    const auto navigation = huxerui::UseNavigation();
    const auto desktop = huxerui::UseEnvironment<DesktopSettingsNavigation>().model;
    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    const auto& theme = huxerui::UseTheme();
    auto busy = model->ruleSetsSaving;
    auto draft = huxerui::UseState(RuleSetDraft{});
    auto alive = huxerui::UseState(true);
    huxerui::Lifecycle([alive] { return [alive] { alive = false; }; }, 0);
    const auto library = model->ruleSetLibrary.Get();
    const auto refreshSucceeded = huxerui::UseString(Localized("本次已重新下载并应用"));
    const auto refreshQueued = huxerui::UseString(Localized("已安排在下次引用启动时重新下载"));
    const auto refreshFailed = huxerui::UseString(Localized("更新失败："));
    const auto persist = [=](std::function<void(singbox::RuleSetLibrary&)> edit, std::function<void()> done) {
        if (busy.Get()) return;
        busy = true;
        tasks.Launch([=]() -> huxerui::Task<void> {
            std::string error;
            auto saved = std::make_shared<singbox::RuleSetLibrary>();
            try {
                error = co_await RunOnTaskThread([library, edit, saved] {
                    auto next = *library;
                    edit(next);
                    std::string failure;
                    if (store::coreStore().saveRuleSetLibrary(next, failure)) *saved = store::coreStore().ruleSetLibrary();
                    return failure;
                });
            } catch (const std::exception& exception) { error = exception.what(); }
            busy = false;
            if (!error.empty()) { toast.Show(error); co_return; }
            model->ruleSetLibrary = std::shared_ptr<const wire::RuleSetLibrary>(std::move(saved));
            model->ruleSetsLoaded = true;
            model->ruleSetsRevision = model->ruleSetsRevision.Get() + 1;
            model->ruleSetRefreshReceipts = std::map<std::string, std::string>{};
            coreModel->RequestRefresh();
            toast.Show(Localized("规则集已保存"));
            if (done) done();
        });
    };
    const auto openEditor = [=](RuleSetResource resource) {
        if (!alive.Get() || busy.Get() || !model->ruleSetsLoaded.Get() || model->ruleSetLibrary.Get() != library) return;
        if (!resource) {
            auto created = std::make_shared<singbox::PersonalRuleSet>();
            created->id = "set-" + cfg::randomSecret();
            resource = std::move(created);
        }
        auto text = std::shared_ptr<const std::string>(resource, &resource->content);
        RuleSetDraft value{.resource = resource,
            .name = huxerui::TextEditingValue::FromText(resource->name),
            .url = huxerui::TextEditingValue::FromText(resource->url),
            .hours = huxerui::TextEditingValue::FromText(std::to_string(resource->update_hours)),
            .content = std::make_shared<TextDocument>(std::move(text)),
            .type = resource->type == "inline" ? 1U : 0U, .format = resource->format == "source" ? 1U : 0U};
        draft = std::move(value);
        const auto save = [=](std::function<void()> done) {
            if (busy.Get()) return;
            if (model->ruleSetLibrary.Get() != library) { toast.Show(Localized("规则集资源库已变化，请重新打开后编辑")); return; }
            const auto value = draft.Get();
            auto hours = value.resource->update_hours;
            if (value.type == 0) {
                const auto& text = value.hours.text;
                const auto parsed = std::from_chars(text.data(), text.data() + text.size(), hours);
                if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
                    toast.Show(Localized("更新周期需要填写整数小时")); return;
                }
            }
            const auto text = value.type == 1 ? value.content->Snapshot() : std::shared_ptr<const std::string>{};
            persist([base = value.resource, name = Trim(value.name.text), url = Trim(value.url.text),
                     type = value.type, format = value.format, hours, text](singbox::RuleSetLibrary& next) {
                auto edited = *base;
                edited.name = name;
                edited.type = type == 0 ? "remote" : "inline";
                edited.format = type == 0 && format == 0 ? "binary" : "source";
                edited.url = type == 0 ? url : "";
                edited.content = text ? *text : std::string{};
                edited.update_hours = hours;
                const auto found = std::ranges::find(next.resources, edited.id, &singbox::PersonalRuleSet::id);
                if (found == next.resources.end()) next.resources.push_back(std::move(edited));
                else *found = std::move(edited);
            }, std::move(done));
        };
        if (compact) OpenRuleEditor(navigation, desktop, [=] { return RuleSetEditorPage(draft, busy, save); });
        else dialog.Show([=](huxerui::DialogContext context) {
            return DialogCard(huxerui::Column{
                huxerui::Text(Localized("编辑规则集"), huxerui::TextRole::Title),
                huxerui::ScrollView(RuleSetForm(draft, busy)).With(huxerui::Frame{.max_height = 460.0F}, huxerui::ScrollBar()),
                huxerui::Row{
                    huxerui::Button(Localized("取消")).OnClick([=] { if (!busy.Get()) context.Dismiss(); }),
                    huxerui::Spacer(),
                    huxerui::Button(Localized("保存")).With(huxerui::Enabled(!busy.Get()))
                        .OnClick([=] { save([context] { context.Dismiss(); }); }),
                },
            }.With(huxerui::Spacing(16.0F), huxerui::Frame{.width = 480.0F},
                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
        });
    };
    huxerui::Lifecycle([=] { addAction = std::function<void()>{[=] { openEditor({}); }}; }, model->ruleSetLibrary, busy, model->ruleSetsLoaded);
    const auto remove = [=](std::string id) {
        if (!alive.Get() || busy.Get() || model->ruleSetLibrary.Get() != library) return;
        persist([id = std::move(id)](singbox::RuleSetLibrary& next) {
            std::erase_if(next.resources, [&](const auto& resource) { return resource.id == id; });
        }, {});
    };
    const auto refresh = [=](std::string id) {
        if (!alive.Get() || busy.Get() || model->ruleSetLibrary.Get() != library) return;
        busy = true;
        tasks.Launch([=]() -> huxerui::Task<void> {
            std::string error;
            bool downloaded = false;
            auto saved = std::make_shared<singbox::RuleSetLibrary>();
            try {
                error = co_await RunOnTaskThread([=, &downloaded] {
                    std::string failure;
                    if (store::coreStore().refreshPersonalRuleSet(id, library->revision, downloaded, failure))
                        *saved = store::coreStore().ruleSetLibrary();
                    return failure;
                });
            } catch (const std::exception& exception) { error = exception.what(); }
            busy = false;
            const auto message = !error.empty() ? refreshFailed + error : downloaded ? refreshSucceeded : refreshQueued;
            auto receipts = model->ruleSetRefreshReceipts.Get();
            receipts[id] = message;
            model->ruleSetRefreshReceipts = std::move(receipts);
            coreModel->RequestRefresh();
            if (error.empty()) {
                model->ruleSetLibrary = std::shared_ptr<const wire::RuleSetLibrary>(std::move(saved));
                model->ruleSetsRevision = model->ruleSetsRevision.Get() + 1;
            }
            toast.Show(message);
        });
    };
    std::vector<RuleSetResource> visible;
    for (const auto& resource : library->resources)
        if (SearchTextMatches(resource.name, search.Get().text))
            visible.emplace_back(library, &resource);
    const auto rows = std::make_shared<const std::vector<RuleSetResource>>(std::move(visible));
    const auto scroll = model->rulesScrolls.try_emplace(std::pair<std::int64_t, std::size_t>{-1, 0}).first->second;
    return huxerui::Column{
        !model->ruleSetsLoaded.Get() ? PageLoadingSkeleton(theme, true) : rows->empty() ?
            EmptyState(Localized("还没有规则集，点击加号添加"), app::images::route) :
            huxerui::View{huxerui::VirtualList(rows->size() + (compact ? 1U : 0U), [=](std::size_t index) -> huxerui::View {
                if (index == rows->size()) return CompactFloatingNavigationFooter();
                const auto receipts = model->ruleSetRefreshReceipts.Get();
                const auto receipt = receipts.find((*rows)[index]->id);
                return RuleSetResourceRow((*rows)[index], busy.Get(), receipt == receipts.end() ? "" : receipt->second,
                    openEditor, remove, refresh).Key((*rows)[index]->id);
            }).Controller(scroll).EstimatedItemExtent(92.0F).With(huxerui::Grow(1.0F), huxerui::ScrollBar())},
    }.With(huxerui::Spacing(12.0F), huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

std::string PersonalRulePattern(const singbox::ProfileRule& row, const singbox::RuleSetLibrary& library) {
    if (row.type == "rule_set") {
        const auto resource = std::ranges::find(library.resources, row.pattern, &singbox::PersonalRuleSet::id);
        if (resource != library.resources.end()) return resource->name;
    }
    return row.pattern;
}

[[huxerui::composable]] huxerui::View SubscriptionRulesPanel(
    ProfilesCache cache, huxerui::State<huxerui::TextEditingValue> search,
    huxerui::State<std::function<void()>> addAction, std::int64_t initialProfile) {
    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    const auto& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseApplicationTaskScope();
    auto toast = huxerui::UseToast();
    auto dialog = huxerui::UseDialog();
    auto menu = UseActionMenu();
    auto clipboard = huxerui::UseApplication().Clipboard();
    const auto navigation = huxerui::UseNavigation();
    const auto profilesModel = huxerui::UseService<ProfilesModel>();
    const auto desktopNavigation = huxerui::UseEnvironment<DesktopSettingsNavigation>().model;
    const auto coreModel = huxerui::UseService<CoreModel>();
    auto profileId = profilesModel->rulesProfile;
    huxerui::Lifecycle([profileId, initialProfile] {
        if (initialProfile != 0) profileId = initialProfile;
    }, initialProfile);
    auto snapshot = huxerui::UseState(std::make_shared<const ProfileRulesSnapshot>());
    auto generation = huxerui::UseState<std::uint64_t>(0);
    auto refresh = huxerui::UseState<std::uint64_t>(0);
    auto busy = huxerui::UseState(false);
    auto previewBusy = huxerui::UseState(false);
    auto view = profilesModel->rulesView;
    auto draft = huxerui::UseState(PersonalRuleDraft{});
    auto alive = huxerui::UseState(true);
    huxerui::Lifecycle([alive] { return [alive] { alive = false; }; }, 0);
    huxerui::Lifecycle([=] {
        if (profilesModel->rulesRequests.Get() != 0) profileId = profilesModel->rulesProfile.Get();
    }, profilesModel->rulesRequests);
    huxerui::Lifecycle([=] {
        if (profileId.Get() != 0 || !profilesModel->loaded.Get()) return;
        const auto& sources = profilesModel->list.Get();
        const auto main = std::ranges::find_if(sources, [](const auto& profile) { return profile.selected; });
        if (main != sources.end()) profileId = main->id;
        else if (!sources.empty()) profileId = sources.front().id;
    }, profilesModel->list, profilesModel->loaded);
    huxerui::Lifecycle([=] {
        const auto id = profileId.Get();
        const auto ticket = generation.Get() + 1;
        generation = ticket;
        if (snapshot.Get()->id != id) snapshot = std::make_shared<const ProfileRulesSnapshot>();
        if (!id) return;
        tasks.Launch([=]() -> huxerui::Task<void> {
            const auto revision = persistence::persistence().profilesRevision();
            ProfileRulesSnapshot result;
            try { result = co_await RunOnTaskThread([id] { return LoadPersonalRules(id); }); }
            catch (const std::exception& exception) { result.id = id; result.error = exception.what(); }
            if (!alive.Get() || ticket != generation.Get() || profileId.Get() != id) co_return;
            if (revision != persistence::persistence().profilesRevision()) { refresh = refresh.Get() + 1; co_return; }
            snapshot = std::make_shared<const ProfileRulesSnapshot>(std::move(result));
        });
    }, profileId, refresh, profilesModel->list, profilesModel->loaded, profilesModel->ruleSetsRevision);
    const auto renderedSource = snapshot.Get();
    const auto persist = [=](singbox::ProfileRuleEdits edits, std::function<void()> done) {
        if (busy.Get()) return;
        const auto source = snapshot.Get();
        if (!source->id || source->id != profileId.Get() || !source->supported) return;
        busy = true;
        tasks.Launch([=]() -> huxerui::Task<void> {
            std::string error;
            try {
                error = co_await RunOnTaskThread([source, edits] {
                    std::string failure;
                    store::profilesStore().saveRuleEdits(source->id, edits, source->content, failure);
                    return failure;
                });
            } catch (const std::exception& exception) { error = exception.what(); }
            busy = false;
            if (!error.empty()) { toast.Show(error); co_return; }
            profilesModel->PublishCached(); coreModel->RequestRefresh();
            refresh = refresh.Get() + 1;
            toast.Show(Localized("订阅个人规则已保存"));
            if (done) done();
        });
    };
    const auto openEditor = [=](singbox::ProfileRule row) {
        if (!alive.Get() || busy.Get()) return;
        if (snapshot.Get() != renderedSource) return;
        const auto source = snapshot.Get();
        if (!source->supported || source->id != profileId.Get()) return;
        PersonalRuleDraft value;
        auto targets = source->targets;
        const auto existing = std::ranges::find(source->edits.rules, row.editId, &singbox::ProfileRuleEdit::id);
        if (!row.editId.empty() && existing != source->edits.rules.end()) value.edit = *existing;
        else {
            value.edit.id = "rule-" + cfg::randomSecret();
            if (row.terminal) value.edit.operation = "final";
            else if (!row.anchor.empty()) { value.edit.operation = "replace"; value.edit.anchor = row.anchor; }
            value.edit.match = row.type.empty() ? "domain_suffix" : row.type;
            value.edit.pattern = row.pattern;
            value.edit.target = row.target.empty() ? "DIRECT" : row.target;
        }
        if (value.edit.operation == "disable") {
            value.edit.operation = "replace";
            value.edit.match = row.type; value.edit.pattern = row.pattern; value.edit.target = row.target;
        }
        value.pattern = huxerui::TextEditingValue::FromText(value.edit.pattern);
        const auto kind = std::ranges::find(kPersonalMatches, value.edit.match);
        value.match = kind == kPersonalMatches.end() ? 0 : static_cast<std::size_t>(kind - kPersonalMatches.begin());
        if (value.edit.match == "rule_set")
            value.resource = static_cast<std::size_t>(std::ranges::find(source->library.resources, value.edit.pattern,
                &singbox::PersonalRuleSet::id) - source->library.resources.begin());
        value.position = value.edit.operation == "before_final" ? 1 : 0;
        value.unavailable = value.edit.unavailable == "direct" ? 1 : value.edit.unavailable == "main" ? 2 : 0;
        if (std::ranges::find(targets, value.edit.target) == targets.end()) targets.push_back(value.edit.target);
        value.target = static_cast<std::size_t>(std::ranges::find(targets, value.edit.target) - targets.begin());
        draft = value;
        const auto save = [=](std::function<void()> done) {
            if (snapshot.Get() != source || profileId.Get() != source->id) {
                toast.Show(Localized("订阅已变化，请重新打开编辑器")); return;
            }
            auto edited = draft.Get().edit;
            edited.match = kPersonalMatches[draft.Get().match]; edited.pattern = draft.Get().pattern.text;
            if (edited.match == "rule_set" && edited.operation != "final") {
                if (draft.Get().resource >= source->library.resources.size()) {
                    toast.Show(Localized("请选择有效规则集")); return;
                }
                edited.pattern = source->library.resources[draft.Get().resource].id;
            }
            edited.target = targets[draft.Get().target];
            edited.unavailable = draft.Get().unavailable == 1 ? "direct" : draft.Get().unavailable == 2 ? "main" : "reject";
            if (edited.operation == "prepend" || edited.operation == "before_final")
                edited.operation = draft.Get().position == 0 ? "prepend" : "before_final";
            auto edits = source->edits;
            const auto found = std::ranges::find(edits.rules, edited.id, &singbox::ProfileRuleEdit::id);
            if (found == edits.rules.end()) edits.rules.push_back(std::move(edited)); else *found = std::move(edited);
            persist(std::move(edits), std::move(done));
        };
        if (compact) OpenRuleEditor(navigation, desktopNavigation, [=] { return PersonalRuleEditorPage(draft, targets, source->library, busy, save); });
        else dialog.Show([=](huxerui::DialogContext context) {
            return DialogCard(huxerui::Column{
                huxerui::Text(Localized("编辑订阅规则"), huxerui::TextRole::Title),
                huxerui::ScrollView(PersonalRuleForm(draft, targets, source->library, busy))
                    .With(huxerui::Frame{.max_height = 460.0F}, huxerui::ScrollBar()),
                huxerui::Row{
                    huxerui::Button(Localized("取消")).OnClick([context, busy] { if (!busy.Get()) context.Dismiss(); }),
                    huxerui::Spacer(),
                    huxerui::Button(Localized(source->selected ? "保存并应用" : "保存"))
                        .With(huxerui::Enabled(!busy.Get())).OnClick([save, context] { save([context] { context.Dismiss(); }); }),
                },
            }.With(huxerui::Spacing(16.0F), huxerui::Frame{.width = 480.0F},
                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
        });
    };
    huxerui::Lifecycle([=] {
        addAction = std::function<void()>{[openEditor] { openEditor({}); }};
    }, snapshot, profileId, busy);
    const auto change = [=](singbox::ProfileRule row, std::string action) {
        if (snapshot.Get() != renderedSource || busy.Get()) return;
        const auto source = snapshot.Get();
        if (action == "details") {
            dialog.Show([=](huxerui::DialogContext context) {
                return DialogCard(huxerui::Column{
                    huxerui::Text(Localized("规则详情"), huxerui::TextRole::Title),
                    huxerui::ScrollView(huxerui::Column{
                        huxerui::Text(row.raw),
                        huxerui::Text(row.type + " · " + row.pattern + " → " + row.target),
                    }.With(huxerui::Spacing(12.0F))).With(huxerui::Frame{.max_height = 300.0F}, huxerui::ScrollBar()),
                    huxerui::Button(Localized("关闭")).OnClick([context] { context.Dismiss(); }),
                }.With(huxerui::Spacing(16.0F), huxerui::Frame{.max_width = 600.0F}));
            }); return;
        }
        auto edits = source->edits;
        auto found = std::ranges::find(edits.rules, row.editId, &singbox::ProfileRuleEdit::id);
        if (action == "disable") edits.rules.push_back({.id = "rule-" + cfg::randomSecret(),
            .operation = "disable", .anchor = row.anchor});
        else if (found == edits.rules.end()) return;
        else if (action == "remove") edits.rules.erase(found);
        else if (action == "toggle") found->enabled = !found->enabled;
        else {
            const auto index = static_cast<std::size_t>(found - edits.rules.begin());
            if (action == "up") {
                for (std::size_t i = index; i > 0; --i) if (edits.rules[i-1].operation == found->operation) {
                    std::swap(edits.rules[i-1], edits.rules[index]); break;
                }
            } else for (std::size_t i = index+1; i < edits.rules.size(); ++i) if (edits.rules[i].operation == found->operation) {
                std::swap(edits.rules[i], edits.rules[index]); break;
            }
        }
        persist(std::move(edits), {});
    };
    const auto source = snapshot.Get();
    std::vector<db::Profile> sources;
    std::vector<std::string> names;
    std::size_t selected = 0;
    for (const auto& profile : cache.list) {
        if (profile.id == profileId.Get()) selected = sources.size();
        sources.push_back(profile); names.push_back(profile.name);
    }
    if (std::ranges::find(sources, profileId.Get(), &db::Profile::id) == sources.end() && profileId.Get() != 0) {
        selected = sources.size(); sources.push_back({.id = profileId.Get()});
        names.push_back(huxerui::UseString(Localized("订阅已删除")));
    }
    std::vector<singbox::ProfileRule> rows = view.Get() == 1 ? source->original.rules : source->effective.rules;
    if (view.Get() == 2) {
        rows.clear();
        for (const auto& edit : source->edits.rules) rows.push_back({.anchor = edit.anchor,
            .raw = edit.anchor, .type = edit.match, .pattern = edit.pattern, .target = edit.target,
            .editId = edit.id, .origin = edit.operation == "disable" ? "disabled" :
                edit.operation == "replace" || edit.operation == "final" ? "replaced" : "added",
            .position = edit.operation, .editable = edit.operation != "disable", .enabled = edit.enabled,
            .terminal = edit.operation == "final"});
    }
    std::erase_if(rows, [&](const auto& rule) {
        const auto& query = search.Get().text;
        return !SearchTextMatches(PersonalRulePattern(rule, source->library), query) && !SearchTextMatches(rule.target, query) && !SearchTextMatches(rule.type, query);
    });
    const auto records = std::make_shared<const std::vector<singbox::ProfileRule>>(std::move(rows));
    const auto scroll = profilesModel->rulesScrolls.try_emplace(std::pair{profileId.Get(), view.Get()}).first->second;
    auto list = source->id == 0 && profileId.Get() != 0 ? PageLoadingSkeleton(theme, true) :
        records->empty() ? EmptyState(Localized("没有匹配的规则"), app::images::route) :
        huxerui::View{huxerui::VirtualList(records->size() + (compact ? 1U : 0U), [=](std::size_t index) -> huxerui::View {
            if (index == records->size()) return CompactFloatingNavigationFooter();
            const auto& row = (*records)[index];
            return PersonalRuleRow(row, source->supported, busy.Get(), PersonalRulePattern(row, source->library), openEditor, change)
                .Key(row.editId.empty() ? row.anchor + ":" + std::to_string(index) : row.editId);
        }).Controller(scroll).EstimatedItemExtent(76.0F).With(huxerui::Grow(1.0F), huxerui::ScrollBar())};
    std::vector<huxerui::View> content{
        huxerui::Row{
            names.empty() ? huxerui::View{huxerui::Text(Localized("请先创建一个订阅连接"))} : huxerui::View{huxerui::Select(names, selected, [](const auto& text) { return huxerui::Text(text); })
                .With(huxerui::Grow(1.0F), huxerui::Enabled(!busy.Get()))
                .OnChanged([=](std::size_t index) { if (index < sources.size()) profileId = sources[index].id; })},
            huxerui::IconButton(app::images::more_vertical, Localized("查看规则"))
                .With(menu.Anchor()).OnClick([=] {
                    std::vector<ActionMenuEntry> items;
                    for (std::size_t i=0; i<kPersonalViews.size(); ++i)
                        items.push_back(ActionMenuItem(Localized(kPersonalViews[i]), [view, i] { view = i; }).Checked(view.Get() == i));
                    items.push_back(ActionMenuItem(Localized("编译 JSON"), [=] {
                        if (previewBusy.Get()) return;
                        previewBusy = true;
                        tasks.Launch([=]() -> huxerui::Task<void> {
                            std::string text;
                            try {
                                text = co_await RunOnTaskThread([source] {
                                    const auto result = store::coreStore().profileRulesPreview(source->id, source->content);
                                    return result.error.empty() ? result.json : result.error;
                                });
                            } catch (const std::exception& exception) { text = exception.what(); }
                            previewBusy = false;
                            if (!alive.Get() || snapshot.Get() != source || profileId.Get() != source->id) co_return;
                            const auto document = std::make_shared<TextDocument>(std::move(text));
                            dialog.Show([=](huxerui::DialogContext context) {
                                return DialogCard(huxerui::Column{
                                    huxerui::Text(Localized("编译 JSON"), huxerui::TextRole::Title),
                                    huxerui::Text(Localized("此预览仅包含当前订阅及个人修改，不代表正在运行的全局计划。")),
                                    DocumentTextView(document, true, 320.0F),
                                    huxerui::Row{
                                        huxerui::Button(Localized("复制")).OnClick([clipboard, document] { clipboard->WriteText(*document->Snapshot()); }),
                                        huxerui::Button(Localized("关闭")).OnClick([context] { context.Dismiss(); }),
                                    },
                                }.With(huxerui::Spacing(12.0F), huxerui::Frame{.max_width = 680.0F},
                                    huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
                            });
                        });
                    }).Enabled(source->id != 0 && source->supported && !busy.Get() && !previewBusy.Get()));
                    menu.Show(std::move(items));
                }),
        }.With(huxerui::Spacing(8.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
    };
    if (!source->error.empty())
        content.push_back(huxerui::Text(source->error).Style({huxerui::Font::System(font_size::kCaption), theme.colors.error}));
    if (source->id && !source->supported)
        content.push_back(huxerui::Text(Localized("此格式使用连接或内网路由配置，不支持订阅个人规则。")));
    content.push_back(std::move(list));
    return huxerui::Column{std::move(content)}.With(huxerui::Spacing(ResolveIslandTheme(theme).dense_radius),
        huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

using RuleSourceObjects = std::shared_ptr<const std::vector<singbox::SourceObject>>;

// 运行统计的高频变化只重组这个轻量副作用节点，不重建整页的规则列表。
[[huxerui::composable]] huxerui::View RuleRuntimeEffects(
    huxerui::StateList<std::string> activeConnections,
    huxerui::State<RuleSourceObjects> sourceObjects) {
    const auto coreModel = huxerui::UseService<CoreModel>();
    huxerui::Lifecycle([activeConnections, sourceObjects, coreModel] {
        const auto& core = coreModel->view.Get().core;
        const auto connections = core.state == core::CoreState::Running
            ? core.participatingSources : std::vector<std::string>{};
        if (!std::ranges::equal(activeConnections, connections))
            ReplaceStateList(activeConnections, connections);
        sourceObjects = core.sourceObjects;
        return [] {};
    }, coreModel->view);
    return huxerui::Row{};
}

[[huxerui::composable]] huxerui::View RulesPage(
    ProfilesCache profilesCache, std::function<void()> onBack, bool active, std::int64_t initialProfile) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const float denseGap = ResolveIslandTheme(theme).dense_radius;
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    auto tasks = huxerui::UseTaskScope();
    auto dialog = huxerui::UseDialog();
    auto toast = huxerui::UseToast();
    // 共享数据一律来自 application service（见 *_model.h）。
    const auto coreModel = huxerui::UseService<CoreModel>();
    const auto profilesModel = huxerui::UseService<ProfilesModel>();
    auto section = huxerui::UseState<std::size_t>(0);
    huxerui::Lifecycle([profilesModel, section] {
        if (profilesModel->rulesRequests.Get() != 0) section = 0;
    }, profilesModel->rulesRequests);
    auto sectionMotion = UseSectionTabMotion();
    auto ruleSearch = huxerui::UseState(huxerui::TextEditingValue{});
    auto subscriptionAdd = huxerui::UseState(std::function<void()>{});
    auto resourceAdd = huxerui::UseState(std::function<void()>{});
    huxerui::Lifecycle([=] {
        if (!profilesModel->loaded.Get() || profilesModel->ruleSetsLoaded.Get()) return;
        const auto revision = profilesModel->ruleSetsRevision.Get();
        tasks.Launch([=]() -> huxerui::Task<void> {
            try {
                auto library = co_await RunOnTaskThread([] { return store::coreStore().ruleSetLibrary(); });
                if (revision != profilesModel->ruleSetsRevision.Get()) co_return;
                profilesModel->ruleSetLibrary = std::make_shared<const wire::RuleSetLibrary>(std::move(library));
                profilesModel->ruleSetsLoaded = true;
            } catch (const std::exception& exception) { toast.Show(exception.what()); }
        });
    }, profilesModel->loaded);
    auto globalRevision = huxerui::UseState<std::uint64_t>(0);
    auto globalFilter = huxerui::UseState(std::make_shared<FilteredIndicesCache>()).Get();
    auto globalRules = huxerui::UseStateList<vpn::RouteRule>();
    // 订阅列表来自 ProfilesModel 的镜像（唯一来源见 profiles_model.h），
    // 本页不再自维护副本。
    auto profiles = profilesCache.list;
    auto activeConnections = huxerui::UseStateList<std::string>();
    auto sourceObjects = huxerui::UseState(RuleSourceObjects{});
    auto refreshTick = huxerui::UseState(0);
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
    auto editCatalog = huxerui::UseState(RuleTargetCatalog{});
    auto editSearch = huxerui::UseState(huxerui::TextEditingValue{});
    auto catalogRequest = huxerui::UseState<std::uint64_t>(0);
    auto catalogGeneration = huxerui::UseState<std::uint64_t>(0);
    auto policySaving = huxerui::UseState(false);
    auto loadGeneration = huxerui::UseState<std::uint64_t>(0);
    auto loaded = huxerui::UseState(false);

    auto policySavePending = huxerui::UseState(false);
    // 一次保存要「编译 + 内核 check + 重启内核」，耗时数秒；期间用户可能继续
    // 编辑（新增/开关/删除都会触发保存）。在途时只标 pending，完成后用最新
    // 列表补一次保存——直接丢弃会让刚写的规则在下一次重载时被存储值覆盖。
    auto dispatchPolicySave = std::make_shared<std::function<void()>>();
    *dispatchPolicySave = [tasks, globalRules, toast, refreshTick, policySaving,
                           policySavePending, loadGeneration, coreModel,
                           dispatchPolicySave] {
        policySaving = true;
        loadGeneration = loadGeneration.Get() + 1;
        vpn::VpnPolicy policy;
        policy.rules.reserve(globalRules.Size());
        for (const vpn::RouteRule& rule : globalRules) {
            policy.rules.push_back(rule);
        }
        tasks.Launch([policy = std::move(policy), toast, refreshTick, policySaving,
                      policySavePending, coreModel, dispatchPolicySave]() mutable
                         -> huxerui::Task<void> {
            std::string error;
            bool ok = false;
            try { const auto outcome = co_await RunOnTaskThread([policy = std::move(policy),
                                                       &error]() mutable {
                // 未命中的流量始终跟随当前启用的代理订阅；PPTP
                // 通过全局规则接管自己的目标网段。
                for (const db::Profile& profile : store::profilesStore().list()) {
                    if (profile.selected && profile.type != "pptp") {
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
            toast.Show(ok ? Localized("编排规则已保存")
                          : LocalizedFormat("全局规则保存失败：{}", error));
            // 失败不触发重载：列表保留用户编辑（含失败规则），修正后可重试；
            // 成功才从存储回读，拿到落库后的规则 ID 与顺序。
            if (ok) refreshTick = refreshTick.Get() + 1;
            if (policySavePending.Get()) {
                policySavePending = false;
                (*dispatchPolicySave)();
            }
        });
    };
    auto persistGlobalPolicy = [globalRevision, policySaving, policySavePending,
                                dispatchPolicySave] {
        globalRevision = globalRevision.Get() + 1;
        if (policySaving.Get()) {
            policySavePending = true;
            return;
        }
        (*dispatchPolicySave)();
    };

    huxerui::Lifecycle([=] {
        const auto ticket = loadGeneration.Get() + 1;
        loadGeneration = ticket;
        tasks.Launch([=]() -> huxerui::Task<void> {
            try {
                auto policy = co_await RunOnTaskThread([] { return store::vpnStore().globalPolicy(); });
                if (ticket != loadGeneration.Get() || policySaving.Get()) co_return;
                ReplaceStateList(globalRules, std::move(policy.rules));
                globalRevision = globalRevision.Get() + 1;
                if (profilesModel->loaded.Get()) loaded = true;
            } catch (const std::exception& error) { toast.Show(error.what()); }
        });
    }, profilesModel->list, profilesModel->loaded, refreshTick);

    huxerui::Lifecycle([=] {
        if (catalogRequest.Get() == 0) return;
        const auto ticket = catalogGeneration.Get() + 1;
        catalogGeneration = ticket;
        const auto& sources = editSources.Get();
        const auto index = editTarget.Get();
        const auto id = index < sources.size() ? sources[index].id : 0;
        editCatalog = RuleTargetCatalog{.profileId = id};
        tasks.Launch([=]() -> huxerui::Task<void> {
            const auto revision = persistence::persistence().profilesRevision();
            RuleTargetCatalog result;
            try {
                result = co_await RunOnTaskThread([id] { return LoadRuleTargetCatalog(id); });
            } catch (const std::exception& error) {
                result = RuleTargetCatalog{.profileId = id, .ready = true, .error = error.what()};
            }
            if (ticket != catalogGeneration.Get() || index != editTarget.Get() ||
                index >= editSources.Get().size() || editSources.Get()[index].id != id) co_return;
            if (revision != persistence::persistence().profilesRevision()) {
                catalogRequest = catalogRequest.Get() + 1; co_return;
            }
            editCatalog = std::move(result);
        });
    }, editTarget, editSources, catalogRequest, profilesModel->list, refreshTick);

    const auto selectRuleSource = [editTarget, editObject, editSearch, editCatalog, catalogRequest](std::size_t index) {
        if (editTarget.Get() == index) return;
        editTarget = index;
        ResetRuleTargetSelection(editCatalog, editObject, editSearch);
        catalogRequest = catalogRequest.Get() + 1;
    };
    const auto targetSelectionValid = [=](std::int64_t id) {
        // Keep an unchanged broken reference editable so the user can disable it
        // or choose an unavailable policy. Never silently rebind it.
        const auto index = editIndex.Get();
        if (index >= 0 && static_cast<std::size_t>(index) < globalRules.Size()) {
            const auto& old = globalRules[static_cast<std::size_t>(index)];
            if (old.connectionId == store::ProfileConnectionId(id) &&
                static_cast<std::size_t>(old.targetKind) == editKind.Get() &&
                old.targetObject == editObject.Get().text) return true;
        }
        return RuleTargetSelectionValid(editCatalog.Get(), id, editKind.Get(), editObject.Get().text);
    };

    huxerui::Lifecycle([globalRevision] {
        globalRevision = globalRevision.Get() + 1;
        return [] {};
    }, profiles, profilesModel->ruleSetsRevision);

    const huxerui::View runtimeEffects = RuleRuntimeEffects(activeConnections, sourceObjects);
    if (!active) return huxerui::Column{runtimeEffects}.Key("rules-idle");

    if (kPageTitlesInWindow && !compact && !loaded.Get()) {
        return huxerui::Column {
          runtimeEffects,
          PageLoadingScaffold(Localized("规则"), theme, true),
        }.With(huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }

    const auto mono = [](const std::string& text, huxerui::Color color) {
        return huxerui::Text(text).Style(huxerui::TextStyle{
            huxerui::Font::Monospace(font_size::kMonoBody), color});
    };

    auto openGlobalRuleEditor = [compact, dialog, profiles, editMatch, editPattern,
                                 editTarget, editPriority, globalRules,
                                 persistGlobalPolicy, globalEditorOpen, theme,
                                 editIndex, toast, editTier, editUnavailable, editKind,
                                 editObject, editEnabled, policySaving, editSources, editCatalog, editSearch,
                                 catalogRequest, selectRuleSource, targetSelectionValid](int index) {
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
        editSearch = huxerui::TextEditingValue{};
        editCatalog = RuleTargetCatalog{};
        catalogRequest = catalogRequest.Get() + 1;
        if (compact) {
            globalEditorOpen = true;
            return;
        }
        dialog.Show(
            [profiles, editMatch, editPattern, editTarget, editPriority,
             globalRules, persistGlobalPolicy, theme, editIndex, toast, targetProfiles,
             editTier, editUnavailable, editKind, editObject, editEnabled, policySaving,
             editCatalog, editSearch, selectRuleSource, targetSelectionValid](huxerui::DialogContext ctx)
                -> huxerui::View {
                auto targets = TargetNames(targetProfiles);
                if (!targets.empty() && editTarget.Get() >= targets.size()) targets.push_back(huxerui::UseString(Localized("目标来源已失效，请重新选择")));
                return DialogCard(huxerui::ScrollView(
                    huxerui::Column {
                        huxerui::Text(editIndex.Get() < 0 ? Localized("添加全局规则") : Localized("编辑全局规则"),
                                      huxerui::TextRole::Title),
                        huxerui::Text(
                            Localized("显式分流先于主订阅；无规则引用的次来源不参与。URL 按主机名匹配。"))
                            .Style(huxerui::TextStyle{
                                huxerui::Font::System(font_size::kCaption),
                                theme.colors.on_surface_variant}),
                        huxerui::Select(
                            kMatchKinds, editMatch.Get(),
                            [](const std::string& value) {
                                return huxerui::Text(Localized(value));
                            })
                            .OnChanged([editMatch](std::size_t index) {
                                editMatch = index;
                            })
                            .With(huxerui::Frame{.width = 220.0F}),
                        GlobalRulePatternField(editMatch, editPattern),
                        targets.empty()
                            ? huxerui::View{
                                  huxerui::Text(Localized("请先创建一个订阅连接"))}
                            : huxerui::View{huxerui::Select(
                                  targets, editTarget.Get(),
                                  [](const std::string& value) {
                                      return huxerui::Text(value);
                                  })
                                  .OnChanged(selectRuleSource)},
                        huxerui::TextField(editPriority.Get())
                            .Label(Localized("层内优先级（数字越大越先匹配）"))
                            .Variant(huxerui::TextFieldVariant::Outlined)
                            .OnChanged([editPriority](
                                           const huxerui::TextEditingValue& value) {
                                editPriority = value;
                            }),
                        RuleOptions(editTier, editUnavailable, editKind, editObject, editEnabled, editCatalog, editSearch),
                        huxerui::Row{
                            huxerui::Button(Localized("取消")).OnClick(
                                [ctx] { ctx.Dismiss(); }),
                            huxerui::Button(Localized("保存")).OnClick([=] {
                                if (policySaving.Get()) return;
                                if (targets.empty() || editTarget.Get() >= targetProfiles.size()) {
                                    toast.Show(Localized("请选择目标连接"));
                                    return;
                                }
                                if (!targetSelectionValid(targetProfiles[editTarget.Get()].id)) {
                                    toast.Show(Localized("请选择当前来源中的有效组或节点")); return;
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
                                    toast.Show(Localized("优先级必须是整数"));
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
                                    .targetObject = editKind.Get() == 0 ? "" : editObject.Get().text,
                                };
                                if (editIndex.Get() >= 0 && static_cast<std::size_t>(editIndex.Get()) < globalRules.Size())
                                    rule.id = globalRules[static_cast<std::size_t>(editIndex.Get())].id;
                                std::string validationError;
                                if (!ValidateRuleInput(rule, validationError)) {
                                    toast.Show(Localized(validationError));
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
                        .With(huxerui::Spacing(16.0F),
                              huxerui::CrossAlign(
                                  huxerui::CrossAxisAlignment::Stretch)))
                    .With(huxerui::Frame{.max_width = 520.0F, .max_height = 640.0F}, huxerui::ScrollBar()));
            },
            huxerui::DialogOptions{});
    };

    std::vector<huxerui::View> sectionPages;
    for (std::size_t page = 0; page < kRuleTabs.size(); ++page) {
        huxerui::View body;
        if (page == 0) {
            body = SubscriptionRulesPanel(profilesCache, ruleSearch, subscriptionAdd, initialProfile);
        } else if (page == 2) {
            body = RuleSetLibraryPanel(ruleSearch, resourceAdd);
        } else {
            const std::size_t count = globalRules.Size();
            const std::string query = ruleSearch.Get().text;
            const auto visibleRules = globalFilter->Resolve(globalRevision.Get(), count, query,
                [&globalRules, &profiles, &query, profilesModel](std::size_t index) {
                    const auto& rule = globalRules[index];
                    return SearchTextMatches(rule.match == vpn::MatchKind::RuleSet ? RuleSetName(*profilesModel->ruleSetLibrary.Get(), rule.pattern) : rule.pattern, query) || SearchTextMatches(rule.targetObject, query) ||
                           SearchTextMatches(ConnectionName(profiles, rule.connectionId), query) ||
                           SearchTextMatches(vpn::MatchKindName(rule.match), query) ||
                           SearchTextMatches(std::to_string(rule.priority), query);
                });
            const std::size_t visibleCount = visibleRules.Size();
            if (count == 0) {
                body = EmptyState(Localized("还没有全局规则"), app::images::route);
            } else if (visibleCount == 0 && !query.empty()) {
                body = EmptyState(Localized("没有匹配的全局规则"), app::images::search);
            } else {
                auto globalList = huxerui::VirtualList(
                    visibleCount + (compact ? 1U : 0U),
                    [globalRules, visibleRules,
                     profiles, mono, theme, compact, visibleCount, profilesModel,
                     persistGlobalPolicy, openGlobalRuleEditor, activeConnections,
                     policySaving, sourceObjects, denseGap](std::size_t index) -> huxerui::View {
                        const std::size_t ruleIndex =
                            index < visibleCount ? visibleRules.SourceIndex(index) : 0;
                        // 与订阅规则相同：只在测量 factory 中声明 Scope。
                        return huxerui::Scope([=]() -> huxerui::View {
                          if (compact && index == visibleCount) {
                              return CompactFloatingNavigationFooter().Key("compact-floating-footer");
                          }
                          const auto& rule = globalRules[ruleIndex];
                          const auto catalog = sourceObjects.Get();
                          const bool objectExists = rule.targetKind == vpn::TargetKind::Default || (catalog &&
                              std::ranges::any_of(*catalog, [&](const auto& object) {
                                  return object.sourceId == rule.connectionId && object.kind == rule.targetKind && object.objectId == rule.targetObject;
                              }));
                          const bool active = rule.enabled && objectExists && std::ranges::find(activeConnections, rule.connectionId) != activeConnections.end();
                          const auto key = rule.id.empty() ? "pending-rule-" + std::to_string(ruleIndex) : rule.id;
                          return DenseCard(
                              huxerui::Column {
                                mono(rule.match == vpn::MatchKind::Any ? huxerui::UseString(Localized("全部流量")) : rule.match == vpn::MatchKind::RuleSet ? RuleSetName(*profilesModel->ruleSetLibrary.Get(), rule.pattern) : rule.pattern, theme.colors.on_surface),
                                mono(huxerui::UseString(LocalizedFormat("{} · {} · 优先级 {}", huxerui::UseString(Localized(vpn::MatchKindName(rule.match))),
                                                 ConnectionName(profiles, rule.connectionId) + (rule.targetObject.empty() ? "" : " / " + rule.targetObject), rule.priority)),
                                     theme.colors.on_surface_variant),
                                huxerui::Flow {
                                  huxerui::Text(!rule.enabled ? Localized("已禁用") : active ? Localized("已参与编排") :
                                      LocalizedFormat("目标不可用/待应用 · {}", huxerui::UseString(Localized(kUnavailableNames[static_cast<std::size_t>(rule.unavailable)]))))
                                      .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption),
                                          active ? theme.colors.primary : theme.colors.on_surface_variant}),
                                  huxerui::Text(Localized(rule.tier == vpn::RuleTier::UserOverride ? "全局覆盖" : "来源分流")),
                                  huxerui::Button(Localized(rule.enabled ? "禁用" : "启用")).With(huxerui::Enabled(!policySaving.Get()))
                                      .OnClick([globalRules, ruleIndex, persistGlobalPolicy] {
                                          auto changed = globalRules[ruleIndex]; changed.enabled = !changed.enabled;
                                          globalRules.Set(ruleIndex, std::move(changed)); persistGlobalPolicy();
                                      }),
                                  huxerui::Button(Localized("上移")).OnClick([globalRules, ruleIndex, persistGlobalPolicy, policySaving] {
                                      if (policySaving.Get() || ruleIndex == 0) return;
                                      auto previous = globalRules[ruleIndex - 1]; auto current = globalRules[ruleIndex];
                                      globalRules.Set(ruleIndex - 1, std::move(current)); globalRules.Set(ruleIndex, std::move(previous));
                                      persistGlobalPolicy();
                                  }).With(huxerui::Enabled(!policySaving.Get() && ruleIndex > 0)),
                                  huxerui::Button(Localized("下移")).OnClick([globalRules, ruleIndex, persistGlobalPolicy, policySaving] {
                                      if (policySaving.Get() || ruleIndex + 1 >= globalRules.Size()) return;
                                      auto next = globalRules[ruleIndex + 1]; auto current = globalRules[ruleIndex];
                                      globalRules.Set(ruleIndex + 1, std::move(current)); globalRules.Set(ruleIndex, std::move(next));
                                      persistGlobalPolicy();
                                  }).With(huxerui::Enabled(!policySaving.Get() && ruleIndex + 1 < globalRules.Size())),
                                  huxerui::Button(Localized("编辑")).With(huxerui::Enabled(!policySaving.Get())).OnClick([openGlobalRuleEditor, ruleIndex] {
                                      openGlobalRuleEditor(static_cast<int>(ruleIndex));
                                  }),
                                  huxerui::Button(Localized("删除")).With(huxerui::Enabled(!policySaving.Get())).OnClick([globalRules, ruleIndex, persistGlobalPolicy] {
                                      if (ruleIndex < globalRules.Size()) {
                                          globalRules.Erase(ruleIndex);
                                          persistGlobalPolicy();
                                      }
                                  }),
                                }.With(huxerui::Spacing(8.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
                              }.With(huxerui::Spacing(6.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
                              .With(huxerui::Padding(huxerui::EdgeInsets{.bottom = denseGap}))
                              .Key(key);
                        });
                    });
                globalList = std::move(globalList)
                                 .EstimatedItemExtent(112.0F + denseGap)
                                 .With(huxerui::Grow(1.0F), huxerui::ScrollBar());
                body = huxerui::Column{std::move(globalList)}
                           .With(huxerui::Spacing(8.0F),
                                 huxerui::CrossAlign(
                                     huxerui::CrossAxisAlignment::Stretch),
                                 huxerui::Grow(1.0F));
            }
        }

        sectionPages.push_back(huxerui::Column {body}.With(
            huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
            .Key("rule-section-" + kRuleTabs[page].key));
    }
    huxerui::View body = SectionTabPages(sectionPages, section.Get(),
        [section](std::size_t index) { section = index; }, sectionMotion);

    // 分区切换与代理页/订阅页共用同一下划线标签栏（SectionTabBar）。
    huxerui::View sectionSwitch = SectionTabBar(
        kRuleTabs, kRuleTabs[section.Get()].key,
        [section](const std::string& key) { section = key == "resources" ? 2 : key == "global" ? 1 : 0; }, sectionMotion);
    // Each section supplies its own add action.
    huxerui::View actions = huxerui::View{
        huxerui::IconButton(app::images::add, Localized(section.Get() == 2 ? "添加规则集" : "添加规则"))
            .With(huxerui::Tooltip(Localized(section.Get() == 2 ? "添加规则集" : "添加规则")))
            .OnClick([section, subscriptionAdd, resourceAdd, openGlobalRuleEditor] {
                if (section.Get() == 0) { if (subscriptionAdd.Get()) subscriptionAdd.Get()(); }
                else if (section.Get() == 2) { if (resourceAdd.Get()) resourceAdd.Get()(); }
                else openGlobalRuleEditor(-1);
            })};
    body = huxerui::Column {
        std::move(sectionSwitch), std::move(body),
    }.With(huxerui::Spacing(denseGap), huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    huxerui::View searchField = PillSearchField(
        ruleSearch, Localized("搜索规则"));
    huxerui::View listPage = onBack
        ? SecondaryPageScaffold(std::move(searchField), std::move(actions),
                                std::move(body), onBack, false, true)
        : PageScaffold(Localized("规则"), std::move(actions), std::move(body),
                       false, true, true, std::nullopt, {},
                       std::move(searchField));
    listPage = huxerui::Column{runtimeEffects, listPage}.With(
        huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    const auto editorProfiles = editSources.Get();
    auto editorTargets = TargetNames(editorProfiles);
    if (!editorTargets.empty() && editTarget.Get() >= editorTargets.size()) editorTargets.push_back(huxerui::UseString(Localized("目标来源已失效，请重新选择")));
    const auto saveResponsiveRule = [=] {
        if (policySaving.Get()) return;
        if (editorTargets.empty() || editTarget.Get() >= editorProfiles.size()) {
            toast.Show(Localized("请选择目标连接"));
            return;
        }
        if (!targetSelectionValid(editorProfiles[editTarget.Get()].id)) {
            toast.Show(Localized("请选择当前来源中的有效组或节点")); return;
        }
        int priority = 0;
        const std::string priorityText = editPriority.Get().text;
        const auto parsed = std::from_chars(
            priorityText.data(), priorityText.data() + priorityText.size(),
            priority);
        if (parsed.ec != std::errc() ||
            parsed.ptr != priorityText.data() + priorityText.size()) {
            toast.Show(Localized("优先级必须是整数"));
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
            .targetObject = editKind.Get() == 0 ? "" : editObject.Get().text,
        };
        if (editIndex.Get() >= 0 && static_cast<std::size_t>(editIndex.Get()) < globalRules.Size())
            rule.id = globalRules[static_cast<std::size_t>(editIndex.Get())].id;
        std::string validationError;
        if (!ValidateRuleInput(rule, validationError)) {
            toast.Show(Localized(validationError));
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
        Localized(editIndex.Get() < 0 ? "添加全局规则"
                                      : "编辑全局规则"),
        huxerui::Row {
            huxerui::IconButton(app::images::arrow_back, Localized("返回"))
                .With(huxerui::Tooltip(Localized("返回规则列表")))
                .OnClick([globalEditorOpen] { globalEditorOpen = false; }),
            huxerui::IconButton(app::images::save, Localized("保存规则"))
                .With(huxerui::Tooltip(Localized("保存规则")))
                .OnClick(saveResponsiveRule),
        }.With(huxerui::Spacing(8.0F)),
        huxerui::ScrollView(
            huxerui::Column {
                Card(huxerui::Column {
                    huxerui::Text(
                        Localized("显式分流先于主订阅；无规则引用的次来源不参与。URL 按主机名匹配。")),
                    huxerui::Select(
                        kMatchKinds, editMatch.Get(),
                        [](const std::string& value) {
                            return huxerui::Text(Localized(value));
                        })
                        .OnChanged([editMatch](std::size_t index) {
                            editMatch = index;
                        }),
                    GlobalRulePatternField(editMatch, editPattern),
                    editorTargets.empty()
                        ? huxerui::View{huxerui::Text(Localized("请先创建一个订阅连接"))}
                        : huxerui::View{huxerui::Select(
                              editorTargets, editTarget.Get(),
                              [](const std::string& value) {
                                  return huxerui::Text(value);
                              })
                              .OnChanged(selectRuleSource)},
                    huxerui::TextField(editPriority.Get())
                        .Label(Localized("层内优先级（数字越大越先匹配）"))
                        .Variant(huxerui::TextFieldVariant::Outlined)
                        .OnChanged([editPriority](
                                       const huxerui::TextEditingValue& value) {
                            editPriority = value;
                        }),
                    RuleOptions(editTier, editUnavailable, editKind, editObject, editEnabled, editCatalog, editSearch),
                }.With(huxerui::Spacing(16.0F),
                       huxerui::CrossAlign(
                           huxerui::CrossAxisAlignment::Stretch))),
                CompactFloatingNavigationFooter(),
            }.With(huxerui::Spacing(16.0F),
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
