// profiles_page_forms.cpp — 订阅创建/编辑表单与移动端二级页面。
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
import clashflux.store.profiles;
import clashflux.store.vpn;
import clashflux.utils;
import clashflux.vpn;

#include "profiles_page_shared.h"

namespace clashflux::ui {


// 开关行：左标签（danger = error 色警示）+ 说明，右 Switch。Switch 卸载风险
// 不存在（原地改样式），OnChanged 内直接写 State 安全。
[[huxerui::composable]] huxerui::View ToggleRow(std::string label, std::string hint,
                                                bool danger,
                                                huxerui::State<bool> checked) {
    return SettingSwitchRow(
        label, hint,
        huxerui::Switch(checked.Get()).OnChanged(
            [checked](bool on) { checked = on; }),
        danger);
}

// 订阅表单中的平台字段由平台函数整体负责。Android 不需要也不显示桌面
// 系统代理、内核代理和证书绕过开关；桌面函数保持原有三项。
#if defined(__ANDROID__)

[[huxerui::composable]] huxerui::View AndroidProfileOptions(
    huxerui::State<bool>, huxerui::State<bool>, huxerui::State<bool>) {
    return {};
}

huxerui::View ProfilePlatformOptions(
    huxerui::State<bool> system_proxy, huxerui::State<bool> core_proxy,
    huxerui::State<bool> invalid_cert) {
    return AndroidProfileOptions(system_proxy, core_proxy, invalid_cert);
}

#else

[[huxerui::composable]] huxerui::View DesktopProfileOptions(
    huxerui::State<bool> system_proxy, huxerui::State<bool> core_proxy,
    huxerui::State<bool> invalid_cert) {
    return huxerui::Column {
        ToggleRow("使用系统代理更新", "经环境变量代理拉取订阅", false,
                  system_proxy),
        ToggleRow("使用内核代理更新", "经本应用内核混合端口拉取（内核需运行）",
                  false, core_proxy),
        ToggleRow("允许无效证书（危险）", "跳过 HTTPS 证书校验，仅用于可信来源",
                  true, invalid_cert),
    }.With(huxerui::Spacing(12.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

huxerui::View ProfilePlatformOptions(
    huxerui::State<bool> system_proxy, huxerui::State<bool> core_proxy,
    huxerui::State<bool> invalid_cert) {
    return DesktopProfileOptions(system_proxy, core_proxy, invalid_cert);
}

#endif

// 订阅选项表单（新建/编辑弹窗共用）：描述 + HTTP 超时/更新间隔 + 自动更新/
// 系统代理/内核代理/无效证书开关。字段值由调用方持有的 State 承载。
[[huxerui::composable]] huxerui::View ProfileOptionsForm(
    huxerui::State<huxerui::TextEditingValue> desc,
    huxerui::State<huxerui::TextEditingValue> timeout,
    huxerui::State<huxerui::TextEditingValue> interval,
    huxerui::State<bool> autoUp, huxerui::State<bool> sysProxy,
    huxerui::State<bool> coreProxy, huxerui::State<bool> invalidCert) {
    return huxerui::Column {
        huxerui::TextField(desc.Get())
            .Label("描述（可选）")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([desc](const huxerui::TextEditingValue& v) { desc = v; }),
        huxerui::Row {
            huxerui::TextField(timeout.Get())
                .Label("HTTP 超时（秒）")
                .Variant(huxerui::TextFieldVariant::Outlined)
                .OnChanged([timeout](const huxerui::TextEditingValue& v) {
                    timeout = v;
                })
                .With(huxerui::Grow(1.0F)),
            huxerui::TextField(interval.Get())
                .Label("更新间隔（分钟）")
                .Variant(huxerui::TextFieldVariant::Outlined)
                .OnChanged([interval](const huxerui::TextEditingValue& v) {
                    interval = v;
                })
                .With(huxerui::Grow(1.0F)),
        }
            .With(huxerui::Spacing(8.0F)),
        ToggleRow("允许自动更新", "开启后按更新间隔自动拉新（间隔需 > 0）", false,
                  autoUp),
        ProfilePlatformOptions(sysProxy, coreProxy, invalidCert),
    }
        .With(huxerui::Spacing(12.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// PPTP 原生订阅表单：只在订阅类型为 pptp 时显示。密码属于受控输入，
// 通过 PasswordField 统一提供显隐眼睛。
[[huxerui::composable]] huxerui::View PptpOptionsForm(
    huxerui::State<huxerui::TextEditingValue> server,
    huxerui::State<huxerui::TextEditingValue> username,
    huxerui::State<huxerui::TextEditingValue> password,
    huxerui::State<huxerui::TextEditingValue> timeout,
    huxerui::State<huxerui::TextEditingValue> routes,
    huxerui::State<bool> requireMppe) {
    return huxerui::Column {
        huxerui::TextField(server.Get())
            .Label("PPTP 服务器")
            .Placeholder("vpn.example.com")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([server](const huxerui::TextEditingValue& value) {
                server = value;
            }),
        huxerui::TextField(username.Get())
            .Label("用户名")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([username](const huxerui::TextEditingValue& value) {
                username = value;
            }),
        PasswordField(password),
        huxerui::TextField(timeout.Get())
            .Label("连接超时（秒）")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([timeout](const huxerui::TextEditingValue& value) {
                timeout = value;
            }),
        SettingSwitchRow(
            "要求 MPPE-128", "为 PPTP 连接启用 MPPE-128 加密",
            huxerui::Switch(requireMppe.Get())
                .OnChanged([requireMppe](bool checked) { requireMppe = checked; })),
        huxerui::TextField(routes.Get())
            .Label("内网 CIDR（逗号或换行分隔，可为空）")
            .Placeholder("10.20.0.0/16, 10.30.0.0/16")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([routes](const huxerui::TextEditingValue& value) {
                routes = value;
            }),
    }
        .With(huxerui::Spacing(12.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// OpenVPN 使用原生 .ovpn 文本；支持 inline ca/cert/key/auth-user-pass，避免
// 页面另造一套只覆盖部分 CLI 参数的配置模型。route 只由统一订阅路由字段管理。
[[huxerui::composable]] huxerui::View OpenVpnOptionsForm(
    huxerui::State<huxerui::TextEditingValue> config,
    huxerui::State<huxerui::TextEditingValue> routes) {
    return huxerui::Column {
        huxerui::TextField(config.Get())
            .Label("OpenVPN 配置（.ovpn 文本，支持 inline 证书）")
            .Placeholder("client\\ndev tun\\nremote vpn.example.com 1194 udp")
            .LineLimits(huxerui::TextFieldLineLimits::MultiLine(8, 16))
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([config](const huxerui::TextEditingValue& value) {
                config = value;
            }),
        huxerui::TextField(routes.Get())
            .Label("内网 CIDR（逗号或换行分隔，可为空）")
            .Placeholder("10.20.0.0/16, 10.30.0.0/16")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([routes](const huxerui::TextEditingValue& value) {
                routes = value;
            }),
    }
        .With(huxerui::Spacing(12.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// Android 端的编辑入口使用完整页面，避免弹窗被软键盘和窄视口挤压；桌面
// 仍由调用方使用 Dialog。表单 State 由 ProfilesPage 持有，页面与弹窗共用一份
// 数据和保存逻辑。

[[huxerui::composable]] huxerui::View ProfileCreatePage(
    ProfileCreateFields fields, huxerui::TaskScope tasks,
    huxerui::ToastHandle toast, std::shared_ptr<huxerui::FilePicker> picker,
    std::shared_ptr<huxerui::HttpClient> http, bool pptp_supported,
    bool openvpn_supported, std::function<void()> on_back) {
    const auto typeIndex = fields.type_index.Get();
    const bool remote = typeIndex == 0;
    const bool local = typeIndex == 1;
    const bool pptp = pptp_supported && typeIndex == 2;
    const bool openvpn = openvpn_supported &&
                         typeIndex == (pptp_supported ? 3U : 2U);
    std::vector<huxerui::StringVariant> types{"远程订阅", "本地文件"};
    if (pptp_supported) types.emplace_back("PPTP 内网");
    if (openvpn_supported) types.emplace_back("OpenVPN 内网");

    huxerui::View typeFields;
    if (remote) {
        typeFields = huxerui::TextField(fields.url.Get())
                         .Label("订阅链接")
                         .Placeholder("https://...")
                         .Variant(huxerui::TextFieldVariant::Outlined)
                         .OnChanged([url = fields.url](const huxerui::TextEditingValue& value) {
                             url = value;
                         });
    } else if (local) {
        typeFields = huxerui::Column {
            huxerui::Button("选择文件").OnClick(
                [tasks, picker, path = fields.picked_path] {
                    tasks.Launch([picker, path]() -> huxerui::Task<void> {
                        const auto picked = co_await picker->OpenFileAsync(
                            huxerui::FilePickerFilter{
                                .name = "YAML 订阅",
                                .extensions = {"yaml", "yml"}});
                        if (!picked) co_return;
                        if (const auto file = picked->AsFile()) path = file->Path();
                    });
                }),
            huxerui::Text(fields.picked_path.Get()),
        }.With(huxerui::Spacing(8.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    } else if (pptp) {
        typeFields = PptpOptionsForm(
            fields.pptp_server, fields.pptp_username, fields.pptp_password,
            fields.pptp_timeout, fields.pptp_routes, fields.pptp_mppe);
    } else {
        typeFields = OpenVpnOptionsForm(fields.openvpn_config,
                                        fields.openvpn_routes);
    }

    const auto importProfile = [=] {
        const std::string url = fields.url.Get().text;
        if (remote && url.empty()) {
            toast.Show("订阅链接不能为空");
            return;
        }
        if (local && fields.picked_path.Get().empty()) {
            toast.Show("请先选择订阅文件");
            return;
        }
        std::string configError;
        std::optional<std::string> nativeConfig;
        if (pptp) {
            nativeConfig = makePptpConfig(
                fields.pptp_server.Get(), fields.pptp_username.Get(),
                fields.pptp_password.Get(), fields.pptp_timeout.Get(),
                fields.pptp_mppe.Get(), configError);
            if (!nativeConfig) {
                toast.Show(configError);
                return;
            }
        } else if (openvpn) {
            if (!openvpn::ParseOpenVpnConfig(fields.openvpn_config.Get().text,
                                             configError)) {
                toast.Show(configError);
                return;
            }
            nativeConfig = fields.openvpn_config.Get().text;
        }
        fields.importing = true;
        tasks.Launch([=]() -> huxerui::Task<void> {
            db::Profile options;
            options.description = fields.desc.Get().text;
            if (!pptp && !openvpn) {
                options.timeoutSecs = parseNumber(fields.timeout.Get(), 60);
                options.intervalMins = parseNumber(fields.interval.Get(), 0);
                options.autoUpdate = fields.auto_update.Get();
                options.useSystemProxy = fields.system_proxy.Get();
                options.useCoreProxy = fields.core_proxy.Get();
                options.allowInvalidCert = fields.invalid_cert.Get();
            } else {
                options.type = pptp ? "pptp" : "openvpn";
                options.nativeConfig = *nativeConfig;
                options.nativeRoutes = joinRoutes(parseRouteField(
                    pptp ? fields.pptp_routes.Get().text
                         : fields.openvpn_routes.Get().text));
            }
            const auto [id, error] = co_await ImportProfileForPlatform(
                http, ProfileImportRequest{
                          .remote = remote,
                          .local = local,
                          .pptp = pptp,
                          .openvpn = openvpn,
                          .name = fields.name.Get().text,
                          .url = url,
                          .picked_path = fields.picked_path.Get(),
                          .options = std::move(options)});
            fields.importing = false;
            if (id == 0) {
                toast.Show(error.empty() ? "导入失败" : error);
                co_return;
            }
            toast.Show("订阅已导入");
            on_back();
        });
    };

    huxerui::View commonFields =
        (pptp || openvpn)
            ? huxerui::View{huxerui::TextField(fields.desc.Get())
                                .Label("描述（可选）")
                                .Variant(huxerui::TextFieldVariant::Outlined)
                                .OnChanged([desc = fields.desc](
                                               const huxerui::TextEditingValue& value) {
                                    desc = value;
                                })}
            : huxerui::View{ProfileOptionsForm(
                  fields.desc, fields.timeout, fields.interval,
                  fields.auto_update, fields.system_proxy, fields.core_proxy,
                  fields.invalid_cert)};

    return PageScaffold(
        "新建订阅",
        huxerui::Row {
            huxerui::IconButton(app::images::arrow_back, "返回")
                .With(huxerui::Tooltip("返回订阅列表"))
                .OnClick(on_back),
            fields.importing.Get()
                ? huxerui::View{huxerui::ProgressCircle().With(
                      huxerui::Frame{.width = 20.0F, .height = 20.0F})}
                : huxerui::View{huxerui::IconButton(app::images::add, "导入订阅")
                                    .With(huxerui::Tooltip("导入订阅"))
                                    .OnClick(importProfile)},
        }.With(huxerui::Spacing(8.0F)),
        huxerui::ScrollView(
            huxerui::Column {
                Card(huxerui::Column {
                    huxerui::SegmentedButton(types, typeIndex)
                        .OnChanged([type = fields.type_index](std::size_t index) {
                            type = index;
                        }),
                    typeFields,
                    huxerui::TextField(fields.name.Get())
                        .Label("名称（可选）")
                        .Variant(huxerui::TextFieldVariant::Outlined)
                        .OnChanged([name = fields.name](
                                       const huxerui::TextEditingValue& value) {
                            name = value;
                        }),
                    commonFields,
                }.With(huxerui::Spacing(12.0F),
                       huxerui::CrossAlign(
                           huxerui::CrossAxisAlignment::Stretch))),
                CompactFloatingNavigationFooter(),
            }.With(huxerui::Spacing(12.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
        ).With(huxerui::Grow(1.0F)));
}

[[huxerui::composable]] huxerui::View ProfileEditPage(
    std::int64_t id, huxerui::State<huxerui::TextEditingValue> name,
    huxerui::State<huxerui::TextEditingValue> url,
    huxerui::State<std::string> type,
    huxerui::State<huxerui::TextEditingValue> desc,
    huxerui::State<huxerui::TextEditingValue> timeout,
    huxerui::State<huxerui::TextEditingValue> interval,
    huxerui::State<bool> auto_update, huxerui::State<bool> system_proxy,
    huxerui::State<bool> core_proxy, huxerui::State<bool> invalid_cert,
    huxerui::State<huxerui::TextEditingValue> pptp_server,
    huxerui::State<huxerui::TextEditingValue> pptp_username,
    huxerui::State<huxerui::TextEditingValue> pptp_password,
    huxerui::State<huxerui::TextEditingValue> pptp_timeout,
    huxerui::State<huxerui::TextEditingValue> pptp_routes,
    huxerui::State<bool> pptp_mppe,
    huxerui::State<huxerui::TextEditingValue> openvpn_config,
    huxerui::State<huxerui::TextEditingValue> openvpn_routes,
    huxerui::TaskScope tasks, huxerui::ToastHandle toast,
    std::function<void()> on_back) {
    // 返回会切走 IndexedPages，从而卸载当前按钮节点。先让点击事件完成，避免
    // 事件派发与节点销毁发生在同一帧造成卡顿。
    const auto deferredBack = [tasks, on_back] {
        tasks.Launch([on_back]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            on_back();
        });
    };
    const auto save = [id, name, url, type, desc, timeout, interval, auto_update,
                       system_proxy, core_proxy, invalid_cert, pptp_server,
                       pptp_username, pptp_password, pptp_timeout, pptp_routes,
                       pptp_mppe, openvpn_config, openvpn_routes, tasks, toast,
                       on_back] {
        tasks.Launch([=]() -> huxerui::Task<void> {
            db::Profile fields;
            fields.name = name.Get().text;
            fields.type = type.Get();
            fields.description = desc.Get().text;
            fields.timeoutSecs = parseNumber(timeout.Get(), 60);
            fields.intervalMins = parseNumber(interval.Get(), 0);
            fields.autoUpdate = auto_update.Get();
            fields.useSystemProxy = system_proxy.Get();
            fields.useCoreProxy = core_proxy.Get();
            fields.allowInvalidCert = invalid_cert.Get();
            if (fields.type == "pptp") {
                std::string configError;
                const auto config = makePptpConfig(
                    pptp_server.Get(), pptp_username.Get(), pptp_password.Get(),
                    pptp_timeout.Get(), pptp_mppe.Get(), configError);
                if (!config) {
                    toast.Show(configError);
                    co_return;
                }
                fields.nativeConfig = *config;
                fields.nativeRoutes = joinRoutes(parseRouteField(pptp_routes.Get().text));
                fields.url.clear();
            } else if (fields.type == "openvpn") {
                std::string configError;
                if (!openvpn::ParseOpenVpnConfig(openvpn_config.Get().text,
                                                 configError)) {
                    toast.Show(configError);
                    co_return;
                }
                fields.nativeConfig = openvpn_config.Get().text;
                fields.nativeRoutes = joinRoutes(
                    parseRouteField(openvpn_routes.Get().text));
                fields.url.clear();
            } else {
                fields.url = url.Get().text;
            }
            const std::string err = co_await RunOnTaskThread(
                [id, fields] {
                    auto& ps = store::profilesStore();
                    if (!ps.updateProfile(id, fields)) return ps.lastError();
                    return std::string{};
                });
            if (err.empty()) {
                toast.Show("已保存");
                on_back();
            } else {
                toast.Show(err);
            }
        });
    };

    return PageScaffold(
        "编辑订阅",
        huxerui::IconButton(app::images::arrow_back, "返回")
            .With(huxerui::Tooltip("返回订阅列表"))
            .OnClick(deferredBack),
        huxerui::ScrollView(
            huxerui::Column {
                Card(huxerui::Column {
                    huxerui::Text(type.Get() == "pptp"
                                      ? "类型：PPTP 内网连接"
                                      : type.Get() == "openvpn"
                                      ? "类型：OpenVPN 内网连接"
                                      : type.Get() == "local" ? "类型：本地文件"
                                                               : "类型：远程订阅"),
                    huxerui::TextField(name.Get())
                        .Label("名称")
                        .Variant(huxerui::TextFieldVariant::Outlined)
                        .OnChanged([name](const huxerui::TextEditingValue& value) {
                            name = value;
                        }),
                    type.Get() == "pptp"
                        ? huxerui::View{PptpOptionsForm(
                              pptp_server, pptp_username, pptp_password,
                              pptp_timeout, pptp_routes, pptp_mppe)}
                        : type.Get() == "openvpn"
                        ? huxerui::View{OpenVpnOptionsForm(openvpn_config,
                                                           openvpn_routes)}
                        : huxerui::View{huxerui::Column {
                              huxerui::TextField(url.Get())
                                  .Label("订阅链接（留空 = 本地导入）")
                                  .Placeholder("https://...")
                                  .Variant(huxerui::TextFieldVariant::Outlined)
                                  .OnChanged([url](
                                                 const huxerui::TextEditingValue& value) {
                                      url = value;
                                  }),
                              ProfileOptionsForm(desc, timeout, interval,
                                                 auto_update, system_proxy,
                                                 core_proxy, invalid_cert),
                          }},
                    type.Get() == "pptp" || type.Get() == "openvpn"
                        ? huxerui::View{huxerui::TextField(desc.Get())
                                            .Label("描述（可选）")
                                            .Variant(huxerui::TextFieldVariant::Outlined)
                                            .OnChanged([desc](
                                                           const huxerui::TextEditingValue& value) {
                                                desc = value;
                                            })}
                        : huxerui::View{huxerui::Row{}},
                    huxerui::Row {
                        huxerui::Button("取消").OnClick(deferredBack),
                        huxerui::IconButton(app::images::save, "保存")
                            .With(huxerui::Tooltip("保存订阅"))
                            .OnClick(save),
                    }.With(huxerui::Spacing(8.0F),
                           huxerui::MainAlign(
                               huxerui::MainAxisAlignment::End)),
                }.With(huxerui::Spacing(12.0F),
                       huxerui::CrossAlign(
                           huxerui::CrossAxisAlignment::Stretch))),
                CompactFloatingNavigationFooter(),
            }
                .With(huxerui::Spacing(12.0F),
                      huxerui::CrossAlign(
                          huxerui::CrossAxisAlignment::Stretch))
                .With(huxerui::Grow(1.0F))));
}

[[huxerui::composable]] huxerui::View ResponsiveProfileEditSurface(
    std::int64_t id, ProfileEditFields fields, huxerui::TaskScope tasks,
    huxerui::ToastHandle toast, std::function<void()> on_back) {
    return huxerui::Scope(
        [id, fields, tasks, toast, on_back]() -> huxerui::View {
            // 非激活时必须保持空页：IndexedPages 常驻挂载每一页，而桌面编辑
            // 弹窗与本页绑定同一组表单 State——两个受控 TextField 同时挂载时，
            // 输入法组合值会被另一实例当作外部权威值而抛 std::invalid_argument。
            if (id == 0) return huxerui::Row{};
            return ProfileEditPage(
                id, fields.name, fields.url, fields.type, fields.desc,
                fields.timeout, fields.interval, fields.auto_update,
                fields.system_proxy, fields.core_proxy, fields.invalid_cert,
                fields.pptp_server, fields.pptp_username, fields.pptp_password,
                fields.pptp_timeout, fields.pptp_routes, fields.pptp_mppe,
                fields.openvpn_config, fields.openvpn_routes, tasks, toast,
                on_back);
        });
}
[[huxerui::composable]] huxerui::View ResponsiveProfileCreateSurface(
    bool open, ProfileCreateFields fields, huxerui::TaskScope tasks,
    huxerui::ToastHandle toast, std::shared_ptr<huxerui::FilePicker> picker,
    std::shared_ptr<huxerui::HttpClient> http, bool pptp_supported,
    bool openvpn_supported, std::function<void()> on_back) {
    return huxerui::Scope([=]() -> huxerui::View {
        // 同编辑页：未打开时不得挂载受控输入框（见 ResponsiveProfileEditSurface）。
        if (!open) return huxerui::Row{};
        return ProfileCreatePage(fields, tasks, toast, picker, http,
                                 pptp_supported, openvpn_supported, on_back);
    });
}
[[huxerui::composable]] huxerui::View ProfileFilePage(
    std::int64_t id, huxerui::State<std::shared_ptr<std::string>> content,
    huxerui::State<bool> loading, huxerui::TaskScope tasks,
    huxerui::ToastHandle toast, std::function<void()> on_back) {
    const auto save = [=] {
        if (loading.Get()) return;
        const std::string yaml = *content.Get();
        tasks.Launch([=]() -> huxerui::Task<void> {
            const std::string error = co_await RunOnTaskThread([id, yaml] {
                auto& profiles = store::profilesStore();
                return profiles.saveYaml(id, yaml) ? std::string{}
                                                    : profiles.lastError();
            });
            if (!error.empty()) {
                toast.Show(error);
                co_return;
            }
            toast.Show("已保存");
            on_back();
        });
    };
    huxerui::View editor = loading.Get()
        ? huxerui::View{huxerui::ProgressCircle()}
        : huxerui::View{huxerui::TextField(
              huxerui::TextEditingValue{*content.Get()})
              .Variant(huxerui::TextFieldVariant::Outlined)
              .LineLimits(huxerui::TextFieldLineLimits::MultiLine(18, 40))
              .OnChanged([content](const huxerui::TextEditingValue& value) {
                  *content.Get() = value.text;
              })};
    return PageScaffold(
        "编辑订阅文件",
        huxerui::Row {
            huxerui::IconButton(app::images::arrow_back, "返回")
                .With(huxerui::Tooltip("返回订阅列表"))
                .OnClick(on_back),
            huxerui::IconButton(app::images::save, "保存文件")
                .With(huxerui::Tooltip("保存订阅文件"))
                .OnClick(save),
        }.With(huxerui::Spacing(8.0F)),
        huxerui::Column {
            huxerui::Text("直接编辑订阅 YAML；保存后启用中的订阅会重启生效。"),
            editor,
            CompactFloatingNavigationFooter(),
        }.With(huxerui::Spacing(12.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
               huxerui::Grow(1.0F)));
}
[[huxerui::composable]] huxerui::View ProfileRulesPage(
    std::int64_t id, huxerui::State<std::string> yaml,
    huxerui::StateList<std::string> rules,
    huxerui::State<huxerui::TextEditingValue> input,
    huxerui::State<bool> dirty, huxerui::TaskScope tasks,
    huxerui::ToastHandle toast, std::function<void()> on_back) {
    // 同编辑页：未打开时不得挂载受控输入框（见 ResponsiveProfileEditSurface）。
    if (id == 0) return huxerui::Row{};
    const auto add = [=](bool prepend) {
        const std::string text = input.Get().text;
        if (text.empty()) return;
        const std::string updated = store::insertRule(yaml.Get(), text, prepend);
        yaml = updated;
        ReplaceStateList(rules, store::parseRules(updated));
        input = huxerui::TextEditingValue{""};
        dirty = true;
    };
    const auto save = [=] {
        if (!dirty.Get()) {
            on_back();
            return;
        }
        const std::string content = yaml.Get();
        tasks.Launch([=]() -> huxerui::Task<void> {
            const std::string error = co_await RunOnTaskThread([id, content] {
                auto& profiles = store::profilesStore();
                return profiles.saveYaml(id, content) ? std::string{}
                                                       : profiles.lastError();
            });
            if (!error.empty()) {
                toast.Show(error);
                co_return;
            }
            toast.Show("规则已保存");
            on_back();
        });
    };
    huxerui::View list = rules.Empty()
        ? huxerui::View{huxerui::Text("订阅没有 rules 规则")}
        : huxerui::View{huxerui::VirtualList(
              rules.Size(), [rules](std::size_t index) {
                  return huxerui::Text(std::format("{}  {}", index + 1,
                                                   rules[index]))
                      .Key(std::to_string(index));
              })
              .ItemExtent(32.0F)
              .With(huxerui::Grow(1.0F), huxerui::ScrollBar())};
    return PageScaffold(
        "编辑订阅规则",
        huxerui::Row {
            huxerui::IconButton(app::images::arrow_back, "返回")
                .With(huxerui::Tooltip("返回订阅列表"))
                .OnClick(on_back),
            huxerui::IconButton(app::images::save, "保存规则")
                .With(huxerui::Tooltip("保存订阅规则"))
                .OnClick(save),
        }.With(huxerui::Spacing(8.0F)),
        huxerui::Column {
            huxerui::Text("前置插入列表头，后置追加到列表尾。"),
            list,
            huxerui::TextField(input.Get())
                .Label("规则，如 DOMAIN-SUFFIX,example.com,代理组")
                .Variant(huxerui::TextFieldVariant::Outlined)
                .OnChanged([input](const huxerui::TextEditingValue& value) {
                    input = value;
                }),
            huxerui::Row {
                huxerui::Button("前置").OnClick([add] { add(true); }),
                huxerui::Button("后置").OnClick([add] { add(false); }),
            }.With(huxerui::Spacing(8.0F),
                   huxerui::MainAlign(huxerui::MainAxisAlignment::End)),
            CompactFloatingNavigationFooter(),
        }.With(huxerui::Spacing(12.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
               huxerui::Grow(1.0F)));
}

} // namespace clashflux::ui
