#include <huxerui/huxerui.h>
#include <algorithm>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "app_resources.h"
#include "ui.h"
#include "task_bridge.h"
import clashflux.core;
import clashflux.model;
import clashflux.store.core;
#include "core_model.h"
#include "core_control.h"
#include "settings_model.h"
#include "profiles_model.h"
#include "page_layout.h"
#include "settings_group_layout.h"
#include "action_menu.h"

namespace clashflux::ui {
namespace {
using Network = clashflux::network::Settings;
#if defined(__ANDROID__)
constexpr bool kAndroidNetwork = true;
extern "C" const char* clashflux_android_visible_apps() noexcept;
#else
constexpr bool kAndroidNetwork = false;
#endif

#if defined(__linux__) && !defined(__ANDROID__)
constexpr bool kLinuxNetwork = true;
#else
constexpr bool kLinuxNetwork = false;
#endif
#if (defined(__linux__) && !defined(__ANDROID__)) || defined(_WIN32)
constexpr bool kRouteSetNetwork = true;
#else
constexpr bool kRouteSetNetwork = false;
#endif
using AdvancedNetwork = clashflux::network::Advanced;

struct NetworkForm {
    Network value;
    AdvancedNetwork extra;
    std::map<std::string, huxerui::TextEditingValue> fields;
    explicit NetworkForm(const Network& source) : value(source), extra(source.advanced.value_or(AdvancedNetwork{})) {
        const auto add = [&](const std::string& name, const std::string& text) {
            fields[name] = huxerui::TextEditingValue::FromText(text);
        };
        add("mixed_port", std::to_string(source.mixed_port));
        add("http_port", source.http_port ? std::to_string(source.http_port) : "");
        add("socks_port", source.socks_port ? std::to_string(source.socks_port) : "");
        add("mtu", std::to_string(source.mtu));
        add("udp_nat_max", std::to_string(source.udp_nat_max));
        add("interface_name", source.interface_name); add("addresses", source.addresses);
        add("routes", source.routes); add("exclusions", source.exclusions);
        add("udp_timeout", source.udp_timeout); add("packages", source.packages);
        add("listen_address", source.listen_address); add("bind_interface", source.bind_interface);
        add("allowed_sources", source.allowed_sources); add("username", source.username);
        add("password", source.password); add("bypass", source.bypass);
        add("interfaces", extra.interfaces); add("uids", extra.uids); add("macs", extra.macs);
        add("route_sets", extra.route_sets); add("exclude_sets", extra.exclude_sets);
        add("pac_port", std::to_string(extra.pac_port)); add("pac_url", extra.pac_url);
        add("pac_direct_domains", extra.pac_direct_domains);
    }
    bool Changed(const Network& source) const {
        const NetworkForm original(source);
        if (value != original.value || extra != original.extra) return true;
        return std::ranges::any_of(fields, [&](const auto& item) {
            return item.second.text != original.fields.at(item.first).text;
        });
    }
    // Validate one field through the same validators as the committed DTO. Other
    // unfinished fields must not hide this field's error or its tab badge.
    std::string FieldError(const std::string& key) const {
        const auto& text = fields.at(key).text;
        Network candidate;
        auto advanced = AdvancedNetwork{};
        if (key == "mixed_port" || key == "http_port" || key == "socks_port") {
            const auto trim = [](std::string_view input) {
                const auto first = input.find_first_not_of(" \t\r\n");
                return first == std::string_view::npos ? std::string_view{} :
                    input.substr(first, input.find_last_not_of(" \t\r\n") - first + 1);
            };
            const auto input = trim(text);
            if (input.empty() && key != "mixed_port") return {};
            int port = 0;
            if (!clashflux::network::Integer(input, port, 1, 65535)) return "端口无效";
            for (const auto* other : {"mixed_port", "http_port", "socks_port"}) {
                int number = 0;
                if (key != other && clashflux::network::Integer(trim(fields.at(other).text), number, 1, 65535)
                    && number == port) return "不同类型的端口不能重复";
            }
        } else if (key == "mtu") {
            if (value.override_mtu && !clashflux::tun_settings::ParseMtu(text))
                return "MTU 需要填写 1280–9000 的整数";
        } else if (key == "udp_nat_max") {
            if (value.override_udp_nat_max && !clashflux::network::Integer(text, candidate.udp_nat_max, 0, 1048576))
                return "UDP 会话上限需要填写 0–1048576 的整数";
        } else if (key == "pac_port") {
            if (extra.proxy_mode != "pac") return {};
            if (!clashflux::network::Integer(text, advanced.pac_port, 1, 65535))
                return "PAC 端口需要 1–65535 的整数";
            for (const auto* port : {"mixed_port", "http_port", "socks_port"}) {
                int number = 0;
                if (clashflux::network::Integer(fields.at(port).text, number, 1, 65535) && number == advanced.pac_port)
                    return "PAC 端口不能与代理监听端口重复";
            }
        } else if (key == "addresses") candidate.addresses = text;
        else if (key == "interface_name") candidate.interface_name = text;
        else if (key == "routes") { candidate.override_routes = value.override_routes; candidate.routes = text; }
        else if (key == "exclusions") {
            candidate.override_exclusions = value.override_exclusions; candidate.exclusions = text;
        }
        else if (key == "udp_timeout") candidate.udp_timeout = text;
        else if (key == "packages") { candidate.package_mode = value.package_mode; candidate.packages = text; }
        else if (key == "listen_address") {
            candidate.override_listen = value.override_listen; candidate.listen_address = text;
        }
        else if (key == "bind_interface") candidate.bind_interface = text;
        else if (key == "allowed_sources") candidate.allowed_sources = text;
        else if (key == "username" || key == "password") {
            candidate.override_auth = value.override_auth;
            candidate.username = fields.at("username").text; candidate.password = fields.at("password").text;
        } else if (key == "bypass") candidate.bypass = text;
        else if (key == "pac_url") {
            if (extra.proxy_mode != "pac_url") return {};
            advanced.proxy_mode = extra.proxy_mode; advanced.pac_url = text;
        } else if (key == "pac_direct_domains") advanced.pac_direct_domains = text;
        else if (key == "interfaces" || key == "uids" || key == "macs") {
            advanced.override_filters = extra.override_filters; advanced.auto_redirect = extra.auto_redirect;
            if (key == "interfaces") { advanced.interface_mode = extra.interface_mode; advanced.interfaces = text; }
            if (key == "uids") { advanced.uid_mode = extra.uid_mode; advanced.uids = text; }
            if (key == "macs") { advanced.mac_mode = extra.mac_mode; advanced.macs = text; }
        } else if (key == "route_sets" || key == "exclude_sets") {
            advanced.override_route_sets = extra.override_route_sets;
            if (key == "route_sets") advanced.route_sets = text; else advanced.exclude_sets = text;
        }
        candidate.advanced = advanced;
        return clashflux::network::Validate(candidate, kAndroidNetwork);
    }
    std::string Read(Network& output) const {
        output = value;
        const auto ports = ParseInboundPorts(fields.at("mixed_port").text,
            fields.at("http_port").text, fields.at("socks_port").text);
        if (ports.error != PortSettingsError::None)
            return ports.error == PortSettingsError::Duplicate ? "不同类型的端口不能重复" : "端口无效";
        output.mixed_port = std::stoi(ports.ports.mixed);
        output.http_port = ports.ports.http.empty() ? 0 : std::stoi(ports.ports.http);
        output.socks_port = ports.ports.socks.empty() ? 0 : std::stoi(ports.ports.socks);
        if (value.override_mtu) {
            const auto mtu = clashflux::tun_settings::ParseMtu(fields.at("mtu").text);
            if (!mtu) return "MTU 需要填写 1280–9000 的整数";
            output.mtu = *mtu;
        }
        if (value.override_udp_nat_max && !clashflux::network::Integer(fields.at("udp_nat_max").text,
            output.udp_nat_max, 0, 1048576)) return "UDP 会话上限需要填写 0–1048576 的整数";
        output.interface_name = fields.at("interface_name").text;
        output.addresses = fields.at("addresses").text;
        output.routes = fields.at("routes").text; output.exclusions = fields.at("exclusions").text;
        output.udp_timeout = fields.at("udp_timeout").text; output.packages = fields.at("packages").text;
        output.listen_address = fields.at("listen_address").text;
        output.bind_interface = fields.at("bind_interface").text;
        output.allowed_sources = fields.at("allowed_sources").text;
        output.username = fields.at("username").text; output.password = fields.at("password").text;
        output.bypass = fields.at("bypass").text;
        auto advanced = extra;
        advanced.interfaces = fields.at("interfaces").text; advanced.uids = fields.at("uids").text;
        advanced.macs = fields.at("macs").text;
        advanced.route_sets = fields.at("route_sets").text; advanced.exclude_sets = fields.at("exclude_sets").text;
        advanced.pac_url = fields.at("pac_url").text; advanced.pac_direct_domains = fields.at("pac_direct_domains").text;
        if (advanced.proxy_mode == "pac" && !clashflux::network::Integer(fields.at("pac_port").text, advanced.pac_port, 1, 65535))
            return "PAC 端口需要 1–65535 的整数";
        if (value.advanced || advanced != AdvancedNetwork{}) output.advanced = advanced;
        return clashflux::network::Validate(output, kAndroidNetwork);
    }
};

huxerui::View NetworkField(huxerui::State<NetworkForm> form, const std::string& key,
                          huxerui::StringVariant label, bool multiline = false, bool secure = false) {
    const auto error = form.Get().FieldError(key);
    return SettingsContentRow(huxerui::TextField(form.Get().fields.at(key)).Label(label)
        .Variant(huxerui::TextFieldVariant::Outlined).Secure(secure)
        .Validation(error.empty() ? huxerui::ValidationResult::Valid() : huxerui::ValidationResult::Invalid(error))
        .LineLimits(multiline ? huxerui::TextFieldLineLimits::MultiLine(2, 5) : huxerui::TextFieldLineLimits::SingleLine())
        .OnChanged([form, key](const huxerui::TextEditingValue& value) {
            form.Update([&](NetworkForm& next) { next.fields.at(key) = value; });
        })).Key("network-field-" + key);
}

huxerui::View NetworkSection(std::vector<huxerui::View> rows) {
    // One modal surface; rows own their whitespace and feedback, without nested cards.
    return huxerui::ProvideEnvironment(SettingsRowInsets{2.0F, 8.0F, 48.0F},
        huxerui::Column(rows).With(huxerui::Spacing(0.0F),
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
}

[[huxerui::composable]] huxerui::View NetworkDisclosure(huxerui::StringVariant title,
    huxerui::State<bool> expanded, huxerui::StringVariant summary = {}) {
    const auto& theme = huxerui::UseTheme();
    return SettingsActionRow(huxerui::Row {
      huxerui::Text(title).With(huxerui::Grow(1.0F)),
      huxerui::Text(summary),
      huxerui::Image(app::images::chevron_down).Tint(theme.colors.on_surface_variant)
          .With(huxerui::Frame{.width = 20.0F, .height = 20.0F},
              huxerui::Rotation(expanded.Get() ? 0.0F : -90.0F)),
    }.With(huxerui::Spacing(8.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        [expanded] { expanded = !expanded.Get(); },
        {.role = huxerui::SemanticRole::Button, .label = title, .expanded = expanded.Get()});
}
huxerui::View NetworkSwitch(huxerui::State<NetworkForm> form, bool Network::* member,
                           huxerui::StringVariant label, huxerui::StringVariant hint) {
    return SettingSwitchRow(label, hint, huxerui::Switch(form.Get().value.*member)
        .OnChanged([form, member](bool value) { form.Update([&](NetworkForm& next) { next.value.*member = value; }); }));
}
struct Choice { std::string id; huxerui::StringVariant label; };
huxerui::View NetworkChoice(huxerui::State<NetworkForm> form, std::string Network::* member,
                           huxerui::StringVariant label, const std::vector<Choice>& items) {
    const auto found = std::ranges::find(items, form.Get().value.*member, &Choice::id);
    const auto index = found == items.end() ? 0U : static_cast<std::size_t>(found - items.begin());
    return SettingRow(label, {},
      huxerui::Select(items, index, [](const Choice& item) { return huxerui::Text(item.label); })
          .Label(label).OnChanged([form, member, items](std::size_t selected) {
              if (selected < items.size()) form.Update([&](NetworkForm& next) { next.value.*member = items[selected].id; });
          }).With(huxerui::Frame{.width = 240.0F})
    );
}

huxerui::View AdvancedChoice(huxerui::State<NetworkForm> form, std::string AdvancedNetwork::* member,
                            huxerui::StringVariant label, const std::vector<Choice>& items) {
    const auto found = std::ranges::find(items, form.Get().extra.*member, &Choice::id);
    const auto index = found == items.end() ? 0U : static_cast<std::size_t>(found - items.begin());
    return SettingRow(label, {},
      huxerui::Select(items, index, [](const Choice& item) { return huxerui::Text(item.label); })
          .Label(label).OnChanged([form, member, items](std::size_t selected) {
              if (selected < items.size()) form.Update([&](NetworkForm& next) { next.extra.*member = items[selected].id; });
          }).With(huxerui::Frame{.width = 240.0F})
    );
}
huxerui::View AdvancedSwitch(huxerui::State<NetworkForm> form, bool AdvancedNetwork::* member,
                            huxerui::StringVariant label, huxerui::StringVariant hint) {
    return SettingSwitchRow(label, hint, huxerui::Switch(form.Get().extra.*member)
        .OnChanged([form, member](bool value) { form.Update([&](NetworkForm& next) { next.extra.*member = value; }); }));
}

[[huxerui::composable]] huxerui::View NetworkRouteSets(huxerui::State<NetworkForm> form) {
    const auto model = huxerui::UseService<ProfilesModel>();
    const auto tasks = huxerui::UseApplicationTaskScope();
    const auto toast = UseFluxToast();
    huxerui::Lifecycle([=] {
        if (!model->loaded.Get()) return;
        const auto revision = model->ruleSetsRevision.Get();
        tasks.Launch([=]() -> huxerui::Task<void> {
            try {
                auto library = co_await RunOnTaskThread([] { return store::coreStore().ruleSetLibrary(); });
                if (revision != model->ruleSetsRevision.Get()) co_return;
                model->ruleSetLibrary = std::make_shared<const wire::RuleSetLibrary>(std::move(library));
                model->ruleSetsLoaded = true;
            } catch (const std::exception& error) { toast.Show(error.what()); }
        });
    }, model->loaded, model->ruleSetsRevision);
    std::vector<huxerui::View> choices;
    const auto library = model->ruleSetLibrary.Get();
    for (const auto& resource : library->resources) {
        std::vector<huxerui::View> checks;
        checks.push_back(huxerui::Text(resource.name).With(huxerui::Grow(1.0F)));
        for (const auto& key : {std::string("route_sets"), std::string("exclude_sets")}) {
            const auto selected = clashflux::network::Lines(form.Get().fields.at(key).text);
            const bool checked = std::ranges::find(selected, resource.id) != selected.end();
            checks.push_back(huxerui::Checkbox(key == "route_sets" ? Localized("接管") : Localized("排除"), checked)
                .OnChanged([form, key, id = resource.id](bool enabled) {
                    form.Update([&](NetworkForm& next) {
                        auto list = clashflux::network::Lines(next.fields.at(key).text);
                        std::erase(list, id); if (enabled) list.push_back(id);
                        std::string text;
                        for (const auto& item : list) { if (!text.empty()) text += '\n'; text += item; }
                        next.fields.at(key) = huxerui::TextEditingValue::FromText(text);
                    });
                }));
        }
        choices.push_back(huxerui::Row(checks).With(huxerui::Spacing(12.0F),
            huxerui::Frame{.min_height = 48.0F},
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)).Key(resource.id));
    }
    for (const auto& key : {std::string("route_sets"), std::string("exclude_sets")})
        for (const auto& id : clashflux::network::Lines(form.Get().fields.at(key).text))
            if (model->ruleSetsLoaded.Get() && std::ranges::none_of(library->resources, [&](const auto& resource) { return resource.id == id; }))
                choices.push_back(huxerui::Checkbox(LocalizedFormat("已失效的资源：{}", id), true)
                    .OnChanged([form, key, id](bool enabled) {
                        if (enabled) return;
                        form.Update([&](NetworkForm& next) {
                            const auto list = clashflux::network::Lines(next.fields.at(key).text);
                            std::string text;
                            for (const auto& item : list) if (item != id) { if (!text.empty()) text += '\n'; text += item; }
                            next.fields.at(key) = huxerui::TextEditingValue::FromText(text);
                        });
                    }).Key("missing-" + key + "-" + id));
    return huxerui::Column {
      AdvancedSwitch(form, &AdvancedNetwork::override_route_sets, Localized("覆盖规则集接管"),
          Localized("仅提取全局资源中的目标 IP 网段；域名条件不参与系统路由")),
      form.Get().extra.override_route_sets ? SettingsContentRow(huxerui::Column {
        PageScrollView(huxerui::Column(std::move(choices)).With(huxerui::Spacing(12.0F)))
            .With(huxerui::Frame{.max_height = 220.0F}),
        huxerui::Text(Localized("可在设置的外部资源页面添加或管理规则集。")),
      }.With(huxerui::Spacing(12.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))) : huxerui::View{},
    }.With(huxerui::Spacing(0.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

struct NetworkApplication { std::string package, label; bool operator==(const NetworkApplication&) const = default; };
std::vector<NetworkApplication> ReadNetworkApplications() {
    std::vector<NetworkApplication> result;
#if defined(__ANDROID__)
    for (const auto& line : clashflux::network::Lines(clashflux_android_visible_apps())) {
        const auto tab = line.find('\t');
        if (tab != std::string::npos) result.push_back({line.substr(0, tab), line.substr(tab + 1)});
    }
#endif
    return result;
}

[[huxerui::composable]] huxerui::View NetworkApplications(huxerui::State<NetworkForm> form) {
    const auto apps = huxerui::UseState(std::vector<NetworkApplication>{});
    const auto loading = huxerui::UseState(false);
    const auto tasks = huxerui::UseTaskScope();
    const auto toast = UseFluxToast();
    const auto load = [=] {
        if (loading.Get()) return;
        loading = true;
        tasks.Launch([=]() -> huxerui::Task<void> {
            try { apps = co_await RunOnTaskThread([] { return ReadNetworkApplications(); }); }
            catch (...) { toast.Show(Localized("应用列表读取失败")); }
            loading = false;
        });
    };
    const auto chosen = clashflux::network::Lines(form.Get().fields.at("packages").text);
    const auto mode = form.Get().value.package_mode;
    const bool custom = mode == "include" || mode == "exclude";
    const huxerui::View list = ScrollArea(huxerui::VirtualList(apps.Get().size(), [=](std::size_t index) {
        const auto item = apps.Get()[index];
        const bool selected = std::find(chosen.begin(), chosen.end(), item.package) != chosen.end();
        return huxerui::Checkbox(item.label + " (" + item.package + ")", selected)
            .OnChanged([form, item](bool checked) {
                form.Update([&](NetworkForm& next) {
                    auto packages = clashflux::network::Lines(next.fields.at("packages").text);
                    std::erase(packages, item.package);
                    if (checked) packages.push_back(item.package);
                    std::string text;
                    for (const auto& package : packages) { if (!text.empty()) text += '\n'; text += package; }
                    next.fields.at("packages") = huxerui::TextEditingValue::FromText(text);
                });
            }).With(huxerui::Frame{.min_height = 48.0F}).Key(item.package);
    }).EstimatedItemExtent(48.0F)).With(huxerui::Frame{.height = 240.0F});
    return huxerui::Column {
      NetworkChoice(form, &Network::package_mode, Localized("应用接管方式"), {
          {"", Localized("应用：跟随配置")}, {"all", Localized("全部应用")},
          {"include", Localized("仅选中应用")}, {"exclude", Localized("排除选中应用")}}),
      custom ? huxerui::View(huxerui::Column {
        NetworkField(form, "packages", Localized("应用包名（每行一个）"), true),
        SettingsActionRow(huxerui::Text(Localized("选择可见应用")), load,
            {.role = huxerui::SemanticRole::Button, .label = Localized("选择可见应用")})
            .With(huxerui::Enabled(!loading.Get())),
        loading.Get() ? SettingsContentRow(huxerui::ProgressCircle()) : huxerui::View{},
        apps.Get().empty() ? huxerui::View{} : SettingsContentRow(list),
      }.With(huxerui::Spacing(0.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))) : huxerui::View{},
    }.With(huxerui::Spacing(0.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}
[[huxerui::composable]] huxerui::View NetworkTunBasics(huxerui::State<NetworkForm> form) {
    const auto addresses = huxerui::UseState(!form.Get().value.addresses.empty() ||
        !form.Get().value.interface_name.empty());
    std::vector<huxerui::View> adapter;
    if (!kAndroidNetwork) adapter.push_back(NetworkChoice(form, &Network::stack, Localized("协议栈"), {
        {"", Localized("跟随配置")}, {"system", "System"}, {"gvisor", "gVisor"}, {"mixed", "Mixed"}}));
    else adapter.push_back(SettingsContentRow(
        huxerui::Text(Localized("Android 使用 gVisor 协议栈，严格路由由平台管理。"))));
    adapter.push_back(NetworkSwitch(form, &Network::override_mtu, Localized("覆盖 MTU"),
        Localized("关闭时保留配置中的 MTU")));
    if (form.Get().value.override_mtu) adapter.push_back(NetworkField(form, "mtu", "MTU"));
    adapter.push_back(NetworkDisclosure(Localized("网卡名称与地址"), addresses).Key("network-address-toggle"));
    if (addresses.Get()) {
        if (!kAndroidNetwork) adapter.push_back(NetworkField(form, "interface_name", Localized("网卡名称（留空跟随配置）")));
        adapter.push_back(NetworkField(form, "addresses", Localized("网卡 IPv4/IPv6 地址（每行一个 CIDR）"), true));
    }
    return huxerui::Column {
      NetworkSection(adapter),
      NetworkSection({
        NetworkChoice(form, &Network::dns_mode, Localized("DNS 接管方式"), {
            {"", Localized("跟随配置")}, {"native", Localized("原生系统 DNS")},
            {"hijack", Localized("劫持 DNS")}, {"disabled", Localized("关闭 DNS 接管")}}),
        NetworkSwitch(form, &Network::ipv6, "IPv6", Localized("全局网络设置，同时影响 DNS 和虚拟网卡")),
      }),
      kAndroidNetwork ? huxerui::View{} : NetworkSection({
        NetworkSwitch(form, &Network::strict_route, Localized("严格路由"), Localized("减少流量绕过虚拟网卡的情况")),
      }),
    }.With(huxerui::Spacing(20.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View NetworkLinuxFilters(huxerui::State<NetworkForm> form) {
    const auto interfaces = huxerui::UseState(!form.Get().extra.interface_mode.empty());
    const auto users = huxerui::UseState(!form.Get().extra.uid_mode.empty());
    const auto macs = huxerui::UseState(!form.Get().extra.mac_mode.empty());
    std::vector<huxerui::View> rows{
        AdvancedSwitch(form, &AdvancedNetwork::override_filters, Localized("覆盖接管筛选"),
            Localized("筛选流量在进入内核之前生效；关闭覆盖则保留来源配置")),
    };
    if (form.Get().extra.override_filters) {
        const std::vector<Choice> modes = {{"", Localized("不限制")},
            {"include", Localized("仅选中")}, {"exclude", Localized("排除选中")}};
        const auto filter = [&](huxerui::State<bool> expanded, std::string AdvancedNetwork::* mode,
                                const std::string& field, huxerui::StringVariant title,
                                huxerui::StringVariant label) {
            const auto selected = std::ranges::find(modes, form.Get().extra.*mode, &Choice::id);
            return huxerui::Column {
              NetworkDisclosure(title, expanded, selected == modes.end() ? modes[0].label : selected->label),
              expanded.Get() ? AdvancedChoice(form, mode, title, modes) : huxerui::View{},
              expanded.Get() && !(form.Get().extra.*mode).empty()
                  ? NetworkField(form, field, label, true) : huxerui::View{},
            }.With(huxerui::Spacing(0.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
        };
        rows.push_back(filter(interfaces, &AdvancedNetwork::interface_mode, "interfaces",
            Localized("网络接口筛选"), Localized("接口名称（每行一个）")));
        rows.push_back(filter(users, &AdvancedNetwork::uid_mode, "uids",
            Localized("用户 UID 筛选"), Localized("UID 或起始:结束（每行一个）")));
        rows.push_back(filter(macs, &AdvancedNetwork::mac_mode, "macs",
            Localized("MAC 地址筛选"), Localized("MAC 地址（每行一个，需要自动重定向）")));
    }
    return NetworkSection(rows);
}

[[huxerui::composable]] huxerui::View NetworkTunScope(huxerui::State<NetworkForm> form) {
    std::vector<huxerui::View> sections;
    if (kLinuxNetwork) sections.push_back(NetworkSection({
        AdvancedChoice(form, &AdvancedNetwork::auto_redirect, Localized("路由方式"), {
            {"", Localized("策略路由（默认）")}, {"disabled", Localized("策略路由（关闭重定向）")},
            {"enabled", Localized("启用 nftables 重定向")}}),
        form.Get().extra.auto_redirect == "enabled"
            ? SettingsContentRow(huxerui::Text(Localized("自动重定向需要系统 nftables 支持，与原生 VPN 补偿路由及路由标记不兼容。")))
            : huxerui::View{},
    }));
    std::vector<huxerui::View> ranges{
        NetworkSwitch(form, &Network::override_routes, Localized("自定义接管网段"),
            Localized("启用后仅接管所列网段；列表留空表示全部流量")),
    };
    if (form.Get().value.override_routes)
        ranges.push_back(NetworkField(form, "routes", Localized("接管网段（每行一个 CIDR）"), true));
    ranges.push_back(NetworkSwitch(form, &Network::override_exclusions, Localized("自定义排除网段"),
        Localized("排除的流量绕过内核，不再执行全局规则")));
    if (form.Get().value.override_exclusions)
        ranges.push_back(NetworkField(form, "exclusions", Localized("排除网段（每行一个 CIDR）"), true));
    sections.push_back(NetworkSection(ranges));
    if (kRouteSetNetwork) sections.push_back(NetworkSection({NetworkRouteSets(form)}));
    if (kLinuxNetwork) sections.push_back(NetworkLinuxFilters(form));
    if (kAndroidNetwork) sections.push_back(NetworkSection({NetworkApplications(form)}));
    return huxerui::Column(sections).With(huxerui::Spacing(20.0F),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

huxerui::View NetworkUdp(huxerui::State<NetworkForm> form) {
    const std::vector<Choice> nat = {{"", Localized("跟随配置")},
        {"endpoint_independent", Localized("端点无关")}, {"address_dependent", Localized("依赖远端地址")},
        {"address_and_port_dependent", Localized("依赖远端地址和端口")}};
    return huxerui::Column {
      NetworkSection({
        NetworkField(form, "udp_timeout", Localized("UDP 超时（例如 5m，留空跟随配置）")),
        NetworkSwitch(form, &Network::override_udp_nat_max, Localized("覆盖 UDP 会话上限"),
            Localized("0 表示由内核自动选择")),
        form.Get().value.override_udp_nat_max
            ? NetworkField(form, "udp_nat_max", Localized("UDP 会话上限")) : huxerui::View{},
      }),
      NetworkSection({
        NetworkChoice(form, &Network::udp_mapping, Localized("UDP NAT 映射"), nat),
        NetworkChoice(form, &Network::udp_filtering, Localized("UDP NAT 过滤"), nat),
      }),
    }.With(huxerui::Spacing(20.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

huxerui::View NetworkSystemProxy(huxerui::State<NetworkForm> form) {
    std::vector<huxerui::View> method{
        AdvancedChoice(form, &AdvancedNetwork::proxy_mode, Localized("系统代理方式"), {
            {"manual", Localized("手动代理")}, {"pac", Localized("本机 PAC")}, {"pac_url", Localized("自定义 PAC URL")}}),
    };
    if (form.Get().extra.proxy_mode != "pac_url") method.push_back(
        NetworkChoice(form, &Network::proxy_target, Localized("系统代理目标"), {
            {"mixed", Localized("混合监听")}, {"http", Localized("HTTP 监听")}}));
    if (form.Get().extra.proxy_mode == "pac") {
        method.push_back(NetworkField(form, "pac_port", Localized("PAC 服务端口（仅回环监听）")));
        method.push_back(NetworkField(form, "pac_direct_domains", Localized("PAC 直连域名（每行一个，支持通配符）"), true));
        method.push_back(SettingsContentRow(
            huxerui::Text(Localized("其它请求交给内核分流；连接失败不会自动直连。PAC 服务随系统接管开启和停止。"))));
    }
    if (form.Get().extra.proxy_mode == "pac_url") {
        method.push_back(NetworkField(form, "pac_url", Localized("PAC URL（HTTP 或 HTTPS）")));
        method.push_back(SettingsContentRow(
            huxerui::Text(Localized("脚本由系统或应用下载执行，请仅使用可信地址；内核规则不控制脚本选择的其它代理。"))));
    }
    std::vector<huxerui::View> bypass{
        NetworkSwitch(form, &Network::override_bypass, Localized("自定义代理绕过列表"),
            Localized("仅影响遵循系统代理的应用；按系统原生语义匹配")),
    };
    if (form.Get().value.override_bypass)
        bypass.push_back(NetworkField(form, "bypass", Localized("绕过主机、域名或 IP（每行一个）"), true));
    const bool authenticated = form.Get().value.override_auth &&
        (!form.Get().fields.at("username").text.empty() || !form.Get().fields.at("password").text.empty());
    if (authenticated && form.Get().extra.proxy_mode != "pac_url")
        method.push_back(SettingsContentRow(
            huxerui::Text(Localized("本地监听已设置认证，系统代理无法自动提供凭据。请在访问控制中检查认证设置。"))));
    return huxerui::Column {
      NetworkSection(method),
      NetworkSection(bypass),
      NetworkSection({
        NetworkSwitch(form, &Network::restore_proxy, Localized("恢复原系统代理"),
            Localized("关闭接管时恢复；系统设置已被他人修改则保留")),
      }),
    }.With(huxerui::Spacing(20.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

huxerui::View NetworkListeners(huxerui::State<NetworkForm> form) {
    return huxerui::Column {
      NetworkSection({
        NetworkSwitch(form, &Network::override_ports, Localized("覆盖配置中的监听端口"),
            Localized("关闭时保留原生配置中的端口；以下端口仍用于生成新监听")),
        NetworkField(form, "mixed_port", Localized("混合端口")),
        NetworkField(form, "http_port", Localized("HTTP 端口（留空关闭）")),
        NetworkField(form, "socks_port", Localized("SOCKS 端口（留空关闭）")),
      }),
      NetworkSection({
        NetworkSwitch(form, &Network::override_listen, Localized("覆盖监听地址"),
            Localized("留空时按局域网开关选择；关闭覆盖则保留原生配置")),
        form.Get().value.override_listen
            ? NetworkField(form, "listen_address", Localized("监听 IP（留空自动选择）")) : huxerui::View{},
        NetworkField(form, "bind_interface", Localized("绑定网络接口（留空跟随配置）")),
      }),
    }.With(huxerui::Spacing(20.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

huxerui::View NetworkAccess(huxerui::State<NetworkForm> form) {
    std::vector<huxerui::View> auth{
        NetworkSwitch(form, &Network::override_auth, Localized("覆盖代理认证"),
            Localized("两项留空表示取消认证；系统代理无法自动提供凭据")),
    };
    if (form.Get().value.override_auth) {
        auth.push_back(NetworkField(form, "username", Localized("用户名")));
        auth.push_back(NetworkField(form, "password", Localized("密码"), false, true));
    }
    return huxerui::Column {
      NetworkSection({
        NetworkSwitch(form, &Network::allow_lan, Localized("局域网连接"), Localized("允许局域网设备连接本地代理")),
        NetworkField(form, "allowed_sources", Localized("允许访问的网段（每行一个 CIDR，留空不限）"), true),
        SettingsContentRow(huxerui::Text(Localized("访问限制仅作用于本地代理监听，始终允许本机回环访问。"))),
      }),
      NetworkSection(auth),
    }.With(huxerui::Spacing(20.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View NetworkTabContent(huxerui::View content,
    std::shared_ptr<PageScrollOffset> offset, bool fullPage) {
    const auto controller = huxerui::UseScrollController(offset->value);
    const float inset = PageScrollInset(huxerui::UseTheme());
    return ScrollArea(huxerui::ScrollView(PageScrollContent(content, inset))
        .With(RememberPageScroll{offset}), controller, offset, fullPage ? inset : 2.0F).With(huxerui::Grow(1.0F));
}

struct NetworkSaveResult { std::string error; Network value; CoreView core; };
} // namespace

[[huxerui::composable]] huxerui::View NetworkSettingsEditor(bool tun, std::function<void()> onClose, bool fullPage,
    std::shared_ptr<std::function<void()>> dismissRequest) {
    const auto& theme = huxerui::UseTheme();
    const auto settings = huxerui::UseService<SettingsModel>();
    const auto control = UseCoreControl();
    const auto baseline = huxerui::UseState(settings->view.Get().network);
    const auto form = huxerui::UseState(NetworkForm(baseline.Get()));
    const auto selected = huxerui::UseState(std::size_t{0});
    const auto motion = UseSectionTabMotion();
    const auto scrolls = huxerui::UseState(std::vector<std::shared_ptr<PageScrollOffset>>{
        std::make_shared<PageScrollOffset>(), std::make_shared<PageScrollOffset>(),
        std::make_shared<PageScrollOffset>()});
    const auto alive = huxerui::UseState(true);
    huxerui::Lifecycle([alive] { return [alive] { alive = false; }; }, 0);
    const auto toast = UseFluxToast();
    Network parsed;
    const auto failure = form.Get().Read(parsed);
    const auto title = tun ? Localized("虚拟网卡设置") : kAndroidNetwork ? Localized("本地代理设置") : Localized("系统代理设置");
    const bool busy = control.Busy();
    const bool valid = failure.empty() && settings->view.Get().networkError.empty() && !busy;
    const bool canApply = control.ProxyActive();
    const auto save = [=](bool apply) {
        if (control.Busy()) return;
        Network next;
        if (const auto error = form.Get().Read(next); !error.empty()) { toast.Show(error); return; }
        const auto expected = baseline.Get();
        LaunchCoreOperation<NetworkSaveResult>(control.tasks, control.model, {.action = CoreAction::NetworkSettings},
            [next, expected, apply](CoreCommand) -> huxerui::Task<NetworkSaveResult> {
                co_return co_await RunOnTaskThread([=] {
                    auto& core = store::coreStore();
                    NetworkSaveResult result;
                    result.error = core.saveNetworkSettings(next, expected, apply);
                    result.value = core.networkSettings();
                    result.core = ReadCoreView(true, false);
                    return result;
                });
            }, [=](std::optional<NetworkSaveResult> result, std::exception_ptr error) {
                if (error || !result) {
                    try { if (error) std::rethrow_exception(error); }
                    catch (const std::exception& exception) { toast.Show(exception.what()); return; }
                    catch (...) {}
                    toast.Show(Localized("网络设置保存失败")); return;
                }
                settings->PublishNetwork(result->value);
                control.model->Update([&](CoreView& view) { view = result->core; });
                if (!result->error.empty()) { toast.Show(result->error); return; }
                toast.Show(apply && canApply ? Localized("网络设置已应用") : Localized("网络设置已保存，待下次连接或重启应用"));
                if (alive.Get()) onClose();
            });
    };
    const auto dialog = huxerui::UseDialog();
    const auto close = [=] {
        if (control.Busy()) return;
        if (!form.Get().Changed(baseline.Get())) { onClose(); return; }
        ConfirmDiscardChanges(dialog, onClose);
    };
    huxerui::Lifecycle([dismissRequest, close] {
        if (dismissRequest) *dismissRequest = close;
        return [dismissRequest] { if (dismissRequest) *dismissRequest = {}; };
    }, 0);

    std::vector<SectionTab> tabs;
    std::vector<huxerui::View> pages;
    const auto badge = [&](std::initializer_list<const char*> fields) -> std::string {
        return std::ranges::any_of(fields, [&](const char* field) { return !form.Get().FieldError(field).empty(); })
            ? "!" : "";
    };
    if (tun) {
        tabs = {{"basic", Localized("基础"), badge({"mtu", "interface_name", "addresses"})},
            {"scope", Localized("接管范围"), badge({"routes", "exclusions", "route_sets", "exclude_sets",
                "interfaces", "uids", "macs", "packages"})},
            {"udp", "UDP", badge({"udp_timeout", "udp_nat_max"})}};
        pages = {NetworkTunBasics(form), NetworkTunScope(form), NetworkUdp(form)};
    } else {
        if (!kAndroidNetwork) {
            tabs.push_back({"system", Localized("系统接管"),
                badge({"pac_port", "pac_url", "pac_direct_domains", "bypass"})});
            pages.push_back(NetworkSystemProxy(form));
        }
        tabs.push_back({"listen", Localized("本地监听"), badge({"mixed_port", "http_port", "socks_port",
            "listen_address", "bind_interface"})});
        tabs.push_back({"access", Localized("访问控制"), badge({"allowed_sources", "username", "password"})});
        pages.push_back(NetworkListeners(form)); pages.push_back(NetworkAccess(form));
    }
    for (std::size_t index = 0; index < pages.size(); ++index)
        pages[index] = NetworkTabContent(pages[index], scrolls.Get()[index], fullPage)
            .Key("network-page-" + tabs[index].key);
    const auto select = [selected](std::size_t index) { selected = index; };
    const auto tabBar = SectionTabBar(tabs, tabs[selected.Get()].key, [selected, tabs](const std::string& key) {
        const auto found = std::ranges::find(tabs, key, &SectionTab::key);
        if (found != tabs.end()) selected = static_cast<std::size_t>(found - tabs.begin());
    }, motion).Key("network-tabs");
    std::vector<huxerui::View> notices;
    if (canApply && control.model->view.Get().core.networkRevision != settings->view.Get().network.revision)
        notices.push_back(huxerui::Text(Localized("有待应用的网络设置")));
    if (!failure.empty()) notices.push_back(huxerui::Text(failure).Style(huxerui::TextStyle{
        huxerui::Font::System(font_size::kCaption), theme.colors.error}));
    if (!settings->view.Get().networkError.empty()) notices.push_back(huxerui::Text(settings->view.Get().networkError));
    const auto content = huxerui::Column {
      tabBar,
      notices.empty() ? huxerui::View{} : huxerui::View{huxerui::Column(notices).With(
          huxerui::Spacing(4.0F), huxerui::Padding(huxerui::EdgeInsets::Symmetric(fullPage ? 16.0F : 2.0F, 4.0F)),
          huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)).Key("network-notices")},
      huxerui::Column {
        huxerui::ProvideEnvironment(SectionTabContentInsets{2.0F},
            SectionTabPages(pages, selected.Get(), select, motion)),
      }.With(huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)).Key("network-pager"),
    }.With(huxerui::Grow(1.0F), huxerui::Enabled(!busy),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    const auto actions = DialogActions({
      fullPage ? huxerui::View{} : DialogAction(Localized("取消"), close, false, !busy),
      DialogAction(Localized("保存"), [save] { save(false); }, !canApply, valid).Key("network-save"),
      canApply ? DialogAction(Localized("保存并应用"), [save] { save(true); }, true, valid)
          .Key("network-save-apply") : huxerui::View{},
    });
    const auto saveMenu = UseActionMenu();
    if (fullPage) {
        const auto commit = [saveMenu, save, canApply] {
            if (!canApply) { save(false); return; }
            saveMenu.Show({
                ActionMenuItem(app::images::check, Localized("保存并应用"), [save] { save(true); }),
                ActionMenuItem(app::images::save, Localized("保存"), [save] { save(false); }),
            });
        };
        return SecondaryPageScaffold(huxerui::Text(title, huxerui::TextRole::Title),
            FormHeaderAction(app::images::save, Localized("保存"), commit, valid, busy)
                .With(saveMenu.Target()).Key("network-save"),
            FormPageContent(content), close, false, true, true);
    }
    return DialogScaffold(DialogHeader(title), content, actions, DialogSize::Large, true)
        .On<huxerui::ViewEvents::BackRequested>(close);
}

void ShowNetworkSettingsDialog(huxerui::DialogHandle dialog, bool tun) {
    auto request = std::make_shared<std::function<void()>>();
    dialog.Show([tun, request](huxerui::DialogContext context) {
        return NetworkSettingsEditor(tun, [context] { context.Dismiss(); }, false, request);
    }, {.on_dismiss_request = [request] { if (*request) (*request)(); }});
}
} // namespace clashflux::ui
