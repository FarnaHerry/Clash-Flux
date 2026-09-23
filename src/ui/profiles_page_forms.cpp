// profiles_page_forms.cpp — 订阅创建/编辑表单与移动端二级页面。
#include <huxerui/huxerui.h>
#include <huxerui/camera.h>

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
#if defined(__ANDROID__)
#include "qr_photo_decoder.h"
#endif

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

namespace {

huxerui::PageTransition ProfileSecondaryTransition(
    const huxerui::MotionScheme& motion) {
    const huxerui::TransitionSpec enter{
        huxerui::SlideTransition{.incoming_offset = {1.0F, 0.0F},
                                 .outgoing_offset = {-0.2F, 0.0F}},
        huxerui::TweenSpec{.duration = motion.slow,
                           .easing = huxerui::Easing::EaseOut}};
    return huxerui::PageTransition{
        .push = enter,
        .pop = enter.Reversed(huxerui::TweenSpec{
            .duration = motion.normal, .easing = huxerui::Easing::EaseOut}),
        .replace = enter};
}

bool IsHttpProfileUrl(const std::string& value) {
    return value.starts_with("https://") || value.starts_with("http://");
}

huxerui::Task<std::pair<std::int64_t, std::string>> ImportProfileContent(
    std::string name, std::string content, db::Profile options) {
    co_return co_await RunOnTaskThread(
        [name = std::move(name), content = std::move(content),
         options = std::move(options)] {
            auto& profiles = store::profilesStore();
            const std::int64_t id =
                profiles.importContent(name, content, options);
            return std::pair{id, profiles.lastError()};
        });
}

} // namespace


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
            "要求 MPPE-128", "勾选后强制 MPPE-128；不勾选时按服务端要求协商",
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
                                .name = "sing-box / Clash 配置",
                                .extensions = {"json", "yaml", "yml"}});
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
            toast.Show("配置已导入");
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

[[huxerui::composable]] huxerui::View ProfileFlowPage(
    huxerui::View title, huxerui::View actions, huxerui::View content,
    std::function<void()> on_back) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return SecondaryPageScaffold(title, actions, content, std::move(on_back))
        .With(ProfileSecondaryTransition(theme.motion));
}

[[huxerui::composable]] huxerui::View ProfileAddMethodRow(
    huxerui::ImageResource icon, std::string name, std::string hint,
    std::string key, bool divider, std::function<void()> on_click) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    huxerui::View row = UnifiedListRow(
        huxerui::Row {
            huxerui::Image(icon)
                .Tint(theme.colors.on_surface)
                .With(huxerui::Frame{.width = 26.0F, .height = 26.0F}),
            huxerui::Column {
                huxerui::Text(name).Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kBody),
                    theme.colors.on_surface}),
                huxerui::Text(hint).Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    theme.colors.on_surface_variant}),
            }.With(huxerui::Spacing(3.0F), huxerui::Grow(1.0F)),
        }.With(huxerui::Spacing(16.0F),
               huxerui::Padding(huxerui::EdgeInsets::Symmetric(8.0F, 14.0F)),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        std::move(key), true, divider);
    return std::move(row).OnClick(on_click)
        .With(huxerui::Focusable(true),
              huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                 .label = std::move(name)});
}

#if defined(__ANDROID__)

[[huxerui::composable]] huxerui::View ProfileQrScannerPage(
    std::function<void()> on_back,
    std::function<void(std::string)> on_result) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const huxerui::ApplicationHandle application = huxerui::UseApplication();
    const std::shared_ptr<QrPhotoDecoder> decoder =
        huxerui::UseService<QrPhotoDecoder>();
    const huxerui::TaskScope tasks = huxerui::UseTaskScope();
    auto active = huxerui::UseState(false);
    auto scanning = huxerui::UseState(false);
    auto decoding = huxerui::UseState(false);
    auto detected = huxerui::UseState(std::string{});
    auto message = huxerui::UseState(std::string{"正在准备相机…"});
    const huxerui::camera::CameraSession session =
        huxerui::camera::UseCamera({
            .facing = huxerui::camera::Facing::Back,
            .active = active.Get(),
        });
    const auto pending_request =
        std::make_shared<std::optional<huxerui::PlatformRequestId>>();

    const auto start_scanning = [=] {
        if (scanning.Get()) return huxerui::TaskHandle{};
        scanning = true;
        message = std::string{"正在请求相机权限…"};
        detected = std::string{};
        return tasks.Launch([=]() -> huxerui::Task<void> {
            const huxerui::PermissionStatus permission =
                co_await application.RequestPermissionAsync(
                    huxerui::Permission::Camera);
            if (permission != huxerui::PermissionStatus::Granted) {
                message = permission == huxerui::PermissionStatus::Unavailable
                              ? "当前设备无法使用相机"
                              : "请允许相机权限后重试";
                scanning = false;
                co_return;
            }

            if (session.Status().state ==
                huxerui::camera::SessionState::Failed) {
                session.Retry();
            }
            active = true;
            message = std::string{"正在启动相机…"};

            while (scanning.Get()) {
                const huxerui::camera::CameraStatus status = session.Status();
                if (status.state == huxerui::camera::SessionState::Failed) {
                    message = status.error ? status.error->message
                                           : "相机启动失败，请重试";
                    active = false;
                    scanning = false;
                    co_return;
                }
                if (status.state != huxerui::camera::SessionState::Running) {
                    co_await huxerui::Delay(
                        std::chrono::milliseconds{120});
                    continue;
                }

                message = std::string{"将二维码放入取景画面，识别后自动导入"};
                const auto photo = co_await session.CapturePhotoAsync(
                    {.jpeg_quality = 50,
                     .mirror = huxerui::camera::MirrorMode::Off});
                if (!photo.Succeeded()) {
                    const auto code = photo.Error().code;
                    if (code != huxerui::camera::CameraErrorCode::NotReady &&
                        code != huxerui::camera::CameraErrorCode::Interrupted &&
                        code != huxerui::camera::CameraErrorCode::OperationInProgress) {
                        message = photo.Error().message;
                        active = false;
                        scanning = false;
                        co_return;
                    }
                    co_await huxerui::Delay(
                        std::chrono::milliseconds{180});
                    continue;
                }

                const auto encoded = photo.Value().EncodedBytes();
                huxerui::Bytes jpeg(encoded.begin(), encoded.end());
                decoding = true;
                const huxerui::PlatformRequestId request = decoder->Decode(
                    std::move(jpeg),
                    [decoding, detected, message](
                        huxerui::PlatformResult<std::string> result) {
                        decoding = false;
                        if (const auto* error =
                                std::get_if<huxerui::PlatformError>(&result)) {
                            message = error->message;
                            return;
                        }
                        const std::string content =
                            std::get<std::string>(std::move(result));
                        if (!content.empty()) {
                            detected = content;
                        }
                    });
                *pending_request = request;

                while (scanning.Get() && decoding.Get()) {
                    co_await huxerui::Delay(
                        std::chrono::milliseconds{25});
                }
                pending_request->reset();
                if (!scanning.Get()) co_return;
                if (!detected.Get().empty()) {
                    const std::string content = detected.Get();
                    active = false;
                    scanning = false;
                    on_result(content);
                    co_return;
                }
                co_await huxerui::Delay(
                    std::chrono::milliseconds{380});
            }
        });
    };

    huxerui::Lifecycle([=] {
        const huxerui::TaskHandle request = start_scanning();
        return [request, active, scanning, decoder, pending_request] {
            request.Cancel();
            scanning = false;
            active = false;
            if (*pending_request) {
                decoder->Cancel(**pending_request);
                pending_request->reset();
            }
        };
    });

    const huxerui::camera::CameraStatus status = session.Status();
    huxerui::View preview;
    if (status.preview) {
        preview = huxerui::camera::CameraPreview(
            session, {.fit = huxerui::ImageFit::Cover,
                      .mirror = huxerui::camera::MirrorMode::Off});
    } else {
        preview = huxerui::Column {
            scanning.Get() ? huxerui::View{huxerui::ProgressCircle()}
                           : huxerui::View{huxerui::Image(app::images::qr_scan)
                                               .Tint(theme.colors.on_surface_variant)
                                               .With(huxerui::Frame{.width = 36.0F,
                                                                    .height = 36.0F})},
            huxerui::Text(message.Get())
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kBody),
                    theme.colors.on_surface_variant}),
        }.With(huxerui::Spacing(12.0F),
               huxerui::Padding(20.0F),
               huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    }
    preview = std::move(preview).With(
        huxerui::Frame{.height = 400.0F},
        huxerui::Background(theme.colors.surface_container_high),
        huxerui::CornerRadius(theme.shapes.large),
        huxerui::ClipChildren());

    huxerui::View body = huxerui::Column {
        std::move(preview),
        huxerui::Text(message.Get())
            .Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody),
                theme.colors.on_surface_variant}),
        !scanning.Get() && !detected.Get().empty()
            ? huxerui::View{huxerui::Text("正在导入配置…")
                                .Style(huxerui::TextStyle{
                                    huxerui::Font::System(font_size::kCaption),
                                    theme.colors.on_surface_variant})}
            : huxerui::View{huxerui::Row{}},
        !scanning.Get()
            ? huxerui::View{huxerui::Button("重试")
                                .OnClick(start_scanning)}
            : huxerui::View{huxerui::Row{}},
    }.With(huxerui::Spacing(12.0F),
           huxerui::Padding(huxerui::EdgeInsets::Symmetric(16.0F, 12.0F)),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    return ProfileFlowPage(
        huxerui::Text("扫描二维码", huxerui::TextRole::Title),
        huxerui::View{}, std::move(body), std::move(on_back));
}

void PushProfileQrScanner(
    ProfileCreateFields fields, huxerui::NavigationController navigation,
    huxerui::ToastHandle toast,
    std::function<void(std::string)> on_result) {
    static_cast<void>(toast);
    fields.importing = true;
    navigation.Push([=] {
        return ProfileQrScannerPage(
            [navigation, fields] {
                fields.importing = false;
                static_cast<void>(navigation.Pop());
            },
            [navigation, fields, on_result](std::string content) mutable {
                fields.importing = false;
                static_cast<void>(navigation.Pop());
                if (on_result) on_result(std::move(content));
            });
    });
}

#else

void PushProfileQrScanner(
    ProfileCreateFields, huxerui::NavigationController,
    huxerui::ToastHandle toast,
    std::function<void(std::string)>) {
    toast.Show("当前平台暂不支持二维码扫描");
}

#endif

[[huxerui::composable]] huxerui::View ProfileAddMethodPage(
    ProfileCreateFields fields, huxerui::TaskScope tasks,
    huxerui::ToastHandle toast, std::shared_ptr<huxerui::FilePicker> picker,
    std::shared_ptr<huxerui::HttpClient> http,
    bool pptp_supported, bool openvpn_supported,
    huxerui::NavigationController navigation) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto dialog = huxerui::UseDialog();
    auto urlInput = huxerui::UseState(huxerui::TextEditingValue{""});
    const auto on_back = [navigation] {
        static_cast<void>(navigation.Pop());
    };
    const auto finishImport = [fields, toast, navigation](
                                  ProfileImportResult result) {
        fields.importing = false;
        if (result.first == 0) {
            toast.Show(result.second.empty() ? "导入失败" : result.second);
            return;
        }
        toast.Show("配置已添加");
        static_cast<void>(navigation.Pop());
    };
    const auto importRemote = [fields, tasks, http, finishImport](
                                  std::string url) {
        if (fields.importing.Get()) return;
        fields.importing = true;
        db::Profile options;
        options.autoUpdate = true;
        options.intervalMins = 1440;
        options.timeoutSecs = 60;
        tasks.Launch([http, url = std::move(url), options,
                      finishImport]() mutable -> huxerui::Task<void> {
            ProfileImportRequest request;
            request.remote = true;
            request.url = std::move(url);
            request.options = options;
            finishImport(co_await ImportProfileForPlatform(
                http, std::move(request)));
        });
    };
    const auto importInline = [fields, tasks, finishImport](
                                  std::string content) {
        if (fields.importing.Get()) return;
        fields.importing = true;
        db::Profile options;
        tasks.Launch([content = std::move(content), options,
                      finishImport]() mutable -> huxerui::Task<void> {
            const auto [id, error] = co_await ImportProfileContent(
                "二维码配置", std::move(content), options);
            finishImport(ProfileImportResult{id, error});
        });
    };
    const auto beginQrScan = [fields, navigation, toast,
                              importRemote, importInline] {
        if (fields.importing.Get()) return;
        PushProfileQrScanner(
            fields, navigation, toast,
            [importRemote, importInline](std::string content) mutable {
                if (IsHttpProfileUrl(content)) {
                    importRemote(std::move(content));
                } else {
                    importInline(std::move(content));
                }
            });
    };
    const auto openDirect = [fields, tasks, toast, picker, http,
                             pptp_supported, openvpn_supported, navigation,
                             on_back, popDuration = theme.motion.normal] {
        if (fields.importing.Get()) return;
        fields.config_content = huxerui::TextEditingValue{""};
        fields.type_index = 0;
        const auto on_complete = [tasks, navigation, popDuration] {
            tasks.Launch([navigation, popDuration]() -> huxerui::Task<void> {
                if (navigation.Depth() < 3) co_return;
                static_cast<void>(navigation.Pop());
                co_await huxerui::Delay(
                    std::chrono::duration<double>{popDuration});
                if (navigation.Depth() > 1) {
                    static_cast<void>(navigation.Pop());
                }
            });
        };
        navigation.Push([=] {
            return ProfileCreateMethodPage(
                ProfileAddMethod::Direct, fields, tasks, toast, picker, http,
                pptp_supported, openvpn_supported, navigation, on_back,
                on_complete);
        });
    };
    const auto chooseFile = [fields, tasks, toast, picker, http, finishImport] {
        if (fields.importing.Get()) return;
        if (!picker) {
            toast.Show("文件选择器不可用");
            return;
        }
        fields.importing = true;
        tasks.Launch([fields, picker, http, finishImport]() mutable
                         -> huxerui::Task<void> {
            const auto picked = co_await picker->OpenFileAsync(
                huxerui::FilePickerFilter{
                    .name = "sing-box / Clash 配置",
                    .extensions = {"json", "yaml", "yml"}});
            if (!picked) {
                fields.importing = false;
                co_return;
            }
            const auto file = picked->AsFile();
            if (!file) {
                fields.importing = false;
                co_return;
            }
            const std::string path = file->Path();
            fields.picked_path = path;
            const std::size_t slash = path.find_last_of("/\\");
            std::string name = path.substr(
                slash == std::string::npos ? 0 : slash + 1);
            const std::size_t extension = name.find_last_of('.');
            if (extension != std::string::npos && extension > 0) {
                name.erase(extension);
            }
            ProfileImportRequest request;
            request.local = true;
            request.name = std::move(name);
            request.picked_path = path;
            finishImport(co_await ImportProfileForPlatform(
                http, std::move(request)));
        });
    };
    const auto openUrlDialog = [fields, dialog, urlInput, toast,
                                importRemote] {
        if (fields.importing.Get()) return;
        urlInput = huxerui::TextEditingValue{""};
        dialog.Show(
            [urlInput, toast, importRemote](
                huxerui::DialogContext ctx) -> huxerui::View {
                return DialogCard(huxerui::Column {
                    huxerui::Text("添加订阅", huxerui::TextRole::Title),
                    huxerui::TextField(urlInput.Get())
                        .Label("订阅 URL")
                        .Placeholder("https://...")
                        .Variant(huxerui::TextFieldVariant::Outlined)
                        .OnChanged([urlInput](
                                       const huxerui::TextEditingValue& value) {
                            urlInput = value;
                        }),
                    huxerui::Row {
                        huxerui::Button("取消").OnClick(
                            [ctx] { ctx.Dismiss(); }),
                        huxerui::Button("获取配置").OnClick(
                            [ctx, urlInput, toast, importRemote] {
                                const std::string url = urlInput.Get().text;
                                if (!IsHttpProfileUrl(url)) {
                                    toast.Show("请输入有效的 HTTP 或 HTTPS URL");
                                    return;
                                }
                                ctx.Dismiss();
                                importRemote(url);
                            }),
                    }.With(huxerui::Spacing(8.0F),
                           huxerui::MainAlign(
                               huxerui::MainAxisAlignment::End)),
                }.With(huxerui::Spacing(12.0F),
                       huxerui::Frame{.width = 300.0F},
                       huxerui::CrossAlign(
                           huxerui::CrossAxisAlignment::Stretch)));
            },
            huxerui::DialogOptions{});
    };

    huxerui::View methods = huxerui::Column {
        ProfileAddMethodRow(
            app::images::qr_scan, "二维码",
            fields.importing.Get() ? "正在扫描或导入配置…"
                                  : "扫描后自动导入订阅链接或配置",
            "profile-add-qr", true,
            beginQrScan),
        ProfileAddMethodRow(
            app::images::file_import, "文件",
            fields.importing.Get() ? "正在导入配置…"
                                  : "选择 sing-box JSON 或 Clash YAML 配置文件",
            "profile-add-file", true,
            chooseFile),
        ProfileAddMethodRow(
            app::images::link, "URL", "输入订阅链接并自动获取配置",
            "profile-add-url", true,
            openUrlDialog),
        ProfileAddMethodRow(
            app::images::manual_config, "直接配置", "粘贴配置内容或填写 VPN 参数",
            "profile-add-direct", false,
            openDirect),
    }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    huxerui::View content = huxerui::ScrollView(huxerui::Column {
        huxerui::Text("选择添加方式")
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption),
                                      theme.colors.on_surface_variant}),
        methods,
        CompactFloatingNavigationFooter(),
    }.With(huxerui::Spacing(8.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
        .With(huxerui::Grow(1.0F), huxerui::ScrollBar());
    return ProfileFlowPage(huxerui::Text("添加配置", huxerui::TextRole::Title),
                            huxerui::View{}, std::move(content), on_back)
        .With(huxerui::Semantics{.role = huxerui::SemanticRole::Navigation,
                                 .label = "添加配置"});
}

[[huxerui::composable]] huxerui::View ProfileCreateMethodPage(
    ProfileAddMethod method, ProfileCreateFields fields,
    huxerui::TaskScope tasks, huxerui::ToastHandle toast,
    std::shared_ptr<huxerui::FilePicker> picker,
    std::shared_ptr<huxerui::HttpClient> http,
    bool pptp_supported, bool openvpn_supported,
    huxerui::NavigationController navigation, std::function<void()> on_back,
    std::function<void()> on_complete) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const std::string qrContent = fields.qr_content.Get().text;
    const bool qrRemote = method == ProfileAddMethod::Qr &&
                          IsHttpProfileUrl(qrContent);
    const bool isRemote = method == ProfileAddMethod::Url || qrRemote;
    const bool isFile = method == ProfileAddMethod::File;
    const bool isQr = method == ProfileAddMethod::Qr;
    const bool isDirect = method == ProfileAddMethod::Direct;
    const std::size_t directIndex = fields.type_index.Get();
    const bool directPptp = isDirect && pptp_supported && directIndex == 1;
    const bool directOpenVpn =
        isDirect && openvpn_supported &&
        directIndex == (pptp_supported ? 2U : 1U);

    const auto import_profile = [=] {
        std::string url;
        std::string inlineConfig;
        bool remote = isRemote;
        bool local = isFile;
        bool pptp = directPptp;
        bool openvpn = directOpenVpn;
        if (method == ProfileAddMethod::Url) {
            url = fields.url.Get().text;
        } else if (isQr) {
            url = qrContent;
            if (!IsHttpProfileUrl(qrContent)) {
                remote = false;
                inlineConfig = qrContent;
            }
        } else if (isDirect && !pptp && !openvpn) {
            inlineConfig = fields.config_content.Get().text;
        }
        if ((remote && url.empty()) || (isQr && qrContent.empty())) {
            toast.Show(isQr ? "请先扫描二维码" : "订阅 URL 不能为空");
            return;
        }
        if (local && fields.picked_path.Get().empty()) {
            toast.Show("请先选择配置文件");
            return;
        }
        if ((isDirect && !pptp && !openvpn) ||
            (isQr && !IsHttpProfileUrl(qrContent))) {
            if (inlineConfig.empty()) {
                toast.Show("配置内容不能为空");
                return;
            }
        }

        db::Profile options;
        options.description = fields.desc.Get().text;
        if (remote) {
            options.timeoutSecs = parseNumber(fields.timeout.Get(), 60);
            options.intervalMins = parseNumber(fields.interval.Get(), 1440);
            options.autoUpdate = fields.auto_update.Get();
            options.useSystemProxy = fields.system_proxy.Get();
            options.useCoreProxy = fields.core_proxy.Get();
            options.allowInvalidCert = fields.invalid_cert.Get();
        } else if (pptp) {
            std::string configError;
            const auto config = makePptpConfig(
                fields.pptp_server.Get(), fields.pptp_username.Get(),
                fields.pptp_password.Get(), fields.pptp_timeout.Get(),
                fields.pptp_mppe.Get(), configError);
            if (!config) {
                toast.Show(configError);
                return;
            }
            options.type = "pptp";
            options.nativeConfig = *config;
            options.nativeRoutes = joinRoutes(
                parseRouteField(fields.pptp_routes.Get().text));
        } else if (openvpn) {
            std::string configError;
            if (!openvpn::ParseOpenVpnConfig(fields.openvpn_config.Get().text,
                                             configError)) {
                toast.Show(configError);
                return;
            }
            options.type = "openvpn";
            options.nativeConfig = fields.openvpn_config.Get().text;
            options.nativeRoutes = joinRoutes(
                parseRouteField(fields.openvpn_routes.Get().text));
        }

        fields.importing = true;
        const std::string name = fields.name.Get().text;
        const std::string pickedPath = fields.picked_path.Get();
        tasks.Launch([=]() mutable -> huxerui::Task<void> {
            std::pair<std::int64_t, std::string> result;
            if (!inlineConfig.empty()) {
                result = co_await ImportProfileContent(
                    name.empty() ? "手动配置" : name, inlineConfig, options);
            } else {
                result = co_await ImportProfileForPlatform(
                    http, ProfileImportRequest{
                              .remote = remote,
                              .local = local,
                              .pptp = pptp,
                              .openvpn = openvpn,
                              .name = name,
                              .url = url,
                              .picked_path = pickedPath,
                              .options = options});
            }
            fields.importing = false;
            if (result.first == 0) {
                toast.Show(result.second.empty() ? "导入失败" : result.second);
                co_return;
            }
            toast.Show("配置已导入");
            on_complete();
        });
    };

    huxerui::View typeFields;
    if (method == ProfileAddMethod::Url) {
        typeFields = huxerui::TextField(fields.url.Get())
                         .Label("订阅 URL")
                         .Placeholder("https://...")
                         .Variant(huxerui::TextFieldVariant::Outlined)
                         .OnChanged([url = fields.url](
                                        const huxerui::TextEditingValue& value) {
                             url = value;
                         });
    } else if (isFile) {
        typeFields = huxerui::Column {
            huxerui::Button("选择配置文件").OnClick(
                [tasks, picker, path = fields.picked_path] {
                    tasks.Launch([picker, path]() -> huxerui::Task<void> {
                        const auto picked = co_await picker->OpenFileAsync(
                            huxerui::FilePickerFilter{
                                .name = "sing-box / Clash 配置",
                                .extensions = {"json", "yaml", "yml"}});
                        if (!picked) co_return;
                        if (const auto file = picked->AsFile()) path = file->Path();
                    });
                }),
            huxerui::Text(fields.picked_path.Get())
                .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption),
                                          theme.colors.on_surface_variant}),
        }.With(huxerui::Spacing(8.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    } else if (isQr) {
        const bool scanned = !qrContent.empty();
        typeFields = huxerui::Column {
            huxerui::Text("将二维码放入取景框，支持订阅链接和配置文本。")
                .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                          theme.colors.on_surface_variant}),
            huxerui::Button(scanned ? "重新扫描" : "开始扫码")
                .OnClick([fields, navigation, toast,
                          content = fields.qr_content] {
                    PushProfileQrScanner(
                        fields, navigation, toast,
                        [content](std::string result) {
                            content = huxerui::TextEditingValue{
                                std::move(result)};
                        });
                }),
            scanned
                ? huxerui::View{huxerui::Text(
                      qrRemote ? "已识别订阅链接"
                               : std::format("已识别配置文本 · {} 字符",
                                             qrContent.size()))
                      .Style(huxerui::TextStyle{
                          huxerui::Font::System(font_size::kCaption),
                          theme.colors.on_surface_variant})}
                : huxerui::View{huxerui::Row{}},
        }.With(huxerui::Spacing(12.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    } else if (isDirect && directPptp) {
        typeFields = PptpOptionsForm(
            fields.pptp_server, fields.pptp_username, fields.pptp_password,
            fields.pptp_timeout, fields.pptp_routes, fields.pptp_mppe);
    } else if (isDirect && directOpenVpn) {
        typeFields = OpenVpnOptionsForm(fields.openvpn_config,
                                        fields.openvpn_routes);
    } else if (isDirect) {
        typeFields = huxerui::TextField(fields.config_content.Get())
                         .Label("代理配置内容")
                         .Placeholder("粘贴 sing-box JSON 或 Clash YAML 配置")
                         .LineLimits(huxerui::TextFieldLineLimits::MultiLine(12, 28))
                         .Variant(huxerui::TextFieldVariant::Outlined)
                         .OnChanged([config = fields.config_content](
                                        const huxerui::TextEditingValue& value) {
                             config = value;
                         });
    }

    std::vector<huxerui::StringVariant> directTypes{"代理配置"};
    if (pptp_supported) directTypes.emplace_back("PPTP 内网");
    if (openvpn_supported) directTypes.emplace_back("OpenVPN 内网");
    huxerui::View directTypeSelector =
        isDirect && directTypes.size() > 1
            ? huxerui::View{huxerui::SegmentedButton(directTypes, directIndex)
                                .OnChanged([type = fields.type_index](
                                               std::size_t index) {
                                    type = index;
                                })}
            : huxerui::View{huxerui::Row{}};

    huxerui::View commonFields;
    bool hasCommonFields = false;
    if (isRemote || (isQr && qrRemote)) {
        commonFields = ProfileOptionsForm(
            fields.desc, fields.timeout, fields.interval, fields.auto_update,
            fields.system_proxy, fields.core_proxy, fields.invalid_cert);
        hasCommonFields = true;
    } else if (method == ProfileAddMethod::Url || isFile ||
               (isQr && !qrRemote) || isDirect) {
        commonFields = huxerui::TextField(fields.desc.Get())
                           .Label("描述（可选）")
                           .Variant(huxerui::TextFieldVariant::Outlined)
                           .OnChanged([desc = fields.desc](
                                          const huxerui::TextEditingValue& value) {
                               desc = value;
                           });
        hasCommonFields = true;
    }

    std::vector<huxerui::View> formFields;
    formFields.push_back(std::move(directTypeSelector));
    formFields.push_back(std::move(typeFields));
    formFields.push_back(huxerui::TextField(fields.name.Get())
                             .Label("名称（可选）")
                             .Variant(huxerui::TextFieldVariant::Outlined)
                             .OnChanged([name = fields.name](
                                            const huxerui::TextEditingValue& value) {
                                 name = value;
                             }));
    if (hasCommonFields) formFields.push_back(std::move(commonFields));
    formFields.push_back(
        fields.importing.Get()
            ? huxerui::View{huxerui::ProgressCircle().With(
                  huxerui::Frame{.width = 28.0F, .height = 28.0F})}
            : huxerui::View{huxerui::Button("导入配置")
                                .With(huxerui::Grow(1.0F))
                                .OnClick(import_profile)});
    formFields.push_back(CompactFloatingNavigationFooter());
    const std::string title =
        method == ProfileAddMethod::Qr ? "扫码导入"
        : isFile                         ? "从文件导入"
        : method == ProfileAddMethod::Url ? "从 URL 导入"
                                          : "直接配置";
    huxerui::View content = huxerui::ScrollView(
        huxerui::Column(std::move(formFields))
            .With(huxerui::Spacing(12.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
                                .With(huxerui::Grow(1.0F), huxerui::ScrollBar());
    return ProfileFlowPage(huxerui::Text(title, huxerui::TextRole::Title),
                           huxerui::View{}, std::move(content),
                           std::move(on_back));
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

    return ProfileFlowPage(
        huxerui::Text("编辑订阅", huxerui::TextRole::Title),
        huxerui::IconButton(app::images::save, "保存")
            .With(huxerui::Tooltip("保存订阅"))
            .OnClick(save),
        huxerui::ScrollView(
            huxerui::Column {
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
                CompactFloatingNavigationFooter(),
            }
                .With(huxerui::Spacing(12.0F),
                      huxerui::CrossAlign(
                          huxerui::CrossAxisAlignment::Stretch)))
                .With(huxerui::Grow(1.0F), huxerui::ScrollBar()),
        on_back);
}

[[huxerui::composable]] huxerui::View ProfileEditRoutePage(
    std::int64_t id, ProfileEditFields fields, huxerui::TaskScope tasks,
    huxerui::ToastHandle toast, std::function<void()> on_back) {
    return ProfileEditPage(
        id, fields.name, fields.url, fields.type, fields.desc, fields.timeout,
        fields.interval, fields.auto_update, fields.system_proxy,
        fields.core_proxy, fields.invalid_cert, fields.pptp_server,
        fields.pptp_username, fields.pptp_password, fields.pptp_timeout,
        fields.pptp_routes, fields.pptp_mppe, fields.openvpn_config,
        fields.openvpn_routes, tasks, toast, std::move(on_back));
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
    return ProfileFlowPage(
        huxerui::Text("编辑订阅文件", huxerui::TextRole::Title),
        huxerui::IconButton(app::images::save, "保存文件")
            .With(huxerui::Tooltip("保存订阅文件"))
            .OnClick(save),
        huxerui::Column {
            huxerui::Text("直接编辑订阅 YAML；保存后启用中的订阅会重启生效。"),
            editor,
            CompactFloatingNavigationFooter(),
        }.With(huxerui::Spacing(12.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
               huxerui::Grow(1.0F)),
        std::move(on_back));
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
    return ProfileFlowPage(
        huxerui::Text("编辑订阅规则", huxerui::TextRole::Title),
        huxerui::IconButton(app::images::save, "保存规则")
            .With(huxerui::Tooltip("保存订阅规则"))
            .OnClick(save),
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
               huxerui::Grow(1.0F)),
        std::move(on_back));
}

} // namespace clashflux::ui
