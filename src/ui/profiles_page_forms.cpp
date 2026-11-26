// profiles_page_forms.cpp — 订阅创建/编辑表单与移动端二级页面。
#include <huxerui/huxerui.h>
#include "share_qr.h"
#include "text_document.h"
#include <huxerui/camera.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <tuple>
#include <vector>

#include "profile_import_task.h"
#include "profile_update_interval.h"
#include "qrs_codec.h"
#include "app_resources.h"
#include "ui.h"
#include "page_layout.h"
#include "empty_state.h"
#include "file_drop_zone.h"
#include "profile_create_type_selector.h"
#include "settings_group_layout.h"
#include "profile_file_picker.h"
#include "task_bridge.h"
#include "ssh_profile_config.h"
#if defined(__ANDROID__)
#include <huxerui/media_library.h>
#include "qr_photo_decoder.h"
#include "qr_scanner_control.h"
#endif

import clashflux.core;
import clashflux.model;
import clashflux.pptp;
import clashflux.store.profiles;
import clashflux.store.vpn;
import clashflux.utils;

#include "profiles_page_shared.h"
#include "profiles_model.h"

namespace clashflux::ui {

namespace {

struct ProfileFormDialog {
    bool enabled = false;
    std::shared_ptr<std::function<void()>> close;
    static ProfileFormDialog Default() { return {}; }
    bool operator==(const ProfileFormDialog&) const = default;
};

[[huxerui::composable]] huxerui::View ProfileFormScroll(huxerui::View fields, bool dialog = false) {
    const bool inDialog = dialog || huxerui::UseEnvironment<ProfileFormDialog>().enabled;
    if (inDialog) return PageScrollView(fields).With(huxerui::Grow(1.0F));
    return ScrollablePageContent(fields);
}

auto ProfileCreateSnapshot(const ProfileCreateFields& fields) {
    const auto& picked = fields.picked_file.Get();
    const auto localFile = picked ? picked->AsFile() : std::nullopt;
    const auto& ssh = fields.ssh.Get();
    return std::tuple{fields.name.Get().text, fields.url.Get().text, fields.qr_content.Get().text,
        fields.desc.Get().text, fields.timeout.Get().text, fields.interval.Get().text,
        fields.system_proxy.Get(), fields.core_proxy.Get(),
        fields.pptp_server.Get().text, fields.pptp_username.Get().text, fields.pptp_password.Get().text,
        fields.pptp_timeout.Get().text, fields.pptp_routes.Get().text, fields.pptp_mppe.Get(),
        localFile ? localFile->Path() : std::string{}, ssh.server.text, ssh.port.text, ssh.username.text,
        ssh.password.text, ssh.hostKey.text, ssh.privateKey, ssh.passphrase.text, ssh.auth};
}

bool IsSshPrivateKey(std::string_view key) {
    return key.find("-----BEGIN ") != std::string_view::npos &&
        key.find("PRIVATE KEY-----") != std::string_view::npos;
}

std::optional<SshProfileConfig> ReadSshProfile(const SshProfileDraft& draft, FluxToastHandle toast) {
    SshProfileConfig config;
    config.server = trim(draft.server.text);
    if (config.server.starts_with('[') && config.server.ends_with(']'))
        config.server = config.server.substr(1, config.server.size() - 2);
    config.username = draft.username.text;
    if (config.server.empty() || config.server.find_first_of(" /?#@[]\r\n\t") != std::string::npos ||
        std::ranges::count(config.server, ':') == 1 || trim(config.username).empty()) {
        toast.Show(Localized("请填写有效的 SSH 服务器地址和用户名"));
        return std::nullopt;
    }
    const auto port = trim(draft.port.text);
    const auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), config.port);
    if (error != std::errc{} || end != port.data() + port.size() || config.port < 1 || config.port > 65535) {
        toast.Show(Localized("端口无效"));
        return std::nullopt;
    }
    config.hostKey = trim(draft.hostKey.text);
    const auto separator = config.hostKey.find_first_of(" \t");
    const auto keyType = std::string_view(config.hostKey).substr(0, separator);
    if (separator == std::string::npos || config.hostKey.find_first_of("\r\n") != std::string::npos ||
        !(keyType.starts_with("ssh-") || keyType.starts_with("ecdsa-") || keyType.starts_with("sk-"))) {
        toast.Show(Localized("请填写 SSH 服务器的主机公钥"));
        return std::nullopt;
    }
    if (draft.auth == 0) config.password = draft.password.text;
    else {
        config.privateKey = draft.privateKey;
        config.passphrase = draft.passphrase.text;
    }
    if (config.password.empty() && !IsSshPrivateKey(config.privateKey)) {
        toast.Show(Localized("请输入 SSH 密码或有效的私钥"));
        return std::nullopt;
    }
    return config;
}

std::function<void()> UseProfileCreateClose(ProfileCreateFields fields, std::function<void()> close,
    std::optional<huxerui::State<bool>> importing = std::nullopt) {
    const auto initial = huxerui::UseState(ProfileCreateSnapshot(fields));
    // Submitted imports run in the application scope; only unsaved draft
    // changes need confirmation. A slow request cannot veto back/close.
    return UseEditorClose([=] {
        return (!importing || !importing->Get()) && ProfileCreateSnapshot(fields) != initial.Get();
    },
        [] { return false; }, close);
}

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
    if (profile_link::IsHttpUrl(value)) return true;
    std::string error;
    return profile_link::Parse(value, error).has_value();
}

huxerui::Task<std::pair<std::int64_t, std::string>> ImportProfileContent(
    std::string name, std::string content, model::Profile options) {
    co_return co_await RunOnTaskThread(
        [name = std::move(name), content = std::move(content),
         options = std::move(options)] {
            auto& profiles = store::profilesStore();
            const std::int64_t id =
                profiles.importContent(name, content, options);
            return std::pair{id, profiles.lastError()};
        });
}

huxerui::Task<ProfileImportResult> ImportSshProfile(
    std::string name, SshProfileConfig config, model::Profile options) {
    if (name.empty()) name = "SSH · " + config.server;
    auto yaml = MakeSshProfileYaml(config);
    if (yaml.empty()) co_return ProfileImportResult{};
    co_return co_await ImportProfileContent(std::move(name), std::move(yaml), std::move(options));
}

} // namespace


// 开关行：左标签（danger = error 色警示）+ 说明，右 Switch。Switch 卸载风险
// 不存在（原地改样式），OnChanged 内直接写 State 安全。
[[huxerui::composable]] huxerui::View ToggleRow(
    huxerui::StringVariant label, huxerui::StringVariant hint, bool danger,
    huxerui::State<bool> checked) {
    return SettingSwitchRow(
        label, hint,
        huxerui::Switch(checked.Get()).OnChanged(
            [checked](bool on) { checked = on; }),
        danger);
}

// Android 不提供桌面系统代理与内核代理选项。
#if defined(__ANDROID__)

huxerui::View ProfilePlatformOptions(huxerui::State<bool>, huxerui::State<bool>) {
    return huxerui::Row{};
}

#else

[[huxerui::composable]] huxerui::View DesktopProfileOptions(
    huxerui::State<bool> system_proxy, huxerui::State<bool> core_proxy) {
    return huxerui::Column {
        ToggleRow(Localized("使用系统代理更新"),
                  Localized("经环境变量代理拉取订阅"), false,
                  system_proxy),
        ToggleRow(Localized("使用内核代理更新"),
                  Localized("经本应用内核混合端口拉取（内核需运行）"),
                  false, core_proxy),
    }.With(huxerui::Spacing(16.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

huxerui::View ProfilePlatformOptions(
    huxerui::State<bool> system_proxy, huxerui::State<bool> core_proxy) {
    return DesktopProfileOptions(system_proxy, core_proxy);
}

#endif

huxerui::View ProfileDescriptionField(huxerui::State<huxerui::TextEditingValue> desc) {
    return huxerui::TextField(desc.Get())
        .Label(Localized("描述（可选）"))
        .Variant(huxerui::TextFieldVariant::Outlined)
        .OnChanged([desc](const huxerui::TextEditingValue& value) { desc = value; });
}

[[huxerui::composable]] huxerui::View ProfileDownloadOptions(
    huxerui::State<huxerui::TextEditingValue> timeout,
    huxerui::State<huxerui::TextEditingValue> interval,
    huxerui::State<bool> sysProxy,
    huxerui::State<bool> coreProxy) {
    return huxerui::Column {
        huxerui::TextField(timeout.Get())
            .Label(Localized("HTTP 超时（秒）"))
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([timeout](const huxerui::TextEditingValue& v) {
                timeout = v;
            }),
        ProfileUpdateIntervalField(interval),
        ProfilePlatformOptions(sysProxy, coreProxy),
    }
        .With(huxerui::Spacing(16.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View ProfileOptionsForm(
    huxerui::State<huxerui::TextEditingValue> desc,
    huxerui::State<huxerui::TextEditingValue> timeout,
    huxerui::State<huxerui::TextEditingValue> interval,
    huxerui::State<bool> sysProxy, huxerui::State<bool> coreProxy) {
    return huxerui::Column {
      ProfileDescriptionField(desc),
      ProfileDownloadOptions(timeout, interval, sysProxy, coreProxy),
    }.With(huxerui::Spacing(16.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
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
            .Label(Localized("PPTP 服务器"))
            .Placeholder("vpn.example.com")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([server](const huxerui::TextEditingValue& value) {
                server = value;
            }),
        huxerui::TextField(username.Get())
            .Label(Localized("用户名"))
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([username](const huxerui::TextEditingValue& value) {
                username = value;
            }),
        PasswordField(password),
        huxerui::TextField(timeout.Get())
            .Label(Localized("连接超时（秒）"))
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([timeout](const huxerui::TextEditingValue& value) {
                timeout = value;
            }),
        SettingSwitchRow(
            Localized("要求 MPPE-128"), Localized("勾选后强制 MPPE-128；不勾选时按服务端要求协商"),
            huxerui::Switch(requireMppe.Get())
                .OnChanged([requireMppe](bool checked) { requireMppe = checked; })),
        huxerui::TextField(routes.Get())
            .Label(Localized("内网 CIDR（逗号或换行分隔，可为空）"))
            .Placeholder("10.20.0.0/16, 10.30.0.0/16")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([routes](const huxerui::TextEditingValue& value) {
                routes = value;
            }),
    }
        .With(huxerui::Spacing(16.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

huxerui::TextField SshField(huxerui::State<SshProfileDraft> draft,
                            huxerui::TextEditingValue SshProfileDraft::* value, huxerui::StringVariant label) {
    return huxerui::TextField(draft.Get().*value).Label(label).Variant(huxerui::TextFieldVariant::Outlined)
        .OnChanged([draft, value](const huxerui::TextEditingValue& nextValue) {
            auto next = draft.Get();
            next.*value = nextValue;
            draft = std::move(next);
        });
}

[[huxerui::composable]] huxerui::View SshOptionsForm(huxerui::State<SshProfileDraft> draft) {
    const auto auth = draft.Get().auth;
    const auto clipboard = huxerui::UseApplication().Clipboard();
    const auto tasks = huxerui::UseTaskScope();
    const auto toast = UseFluxToast();
    const auto pasteKey = [clipboard, tasks, toast, draft] {
        tasks.Launch([clipboard, toast, draft]() -> huxerui::Task<void> {
            if (clipboard) {
                auto key = co_await clipboard->ReadTextAsync();
                if (key.Succeeded() && IsSshPrivateKey(key.Value())) {
                    auto next = draft.Get();
                    next.privateKey = std::move(key).Value();
                    draft = std::move(next);
                    co_return;
                }
            }
            toast.Show(Localized("剪贴板中没有有效的 SSH 私钥"));
        });
    };
    return huxerui::Column {
      SshField(draft, &SshProfileDraft::server, Localized("SSH 服务器")).Placeholder("192.168.1.10"),
      SshField(draft, &SshProfileDraft::port, Localized("SSH 端口"))
          .InputConfiguration({.type = huxerui::TextInputType::Number}),
      SshField(draft, &SshProfileDraft::username, Localized("用户名")),
      SshField(draft, &SshProfileDraft::hostKey, Localized("SSH 主机公钥"))
          .Placeholder("ssh-ed25519 AAAA…"),
      FluxSegmentedButton({Localized("密码"), Localized("SSH 私钥")}, auth,
          [draft](std::size_t choice) {
              auto next = draft.Get();
              next.auth = choice;
              draft = std::move(next);
          }).Key("profile-ssh-auth"),
      auth == 0
          ? huxerui::View{SshField(draft, &SshProfileDraft::password, Localized("密码")).Secure()}
          : huxerui::View{huxerui::Column {
              huxerui::Button(Localized(draft.Get().privateKey.empty()
                  ? "粘贴 SSH 私钥" : "替换已粘贴的 SSH 私钥")).OnClick(pasteKey),
              SshField(draft, &SshProfileDraft::passphrase, Localized("私钥口令（可选）")).Secure(),
            }.With(huxerui::Spacing(16.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))},
    }.With(huxerui::Spacing(16.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
        .Key("profile-ssh-fields");
}

// Android 端的编辑入口使用完整页面，避免弹窗被软键盘和窄视口挤压；桌面
// 仍由调用方使用 Dialog。表单 State 由 ProfilesPage 持有，页面与弹窗共用一份
// 数据和保存逻辑。

huxerui::View ProfileImportAction(bool importing, huxerui::StringVariant label,
                                  std::function<void()> onImport) {
    if (importing) {
        return huxerui::Row{
            huxerui::ProgressCircle().With(huxerui::Frame{.width = 20.0F, .height = 20.0F}),
        }.With(huxerui::Frame{.width = 40.0F, .height = 40.0F},
               huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
            .Key("profile-create-progress");
    }
    return huxerui::IconButton(app::images::add, label)
        .With(huxerui::Tooltip(label), huxerui::Frame{.width = 40.0F, .height = 40.0F})
        .OnClick(std::move(onImport))
        .Key("profile-create-submit");
}

// 可拖入也可点击的配置文件选择区：统一使用 src/ui/file_drop_zone.h 的共享控件
// （圆角/描边随控件族，悬停高亮，整批扩展名校验）。

[[huxerui::composable]] huxerui::View ProfileCreatePage(
    ProfileCreateFields fields, huxerui::TaskScope tasks,
    FluxToastHandle toast, std::shared_ptr<huxerui::FilePicker> picker,
    std::shared_ptr<AppHttpClient> http, bool pptp_supported,
    std::function<void()> on_back, bool embedded) {
    const auto importing = huxerui::UseState(false);
    const auto origin = UseProfileImportOrigin();
    const auto applicationTasks = huxerui::UseApplicationTaskScope();
    const auto go_back = [origin, on_back] {
        origin->Advance();
        if (on_back) on_back();
    };
    const auto cancel = UseProfileCreateClose(fields, go_back, importing);
    const auto profilesModel = huxerui::UseService<ProfilesModel>();
    const auto configFileFilter = ProfileConfigFileFilter();
    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    const auto typeIndex = fields.type_index.Get();
    const bool remote = typeIndex == 0;
    const bool local = typeIndex == 1;
    const bool pptp = pptp_supported && typeIndex == 2;
    const bool ssh = typeIndex == (pptp_supported ? 3 : 2);
    std::vector<huxerui::StringVariant> types{
        Localized("远程订阅"), Localized("本地文件")};
    if (pptp_supported) types.push_back(Localized("PPTP 内网"));
    types.push_back(Localized("SSH 节点"));

    huxerui::View typeFields;
    if (remote) {
        typeFields = huxerui::TextField(fields.url.Get())
                         .Label(Localized("订阅链接"))
                         .Placeholder("https://...")
                         .Variant(huxerui::TextFieldVariant::Outlined)
                         .OnChanged([url = fields.url](const huxerui::TextEditingValue& value) {
                             url = value;
                         });
    } else if (local) {
        typeFields = FileDropZone(app::images::file_import,
                                  Localized("拖入或点击选择配置文件"),
                                  fields.picked_file, tasks, toast, picker,
                                  configFileFilter, "profile-file-drop-zone");
    } else if (ssh) {
        typeFields = SshOptionsForm(fields.ssh);
    } else {
        typeFields = PptpOptionsForm(
            fields.pptp_server, fields.pptp_username, fields.pptp_password,
            fields.pptp_timeout, fields.pptp_routes, fields.pptp_mppe);
    }

    const auto importProfile = [=] {
        if (importing) return;
        const std::string url = fields.url.Get().text;
        if (remote && url.empty()) {
            toast.Show(Localized("订阅链接不能为空"));
            return;
        }
        const auto picked = local ? fields.picked_file.Get() : std::nullopt;
        const auto localFile = picked ? picked->AsFile() : std::nullopt;
        if (local && !localFile) {
            toast.Show(Localized("请先选择订阅文件"));
            return;
        }
        std::string configError;
        std::optional<std::string> nativeConfig;
        std::optional<SshProfileConfig> sshConfig;
        if (ssh) {
            sshConfig = ReadSshProfile(fields.ssh.Get(), toast);
            if (!sshConfig) return;
        }
        if (pptp) {
            nativeConfig = makePptpConfig(
                fields.pptp_server.Get(), fields.pptp_username.Get(),
                fields.pptp_password.Get(), fields.pptp_timeout.Get(),
                fields.pptp_mppe.Get(), configError);
            if (!nativeConfig) {
                toast.Show(configError);
                return;
            }
        }
        model::Profile options;
        options.description = fields.desc.Get().text;
        if (remote || local) {
            options.timeoutSecs = parseNumber(fields.timeout.Get(), 60);
            const auto minutes = ParseProfileUpdateInterval(fields.interval.Get().text);
            if (!minutes) { toast.Show(ProfileUpdateIntervalError()); return; }
            options.intervalMins = *minutes;
            options.autoUpdate = *minutes > 0;
            options.useSystemProxy = fields.system_proxy.Get();
            options.useCoreProxy = fields.core_proxy.Get();
        } else if (pptp) {
            options.type = "pptp";
            options.nativeConfig = *nativeConfig;
            options.nativeRoutes = joinRoutes(parseRouteField(
                fields.pptp_routes.Get().text));
        }
        ProfileImportRequest request{
            .remote = remote, .local = local, .pptp = pptp,
            .name = fields.name.Get().text, .url = url,
            .picked_path = localFile ? localFile->Path() : std::string{}, .options = std::move(options)};
        const auto ticket = origin->Advance();
        LaunchProfileImport(importing, [http, picked, request = std::move(request),
            sshConfig = std::move(sshConfig)]() mutable -> huxerui::Task<ProfileImportResult> {
            if (sshConfig)
                co_return co_await ImportSshProfile(std::move(request.name), std::move(*sshConfig), std::move(request.options));
            static_cast<void>(picked); // Retain access until the path-based import completes.
            co_return co_await ImportProfileForPlatform(http, std::move(request));
        }, [=](ProfileImportResult result) {
            profilesModel->PublishCached();
            const auto& [id, error] = result;
            if (id == 0) {
                toast.Show(error.empty() ? Localized("导入失败")
                                         : huxerui::StringVariant(error));
                return;
            }
            if (origin->CanNavigate(ticket)) go_back();
            toast.Show(pptp ? Localized("配置已导入") : Localized("订阅已保存，正在准备资源"));
            StartProfilePreparation(applicationTasks, profilesModel, id, toast);
        });
    };

    const auto typeSelector = ProfileCreateTypeSelector(std::move(types), fields.type_index);
    huxerui::View formFields = huxerui::Column {
      huxerui::TextField(fields.name.Get())
          .Label(Localized("名称（可选）"))
          .Variant(huxerui::TextFieldVariant::Outlined)
          .OnChanged([name = fields.name](const huxerui::TextEditingValue& value) { name = value; })
          .Key("profile-create-name"),
      ProfileDescriptionField(fields.desc).Key("profile-create-description"),
      typeFields,
      (pptp || ssh) ? huxerui::View{} : ProfileDownloadOptions(fields.timeout, fields.interval,
          fields.system_proxy, fields.core_proxy),
    }.With(huxerui::Spacing(16.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    if (embedded) {
        const auto body = huxerui::Column {
          typeSelector,
          PageScrollView(formFields).With(huxerui::Grow(1.0F)),
        }.With(huxerui::Spacing(16.0F), huxerui::Grow(1.0F), huxerui::Enabled(!importing.Get()),
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
        const auto create = DialogAction(Localized("创建"), importProfile, true, !importing.Get())
            .With(huxerui::Semantics{.busy = importing.Get()}).Key("profile-create-submit");
        const auto progress = importing.Get()
            ? huxerui::View{huxerui::ProgressCircle().With(huxerui::Frame{.width = 20.0F, .height = 20.0F})}
            : huxerui::View{};
        return DialogScaffold(DialogHeader(Localized("新建订阅")).Key("profile-create-title"), body,
            DialogActions({progress, DialogAction(Localized("取消"), cancel, false), create}), DialogSize::Form, true);
    }
    const auto scrollContent = huxerui::Column {typeSelector, formFields}
        .With(huxerui::Spacing(16.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    huxerui::View content = ProfileFormScroll(scrollContent)
        .With(huxerui::Grow(1.0F));
    if (compact) content = FormPageContent(content);
    huxerui::View action = FormHeaderAction(
        app::images::save, Localized("创建"), importProfile,
        !importing, importing);
    return SecondaryPageScaffold(
        huxerui::Text(Localized("新建订阅"), huxerui::TextRole::Title)
            .Key("profile-create-title"),
        compact ? action : ProfileImportAction(
            importing, Localized("导入订阅"), importProfile),
        std::move(content),
        ssh ? cancel : std::move(go_back), false, false, true);
}

[[huxerui::composable]] huxerui::View ProfileFlowPage(
    huxerui::View title, huxerui::View actions, huxerui::View content,
    std::function<void()> on_back, bool centerTitle = false,
    huxerui::View compactAction = {}, huxerui::View dialogAction = {}) {
    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    const bool inDialog = huxerui::UseEnvironment<ProfileFormDialog>().enabled;
    if (inDialog) {
        const bool canGoBack = huxerui::UseNavigation().CanPop();
        const auto rootClose = huxerui::UseEnvironment<ProfileFormDialog>().close;
        return huxerui::Column {
          DialogHeaderContent(title, {},
              canGoBack ? huxerui::View(huxerui::IconButton(app::images::arrow_back,
                  Localized("返回上一页")).OnClick(on_back)) : huxerui::View{}),
          huxerui::View{content}.With(huxerui::Grow(1.0F)),
          DialogActions({DialogAction(Localized("取消"), rootClose ? *rootClose : on_back, false), dialogAction}),
        }.With(huxerui::Spacing(16.0F), huxerui::Grow(1.0F),
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }
    huxerui::View pageActions = actions;
    huxerui::View pageContent = content;
    if (compact && compactAction) {
        pageActions = compactAction;
        pageContent = FormPageContent(content);
    }
    return SecondaryPageScaffold(title, std::move(pageActions),
                                 std::move(pageContent), std::move(on_back),
                                 false, false, centerTitle);
}

[[huxerui::composable]] huxerui::View ProfileAddMethodRow(
    huxerui::ImageResource icon, huxerui::StringVariant name,
    huxerui::StringVariant hint,
    std::string key, bool divider, std::function<void()> on_click) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return SettingsActionRow(huxerui::Row {
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
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)), on_click,
        {.role = huxerui::SemanticRole::Button, .label = name}, SettingsRowInsets{8.0F, 14.0F, 64.0F})
        .Key(key);
}

#if defined(__ANDROID__)

[[huxerui::composable]] huxerui::View ProfileQrScannerPage(
    std::function<void()> on_back,
    std::function<void(std::string)> on_result) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const huxerui::ApplicationHandle application = huxerui::UseApplication();
    const huxerui::TaskScope tasks = huxerui::UseTaskScope();
    const auto control = huxerui::UseState(std::make_shared<QrScannerControl>()).Get();
    const auto mediaLibrary = huxerui::media::UseMediaLibrary();
    const auto toast = UseFluxToast();
    auto decoder = huxerui::UseState(std::shared_ptr<QrPhotoDecoder>{});
    const auto qrsDecoder = huxerui::UseState(std::make_shared<qrs::Decoder>()).Get();
    auto detected = huxerui::UseState(std::string{});
    auto message = huxerui::UseState(Localized("正在准备相机…"));
    const huxerui::camera::CameraSession session = huxerui::camera::UseCamera({
        .facing = huxerui::camera::Facing::Back, .active = control->cameraActive.Get(),
    });
    const auto getDecoder = [decoder] {
        if (!decoder.Get()) decoder = OpenQrPhotoDecoder();
        return decoder.Get();
    };
    // Both sources feed the same QRS accumulator; choosing one frame must not
    // discard frames already obtained from the camera or another image.
    const auto consume = [qrsDecoder, message](std::string content) -> std::string {
        if (content.empty()) return {};
        std::string decoded, error;
        const auto result = qrsDecoder->Add(content, decoded, error);
        if (result == qrs::DecodeResult::Complete) return decoded;
        if (result == qrs::DecodeResult::NotQrs) return content;
        if (result == qrs::DecodeResult::NeedMore) {
            message = LocalizedFormat("已扫描 {0} / {1} 个 QRS 帧",
                qrsDecoder->DecodedBlocks(), qrsDecoder->TotalBlocks());
        } else {
            message = error;
            qrsDecoder->Reset();
        }
        return {};
    };
    const auto start_scanning = [=] {
        if (!control->StartCamera()) return;
        detected = std::string{};
        message = Localized("正在请求相机权限…");
        control->cameraTask = tasks.Launch([=]() -> huxerui::Task<void> {
            try {
                const auto decoderInstance = getDecoder();
                const auto permission = co_await application.RequestPermissionAsync(huxerui::Permission::Camera);
                if (permission != huxerui::PermissionStatus::Granted) {
                    message = Localized(permission == huxerui::PermissionStatus::Unavailable
                        ? "当前设备无法使用相机" : "请允许相机权限后重试");
                    control->scanning = false;
                    co_return;
                }
                if (session.Status().state == huxerui::camera::SessionState::Failed) session.Retry();
                control->cameraActive = true;
                message = qrsDecoder->DecodedBlocks() > 0
                    ? LocalizedFormat("已扫描 {0} / {1} 个 QRS 帧", qrsDecoder->DecodedBlocks(), qrsDecoder->TotalBlocks())
                    : Localized("正在启动相机…");
                while (control->scanning.Get()) {
                    const auto status = session.Status();
                    if (status.state == huxerui::camera::SessionState::Failed) {
                        message = status.error ? huxerui::StringVariant(status.error->message)
                            : Localized("相机启动失败，请重试");
                        break;
                    }
                    if (status.state != huxerui::camera::SessionState::Running) {
                        co_await huxerui::Delay(std::chrono::milliseconds{120});
                        continue;
                    }
                    if (qrsDecoder->DecodedBlocks() == 0)
                        message = Localized("将二维码放入取景框，支持订阅链接和配置文本。");
                    const auto photo = co_await session.CapturePhotoAsync(
                        {.jpeg_quality = 50, .mirror = huxerui::camera::MirrorMode::Off});
                    if (!photo.Succeeded()) {
                        const auto code = photo.Error().code;
                        if (code != huxerui::camera::CameraErrorCode::NotReady &&
                            code != huxerui::camera::CameraErrorCode::Interrupted &&
                            code != huxerui::camera::CameraErrorCode::OperationInProgress) {
                            message = photo.Error().message;
                            break;
                        }
                    } else {
                        const auto encoded = photo.Value().EncodedBytes();
                        const auto content = consume(co_await decoderInstance->DecodeAsync(
                            huxerui::Bytes(encoded.begin(), encoded.end())));
                        if (!content.empty()) {
                            detected = content;
                            control->scanning = false;
                            control->cameraActive = false;
                            on_result(content);
                            co_return;
                        }
                    }
                    co_await huxerui::Delay(std::chrono::milliseconds{180});
                }
            } catch (const std::exception& error) { message = std::string(error.what()); }
            catch (...) { message = Localized("相机启动失败，请重试"); }
            control->scanning = false;
            control->cameraActive = false;
        });
    };
    const auto select_image = [=] {
        const bool resumeCamera = control->scanning.Get();
        const auto previousMessage = message.Get();
        if (!control->BeginSelection()) return;
        message = Localized("正在选择图片…");
        tasks.Launch([=]() -> huxerui::Task<void> {
            struct FinishSelection {
                std::shared_ptr<QrScannerControl> control;
                ~FinishSelection() { control->EndSelection(); }
            } finish{control};
            try {
                auto result = co_await mediaLibrary.PickAsync({huxerui::media::MediaFilter::Images, 1});
                if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
                auto images = std::move(result).Value();
                if (images.empty()) {
                    message = previousMessage;
                } else {
                    message = Localized("正在识别图片…");
                    const auto decoderInstance = getDecoder();
                    const auto text = co_await decoderInstance->DecodeAsync(std::move(images.front()));
                    if (text.empty()) {
                        message = Localized("图片中未识别到二维码，请重新选择");
                        toast.Show(Localized("图片中未识别到二维码，请重新选择"));
                    } else {
                        const auto content = consume(text);
                        if (!content.empty()) {
                            detected = content;
                            control->EndSelection();
                            on_result(content);
                            co_return;
                        }
                    }
                }
            } catch (const std::exception& error) {
                message = std::string(error.what());
                toast.Show(std::string(error.what()));
            } catch (...) {
                message = Localized("无法选择或识别图片，请重试");
                toast.Show(Localized("无法选择或识别图片，请重试"));
            }
            control->EndSelection();
            if (resumeCamera) start_scanning();
        });
    };
    huxerui::Lifecycle([control, start_scanning] {
        start_scanning();
        return [control] { control->StopCamera(); };
    }, 0);

    const huxerui::camera::CameraStatus status = session.Status();
    huxerui::View preview;
    if (status.preview) {
        preview = huxerui::camera::CameraPreview(
            session, {.fit = huxerui::ImageFit::Cover,
                      .mirror = huxerui::camera::MirrorMode::Off});
    } else {
        preview = huxerui::Column {
            control->scanning.Get() ? huxerui::View{huxerui::ProgressCircle()}
                           : huxerui::View{huxerui::Image(app::images::qr_scan)
                                               .Tint(theme.colors.on_surface_variant)
                                               .With(huxerui::Frame{.width = 36.0F,
                                                                    .height = 36.0F})},
            huxerui::Text(message.Get())
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kBody),
                    theme.colors.on_surface_variant}),
        }.With(huxerui::Spacing(16.0F),
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
        !control->scanning.Get() && !detected.Get().empty()
            ? huxerui::View{huxerui::Text(Localized("正在导入配置…"))
                                .Style(huxerui::TextStyle{
                                    huxerui::Font::System(font_size::kCaption),
                                    theme.colors.on_surface_variant})}
            : huxerui::View{huxerui::Row{}},
        !control->scanning.Get() && !control->selecting.Get()
            ? huxerui::View{huxerui::Button(Localized("重试"))
                                .OnClick(start_scanning)}
            : huxerui::View{huxerui::Row{}},
    }.With(huxerui::Spacing(16.0F),
           huxerui::Padding(huxerui::EdgeInsets::Symmetric(16.0F, 12.0F)),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    return ProfileFlowPage(
        huxerui::Text(Localized("扫描二维码"), huxerui::TextRole::Title),
        huxerui::IconButton(app::images::photo_library, Localized("从相册选择"))
            .With(huxerui::Tooltip(Localized("从相册选择")), huxerui::Enabled(!control->selecting.Get()))
            .OnClick(select_image).Key("qr-select-image"),
        PageScrollView(std::move(body))
            .With(huxerui::Grow(1.0F)),
        std::move(on_back))
        .With(ProfileSecondaryTransition(theme.motion));
}

void PushProfileQrScanner(
    huxerui::NavigationController navigation,
    FluxToastHandle toast,
    std::function<void(std::string)> on_result) {
    static_cast<void>(toast);
    navigation.Push([=] {
        return ProfileQrScannerPage(
            [navigation] {
                static_cast<void>(navigation.Pop());
            },
            [navigation, on_result](std::string content) mutable {
                static_cast<void>(navigation.Pop());
                if (on_result) on_result(std::move(content));
            });
    });
}

#else

void PushProfileQrScanner(
    huxerui::NavigationController,
    FluxToastHandle toast,
    std::function<void(std::string)>) {
    toast.Show(Localized("当前平台暂不支持二维码扫描"));
}

#endif

[[huxerui::composable]] huxerui::View ProfileAddMethodPage(
    ProfileCreateFields fields, huxerui::TaskScope tasks,
    FluxToastHandle toast, std::shared_ptr<huxerui::FilePicker> picker,
    std::shared_ptr<AppHttpClient> http,
    bool pptp_supported,
    huxerui::NavigationController navigation,
    std::function<void()> close) {
    const auto origin = UseProfileImportOrigin();
    const auto applicationTasks = huxerui::UseApplicationTaskScope();
    const auto qrImporting = huxerui::UseState(false);
    const auto fileImporting = huxerui::UseState(false);
    const auto pickerTasks = huxerui::UseTaskScope();
    const auto profilesModel = huxerui::UseService<ProfilesModel>();
    const auto configFileFilter = ProfileConfigFileFilter();
    const std::string qrConfigName = huxerui::UseString(Localized("二维码配置"));
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const auto on_back = [navigation, close, origin] {
        origin->Advance();
        if (navigation.CanPop()) static_cast<void>(navigation.Pop());
        else if (close) close();
    };
    std::function<void()> rootBack = on_back;
    if (!huxerui::UseEnvironment<ProfileFormDialog>().enabled)
        rootBack = UseProfileCreateClose(fields, on_back);
    const auto finishImport = [toast, on_back, profilesModel, origin, applicationTasks](
                                  std::uint64_t ticket, ProfileImportResult result) {
        profilesModel->PublishCached();
        if (result.first == 0) {
            toast.Show(result.second.empty() ? Localized("导入失败")
                                             : huxerui::StringVariant(result.second));
            return;
        }
        toast.Show(Localized("订阅已保存，正在准备资源"));
        StartProfilePreparation(applicationTasks, profilesModel, result.first, toast);
        if (origin->CanNavigate(ticket)) on_back();
    };
    const auto importRemote = [qrImporting, origin, http, finishImport](
                                  std::string url) {
        if (qrImporting) return;
        model::Profile options;
        options.autoUpdate = true;
        options.intervalMins = 1440;
        options.timeoutSecs = 60;
        const auto ticket = origin->Advance();
        LaunchProfileImport(qrImporting,
            [http, url = std::move(url), options]() mutable -> huxerui::Task<ProfileImportResult> {
            ProfileImportRequest request;
            request.remote = true;
            request.url = std::move(url);
            request.options = options;
            co_return co_await ImportProfileForPlatform(http, std::move(request));
        }, [finishImport, ticket](auto result) { finishImport(ticket, std::move(result)); });
    };
    const auto importInline = [qrImporting, origin, finishImport, qrConfigName](
                                  std::string content) {
        if (qrImporting) return;
        model::Profile options;
        const auto ticket = origin->Advance();
        LaunchProfileImport(qrImporting,
            [content = std::move(content), options, qrConfigName]() mutable
                -> huxerui::Task<ProfileImportResult> {
                co_return co_await ImportProfileContent(qrConfigName, std::move(content), options);
            }, [finishImport, ticket](auto result) { finishImport(ticket, std::move(result)); });
    };
    const auto beginQrScan = [qrImporting, origin, navigation, toast,
                              importRemote, importInline] {
        if (qrImporting) return;
        origin->Advance();
        PushProfileQrScanner(
            navigation, toast,
            [importRemote, importInline](std::string content) mutable {
                if (IsHttpProfileUrl(content)) {
                    importRemote(std::move(content));
                } else {
                    importInline(std::move(content));
                }
            });
    };
    const auto openMethod = [fields, tasks, toast, picker, http, origin,
                             pptp_supported, navigation,
                             on_back, close, popDuration = theme.motion.normal](ProfileAddMethod method) {
        const auto ticket = origin->Advance();
        const auto on_complete = [tasks, navigation, close, popDuration, origin, ticket] {
            if (!origin->CanNavigate(ticket)) return;
            if (close) { close(); return; }
            tasks.Launch([navigation, popDuration, origin, ticket]() -> huxerui::Task<void> {
                if (!origin->CanNavigate(ticket) || navigation.Depth() < 3) co_return;
                static_cast<void>(navigation.Pop());
                co_await huxerui::Delay(
                    std::chrono::duration<double>{popDuration});
                if (origin->CanNavigate(ticket) && navigation.Depth() == 2) {
                    static_cast<void>(navigation.Pop());
                }
            });
        };
        navigation.Push([=] {
            return ProfileCreateMethodPage(
                method, fields, tasks, toast, picker, http,
                pptp_supported, navigation, on_back,
                on_complete);
        });
    };
    const auto chooseFile = [fileImporting, origin, pickerTasks, toast, picker, http, finishImport, configFileFilter] {
        if (fileImporting) return;
        if (!picker) {
            toast.Show(Localized("文件选择器不可用"));
            return;
        }
        const auto ticket = origin->Advance();
        fileImporting = true;
        pickerTasks.Launch([fileImporting, ticket, picker, http, finishImport, toast, configFileFilter]() mutable
                         -> huxerui::Task<void> {
            struct ResetPicking {
                huxerui::State<bool> busy;
                bool handedOff = false;
                ~ResetPicking() { if (!handedOff) busy = false; }
            } picking{fileImporting};
            const auto picked = co_await PickProfileConfigFile(picker, configFileFilter, toast);
            if (!picked) {
                co_return;
            }
            const auto file = picked->AsFile();
            if (!file) {
                co_return;
            }
            const std::string path = file->Path();
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
            fileImporting = false;
            picking.handedOff = true;
            // The path alone does not retain the picker grant/temporary owner.
            LaunchProfileImport(fileImporting,
                [http, selected = *picked, request = std::move(request)]() mutable -> huxerui::Task<ProfileImportResult> {
                    co_return co_await ImportProfileForPlatform(http, std::move(request));
                }, [finishImport, ticket](auto result) { finishImport(ticket, std::move(result)); });
        });
    };

    huxerui::View methods = huxerui::Column {
        ProfileAddMethodRow(
            app::images::qr_scan, Localized("二维码"),
            Localized(qrImporting ? "正在扫描或导入配置…"
                                             : "扫描后自动导入订阅链接或配置"),
            "profile-add-qr", true,
            beginQrScan),
        ProfileAddMethodRow(
            app::images::file_import, Localized("文件"),
            Localized(fileImporting ? "正在导入配置…"
                                             : "从本地文件导入配置"),
            "profile-add-file", true,
            chooseFile),
        ProfileAddMethodRow(
            app::images::link, "URL", Localized("输入订阅链接并自动获取配置"),
            "profile-add-url", true,
            [openMethod] { openMethod(ProfileAddMethod::Url); }),
        ProfileAddMethodRow(
            app::images::manual_config, Localized("SSH 节点"),
            Localized("填写内网或远程 SSH 服务器"),
            "profile-add-ssh", pptp_supported,
            [openMethod] { openMethod(ProfileAddMethod::Ssh); }),
        pptp_supported
            ? huxerui::View{ProfileAddMethodRow(
                  app::images::manual_config, Localized("PPTP 内网"),
                  Localized("填写 PPTP 内网拨号参数"),
                  "profile-add-direct", false,
                  [openMethod] { openMethod(ProfileAddMethod::Direct); })}
            : huxerui::View{huxerui::Row{}},
    }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    huxerui::View content = ProfileFormScroll(methods)
        .With(huxerui::Grow(1.0F));
    return ProfileFlowPage(huxerui::Text(Localized("添加配置"), huxerui::TextRole::Title),
                            huxerui::View{}, std::move(content), rootBack, true)
        .With(ProfileSecondaryTransition(theme.motion),
              huxerui::Semantics{.role = huxerui::SemanticRole::Navigation,
                                 .label = huxerui::UseString(
                                     Localized("添加配置"))});
}

[[huxerui::composable]] huxerui::View ProfileAddDialogRoot(
    ProfileCreateFields fields, huxerui::TaskScope tasks,
    FluxToastHandle toast, std::shared_ptr<huxerui::FilePicker> picker,
    std::shared_ptr<AppHttpClient> http, bool pptp_supported,
    std::function<void()> close) {
    return ProfileAddMethodPage(fields, tasks, toast, picker, http,
        pptp_supported, huxerui::UseNavigation(), std::move(close));
}

[[huxerui::composable]] huxerui::View ProfileAddDialog(
    ProfileCreateFields fields, huxerui::TaskScope tasks,
    FluxToastHandle toast, std::shared_ptr<huxerui::FilePicker> picker,
    std::shared_ptr<AppHttpClient> http, bool pptp_supported,
    std::function<void()> close) {
    if (kPageTitlesInWindow) {
        auto dialogFields = fields;
        dialogFields.picked_file = huxerui::UseState(std::optional<huxerui::FileReference>{});
        return ProfileCreatePage(dialogFields, huxerui::UseTaskScope(), toast, picker, http,
            pptp_supported, close, /*embedded=*/true);
    }
    const auto guarded = UseProfileCreateClose(fields, close);
    return DialogCard(huxerui::ProvideEnvironment(
        ProfileFormDialog{true, std::make_shared<std::function<void()>>(guarded)},
        huxerui::NavigationStack(ProfileAddDialogRoot, fields, tasks, toast,
            picker, http, pptp_supported, close)
            .With(huxerui::Frame{.width = 520.0F, .height = 620.0F})));
}

[[huxerui::composable]] huxerui::View ProfileCreateMethodPage(
    ProfileAddMethod method, ProfileCreateFields fields,
    huxerui::TaskScope tasks, FluxToastHandle toast,
    std::shared_ptr<huxerui::FilePicker> picker,
    std::shared_ptr<AppHttpClient> http,
    bool pptp_supported,
    huxerui::NavigationController navigation, std::function<void()> on_back,
    std::function<void()> on_complete) {
    const auto importing = huxerui::UseState(false);
    const auto origin = UseProfileImportOrigin();
    const auto applicationTasks = huxerui::UseApplicationTaskScope();
    const auto go_back = [origin, on_back] {
        origin->Advance();
        if (on_back) on_back();
    };
    const auto profilesModel = huxerui::UseService<ProfilesModel>();
    const auto configFileFilter = ProfileConfigFileFilter();
    const std::string manualConfigName = huxerui::UseString(Localized("手动配置"));
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const std::string qrContent = fields.qr_content.Get().text;
    const bool qrRemote = method == ProfileAddMethod::Qr &&
                          IsHttpProfileUrl(qrContent);
    const bool isRemote = method == ProfileAddMethod::Url || qrRemote;
    const bool isFile = method == ProfileAddMethod::File;
    const bool isQr = method == ProfileAddMethod::Qr;
    const bool isDirect = method == ProfileAddMethod::Direct;
    const bool isSsh = method == ProfileAddMethod::Ssh;
    const bool directPptp = isDirect && pptp_supported;
    const auto cancel = UseProfileCreateClose(fields, go_back, importing);

    const auto import_profile = [=] {
        if (importing) return;
        std::string url;
        std::string inlineConfig;
        bool remote = isRemote;
        bool local = isFile;
        bool pptp = directPptp;
        if (method == ProfileAddMethod::Url) {
            url = fields.url.Get().text;
        } else if (isQr) {
            url = qrContent;
            if (!IsHttpProfileUrl(qrContent)) {
                remote = false;
                inlineConfig = qrContent;
            }
        }
        if (remote && profile_link::IsSupportedScheme(
                std::string_view(url).substr(0, url.find(':')))) {
            std::string error;
            const auto link = profile_link::Parse(url, error);
            if (!link) { toast.Show(error); return; }
            url = link->url;
            if (fields.name.Get().text.empty() && !link->name.empty())
                fields.name = huxerui::TextEditingValue{link->name};
        }
        if (remote && !profile_link::IsHttpUrl(url)) {
            toast.Show(Localized("订阅下载仅支持 HTTP 或 HTTPS URL"));
            return;
        }
        if ((remote && url.empty()) || (isQr && qrContent.empty())) {
            toast.Show(Localized(isQr ? "请先扫描二维码" : "订阅 URL 不能为空"));
            return;
        }
        const auto picked = local ? fields.picked_file.Get() : std::nullopt;
        const auto localFile = picked ? picked->AsFile() : std::nullopt;
        if (local && !localFile) {
            toast.Show(Localized("请先选择配置文件"));
            return;
        }
        if (isQr && !IsHttpProfileUrl(qrContent)) {
            if (inlineConfig.empty()) {
                toast.Show(Localized("配置内容不能为空"));
                return;
            }
        }

        model::Profile options;
        std::optional<SshProfileConfig> sshConfig;
        if (isSsh) {
            sshConfig = ReadSshProfile(fields.ssh.Get(), toast);
            if (!sshConfig) return;
        }
        options.description = fields.desc.Get().text;
        if (remote) {
            options.timeoutSecs = parseNumber(fields.timeout.Get(), 60);
            const auto minutes = ParseProfileUpdateInterval(fields.interval.Get().text);
            if (!minutes) { toast.Show(ProfileUpdateIntervalError()); return; }
            options.intervalMins = *minutes;
            options.autoUpdate = *minutes > 0;
            options.useSystemProxy = fields.system_proxy.Get();
            options.useCoreProxy = fields.core_proxy.Get();
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
        }

        const std::string name = fields.name.Get().text;
        const std::string pickedPath = localFile ? localFile->Path() : std::string{};
        const auto ticket = origin->Advance();
        LaunchProfileImport(importing, [=, picked = picked]() mutable -> huxerui::Task<ProfileImportResult> {
            static_cast<void>(picked);
            std::pair<std::int64_t, std::string> result;
            if (sshConfig) {
                result = co_await ImportSshProfile(name, std::move(*sshConfig), options);
            } else if (!inlineConfig.empty()) {
                result = co_await ImportProfileContent(
                    name.empty() ? manualConfigName
                                 : name,
                    inlineConfig, options);
            } else {
                result = co_await ImportProfileForPlatform(
                    http, ProfileImportRequest{
                              .remote = remote,
                              .local = local,
                              .pptp = pptp,
                                  .name = name,
                              .url = url,
                              .picked_path = pickedPath,
                              .options = options});
            }
            co_return result;
        }, [=](ProfileImportResult result) {
            profilesModel->PublishCached();
            if (result.first == 0) {
                toast.Show(result.second.empty() ? Localized("导入失败")
                                                 : huxerui::StringVariant(result.second));
                return;
            }
            toast.Show(directPptp ? Localized("配置已导入") : Localized("订阅已保存，正在准备资源"));
            StartProfilePreparation(applicationTasks, profilesModel, result.first, toast);
            if (origin->CanNavigate(ticket)) on_complete();
        });
    };

    huxerui::View typeFields;
    if (method == ProfileAddMethod::Url) {
        typeFields = huxerui::TextField(fields.url.Get())
                         .Label(Localized("订阅 URL"))
                         .Placeholder("https://...")
                         .Variant(huxerui::TextFieldVariant::Outlined)
                         .OnChanged([url = fields.url](
                                        const huxerui::TextEditingValue& value) {
                             url = value;
                         });
    } else if (isFile) {
        typeFields = FileDropZone(app::images::file_import,
                                  Localized("拖入或点击选择配置文件"),
                                  fields.picked_file, tasks, toast, picker,
                                  configFileFilter, "profile-file-drop-zone");
    } else if (isQr) {
        const bool scanned = !qrContent.empty();
        typeFields = huxerui::Column {
            huxerui::Text(Localized("将二维码放入取景框，支持订阅链接和配置文本。"))
                .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                          theme.colors.on_surface_variant}),
            huxerui::Button(Localized(scanned ? "重新扫描" : "开始扫码"))
                .OnClick([origin, navigation, toast,
                          content = fields.qr_content] {
                    origin->Advance();
                    PushProfileQrScanner(
                        navigation, toast,
                        [content](std::string result) {
                            content = huxerui::TextEditingValue{
                                std::move(result)};
                        });
                }),
            scanned
                ? huxerui::View{huxerui::Text(
                      qrRemote ? Localized("已识别订阅链接")
                               : LocalizedFormat("已识别配置文本 · {} 字符",
                                                 qrContent.size()))
                      .Style(huxerui::TextStyle{
                          huxerui::Font::System(font_size::kCaption),
                          theme.colors.on_surface_variant})}
                : huxerui::View{huxerui::Row{}},
        }.With(huxerui::Spacing(16.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    } else if (isDirect) {
        typeFields = PptpOptionsForm(
            fields.pptp_server, fields.pptp_username, fields.pptp_password,
            fields.pptp_timeout, fields.pptp_routes, fields.pptp_mppe);
    } else if (isSsh) {
        typeFields = SshOptionsForm(fields.ssh);
    }

    huxerui::View commonFields;
    bool hasCommonFields = false;
    if (isRemote || (isQr && qrRemote)) {
        commonFields = ProfileOptionsForm(
            fields.desc, fields.timeout, fields.interval,
            fields.system_proxy, fields.core_proxy);
        hasCommonFields = true;
    } else if (method == ProfileAddMethod::Url || isFile ||
               (isQr && !qrRemote) || isDirect || isSsh) {
        commonFields = huxerui::TextField(fields.desc.Get())
                           .Label(Localized("描述（可选）"))
                           .Variant(huxerui::TextFieldVariant::Outlined)
                           .OnChanged([desc = fields.desc](
                                          const huxerui::TextEditingValue& value) {
                               desc = value;
                           });
        hasCommonFields = true;
    }

    std::vector<huxerui::View> formFields;
    formFields.push_back(std::move(typeFields));
    formFields.push_back(huxerui::TextField(fields.name.Get())
                             .Label(Localized("名称（可选）"))
                             .Variant(huxerui::TextFieldVariant::Outlined)
                             .OnChanged([name = fields.name](
                                            const huxerui::TextEditingValue& value) {
                                 name = value;
                             }));
    if (hasCommonFields) formFields.push_back(std::move(commonFields));
    const huxerui::StringVariant title = Localized(
        method == ProfileAddMethod::Qr ? "扫码导入"
        : isFile                         ? "从文件导入"
        : method == ProfileAddMethod::Url ? "从 URL 导入"
        : isSsh                          ? "SSH 节点"
                                          : "PPTP 内网");
    huxerui::View content = ProfileFormScroll(
        huxerui::Column(std::move(formFields))
            .With(huxerui::Spacing(16.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
                                .With(huxerui::Grow(1.0F));
    huxerui::View action = FormHeaderAction(
        app::images::save, Localized("创建"), import_profile,
        !importing, importing);
    return ProfileFlowPage(huxerui::Text(title, huxerui::TextRole::Title),
                           ProfileImportAction(importing, Localized("导入配置"), import_profile),
                           std::move(content), isSsh ? cancel : std::function<void()>{go_back}, true,
                           std::move(action),
                           DialogAction(Localized("创建"), import_profile, true, !importing))
        .With(ProfileSecondaryTransition(theme.motion));
}

static ProfileCreateFields UseExternalProfileFields(std::string name, std::string url) {
    return {
        huxerui::UseState(huxerui::TextEditingValue{std::move(name)}),
        huxerui::UseState(huxerui::TextEditingValue{std::move(url)}),
        huxerui::UseState(huxerui::TextEditingValue{}),
        huxerui::UseState<std::size_t>(0),
        huxerui::UseState(huxerui::TextEditingValue{}),
        huxerui::UseState(huxerui::TextEditingValue{"60"}),
        huxerui::UseState(huxerui::TextEditingValue{"1440"}),
        huxerui::UseState(false), huxerui::UseState(false),
        huxerui::UseState(huxerui::TextEditingValue{}),
        huxerui::UseState(huxerui::TextEditingValue{}),
        huxerui::UseState(huxerui::TextEditingValue{}),
        huxerui::UseState(huxerui::TextEditingValue{"30"}),
        huxerui::UseState(huxerui::TextEditingValue{}),
        huxerui::UseState(true),
        huxerui::UseState(std::optional<huxerui::FileReference>{}),
        huxerui::UseState(SshProfileDraft{})};
}

#if defined(__ANDROID__)
[[huxerui::composable]] huxerui::View AndroidProfileScanShortcutPage(std::function<void()> onClose) {
    const auto navigation = huxerui::UseNavigation();
    const auto tasks = huxerui::UseTaskScope();
    const auto toast = UseFluxToast();
    const auto picker = huxerui::UseService<huxerui::FilePicker>();
    const auto http = huxerui::UseService<AppHttpClient>();
    const auto fields = UseExternalProfileFields({}, {});
    const auto close = [navigation] { static_cast<void>(navigation.Pop()); };
    const auto reviewBack = UseProfileCreateClose(fields, close);
    auto scanned = huxerui::UseState(false);
    huxerui::Lifecycle([onClose] { return onClose; }, 0);
    if (!scanned.Get()) {
        return ProfileQrScannerPage(close, [content = fields.qr_content, scanned](std::string result) {
            content = huxerui::TextEditingValue{std::move(result)};
            scanned = true;
        });
    }
    return ProfileCreateMethodPage(ProfileAddMethod::Qr, fields, tasks, toast, picker,
        http, false, navigation, reviewBack, close);
}
#endif

[[huxerui::composable]] huxerui::View ProfileLinkImportPage(
    profile_link::RemoteProfile request,
    FluxToastHandle toast, std::shared_ptr<huxerui::FilePicker> picker,
    std::shared_ptr<AppHttpClient> http, huxerui::NavigationController navigation,
    std::function<void()> close, huxerui::State<bool> showing) {
    auto tasks = huxerui::UseTaskScope();
    huxerui::Lifecycle([showing] { return [showing] { showing = false; }; }, 0);
    // Separate form state preserves an unfinished ordinary import.
    const auto fields = UseExternalProfileFields(std::move(request.name), std::move(request.url));
    const auto back = UseProfileCreateClose(fields, close);
    return ProfileCreateMethodPage(ProfileAddMethod::Url, fields, tasks, toast,
        picker, http, false, navigation, back, close);
}

[[huxerui::composable]] huxerui::View ProfileLinkImportEffects(
    std::shared_ptr<ProfilesModel> model, huxerui::NavigationController navigation,
    bool navigation_enabled, bool active) {
    auto tasks = huxerui::UseTaskScope();
    auto toast = UseFluxToast();
    auto dialog = huxerui::UseDialog();
    auto picker = huxerui::UseService<huxerui::FilePicker>();
    auto http = huxerui::UseService<AppHttpClient>();
    auto showing = huxerui::UseState(false);
    huxerui::Lifecycle([=] {
        if (active && !showing.Get() && !model->importLinks.Get().empty()) {
            tasks.Post([=] {
                if (showing.Get()) return;
                auto links = model->importLinks.Get();
                if (links.empty()) return;
                const auto request = links.front();
                links.erase(links.begin());
                model->importLinks = std::move(links);
                showing = true;
                if (navigation_enabled) {
                    navigation.Push([=] {
                        const auto close = [navigation] { static_cast<void>(navigation.Pop()); };
                        return ProfileLinkImportPage(request, toast, picker,
                            http, navigation, close, showing);
                    });
                } else {
                    ShowEditorDialog(dialog,[=](huxerui::DialogContext context) {
                        const auto close = [context] { static_cast<void>(context.Dismiss()); };
                        return DialogCard(huxerui::ProvideEnvironment(ProfileFormDialog{true, {}},
                            huxerui::NavigationStack([=] {
                            return ProfileLinkImportPage(request, toast, picker,
                                http, navigation, close, showing);
                        })
                            .With(huxerui::Frame{.width = 520.0F, .height = 620.0F})));
                    });
                }
            });
        }
        return [] {};
    }, active, model->importLinks, showing);
    return huxerui::Row{}.Key("profile-link-effects");
}

[[huxerui::composable]] huxerui::View ProfileEditPage(
    std::int64_t id, huxerui::State<huxerui::TextEditingValue> name,
    huxerui::State<huxerui::TextEditingValue> url,
    huxerui::State<std::string> type,
    huxerui::State<huxerui::TextEditingValue> desc,
    huxerui::State<huxerui::TextEditingValue> timeout,
    huxerui::State<huxerui::TextEditingValue> interval,
    huxerui::State<bool> system_proxy,
    huxerui::State<bool> core_proxy,
    huxerui::State<huxerui::TextEditingValue> pptp_server,
    huxerui::State<huxerui::TextEditingValue> pptp_username,
    huxerui::State<huxerui::TextEditingValue> pptp_password,
    huxerui::State<huxerui::TextEditingValue> pptp_timeout,
    huxerui::State<huxerui::TextEditingValue> pptp_routes,
    huxerui::State<bool> pptp_mppe,
    huxerui::TaskScope tasks, FluxToastHandle toast,
    std::function<void()> on_back, bool inDialog = false) {
    const auto profilesModel = huxerui::UseService<ProfilesModel>();
    const auto saving = huxerui::UseState(false);
    const auto alive = huxerui::UseState(true);
    huxerui::Lifecycle([alive] { return [alive] { alive = false; }; }, 0);
    const auto snapshot = [=] {
        return std::tuple{name.Get().text, url.Get().text, type.Get(), desc.Get().text,
            timeout.Get().text, interval.Get().text, system_proxy.Get(),
            core_proxy.Get(), pptp_server.Get().text, pptp_username.Get().text,
            pptp_password.Get().text, pptp_timeout.Get().text, pptp_routes.Get().text, pptp_mppe.Get()};
    };
    const auto baseline = huxerui::UseState(snapshot());
    const auto close = UseEditorClose([=] { return snapshot() != baseline.Get(); },
        [saving] { return saving.Get(); }, on_back);
    const auto save = [=] {
        if (saving.Get()) return;
        const auto minutes = type.Get() == "pptp" ? std::optional<int>{0}
            : ParseProfileUpdateInterval(interval.Get().text);
        if (!minutes) { toast.Show(ProfileUpdateIntervalError()); return; }
        saving = true;
        tasks.Launch([=]() -> huxerui::Task<void> {
            model::Profile fields;
            fields.name = name.Get().text;
            fields.type = type.Get();
            fields.description = desc.Get().text;
            fields.timeoutSecs = parseNumber(timeout.Get(), 60);
            fields.intervalMins = *minutes;
            fields.autoUpdate = *minutes > 0;
            fields.useSystemProxy = system_proxy.Get();
            fields.useCoreProxy = core_proxy.Get();
            if (fields.type == "pptp") {
                std::string configError;
                const auto config = makePptpConfig(
                    pptp_server.Get(), pptp_username.Get(), pptp_password.Get(),
                    pptp_timeout.Get(), pptp_mppe.Get(), configError);
                if (!config) {
                    saving = false;
                    toast.Show(configError);
                    co_return;
                }
                fields.nativeConfig = *config;
                fields.nativeRoutes = joinRoutes(parseRouteField(pptp_routes.Get().text));
                fields.url.clear();
            } else {
                fields.url = url.Get().text;
            }
            std::string err;
            try { err = co_await RunOnTaskThread(
                [id, fields] {
                    auto& ps = store::profilesStore();
                    if (!ps.updateProfile(id, fields)) return ps.lastError();
                    return std::string{};
                });
            } catch (const std::exception& exception) { err = exception.what(); }
            saving = false;
            if (err.empty()) {
                profilesModel->PublishCached();
                toast.Show(Localized("已保存"));
                if (alive.Get()) on_back();
            } else {
                toast.Show(err);
            }
        });
    };

    const auto body = ProfileFormScroll(
            huxerui::Column {
                    huxerui::TextField(name.Get())
                        .Label(Localized("名称"))
                        .Variant(huxerui::TextFieldVariant::Outlined)
                        .OnChanged([name](const huxerui::TextEditingValue& value) {
                            name = value;
                        }),
                    type.Get() == "pptp"
                        ? huxerui::View{PptpOptionsForm(
                              pptp_server, pptp_username, pptp_password,
                              pptp_timeout, pptp_routes, pptp_mppe)}
                        : huxerui::View{huxerui::Column {
                              huxerui::TextField(url.Get())
                                  .Label(Localized("订阅链接（留空 = 本地导入）"))
                                  .Placeholder("https://...")
                                  .Variant(huxerui::TextFieldVariant::Outlined)
                                  .OnChanged([url](
                                                 const huxerui::TextEditingValue& value) {
                                      url = value;
                                  }),
                              ProfileOptionsForm(desc, timeout, interval,
                                                 system_proxy,
                                                 core_proxy),
                          }.With(huxerui::Spacing(16.0F),
                              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))},
                    type.Get() == "pptp"
                        ? huxerui::View{huxerui::TextField(desc.Get())
                                            .Label(Localized("描述（可选）"))
                                            .Variant(huxerui::TextFieldVariant::Outlined)
                                            .OnChanged([desc](
                                                           const huxerui::TextEditingValue& value) {
                                                desc = value;
                                            })}
                        : huxerui::View{huxerui::Row{}},
            }
                .With(huxerui::Spacing(16.0F),
                      huxerui::CrossAlign(
                          huxerui::CrossAxisAlignment::Stretch)), inDialog)
                .With(huxerui::Grow(1.0F), huxerui::Enabled(!saving.Get()));
    if (inDialog) return DialogScaffold(DialogHeader(Localized("编辑订阅")),
        huxerui::View{body}.With(huxerui::Enabled(!saving.Get())),
        DialogActions({DialogAction(Localized("取消"), close, false, !saving.Get()),
            DialogAction(Localized("保存"), save, true, !saving.Get())}), DialogSize::Form, true);
    return ProfileFlowPage(huxerui::Text(Localized("编辑订阅"), huxerui::TextRole::Title),
        huxerui::IconButton(app::images::save, Localized("保存"))
            .With(huxerui::Enabled(!saving.Get())).OnClick(save), body, close, false,
        FormHeaderAction(app::images::save, Localized("保存"), save, !saving.Get(), saving.Get()));
}

[[huxerui::composable]] huxerui::View ProfileEditDialogContent(std::int64_t id, ProfileEditFields fields,
    huxerui::TaskScope tasks, FluxToastHandle toast, std::function<void()> close) {
    return ProfileEditPage(id, fields.name, fields.url, fields.type, fields.desc, fields.timeout,
        fields.interval, fields.system_proxy, fields.core_proxy,
        fields.pptp_server, fields.pptp_username, fields.pptp_password, fields.pptp_timeout,
        fields.pptp_routes, fields.pptp_mppe, tasks, toast, close, true);
}

[[huxerui::composable]] huxerui::View ProfileEditRoutePage(
    std::int64_t id, ProfileEditFields fields, huxerui::TaskScope tasks,
    FluxToastHandle toast, std::function<void()> on_back) {
    return ProfileEditPage(
        id, fields.name, fields.url, fields.type, fields.desc, fields.timeout,
        fields.interval, fields.system_proxy,
        fields.core_proxy, fields.pptp_server,
        fields.pptp_username, fields.pptp_password, fields.pptp_timeout,
        fields.pptp_routes, fields.pptp_mppe, tasks, toast, std::move(on_back))
        .With(ProfileSecondaryTransition(huxerui::UseTheme().motion));
}

[[huxerui::composable]] huxerui::View ResponsiveProfileEditSurface(
    std::int64_t id, ProfileEditFields fields, huxerui::TaskScope tasks,
    FluxToastHandle toast, std::function<void()> on_back) {
    return huxerui::Scope(
        [id, fields, tasks, toast, on_back]() -> huxerui::View {
            // 非激活时必须保持空页：IndexedPages 常驻挂载每一页，而桌面编辑
            // 弹窗与本页绑定同一组表单 State——两个受控 TextField 同时挂载时，
            // 输入法组合值会被另一实例当作外部权威值而抛 std::invalid_argument。
            if (id == 0) return huxerui::Row{};
            return ProfileEditPage(
                id, fields.name, fields.url, fields.type, fields.desc,
                fields.timeout, fields.interval,
                fields.system_proxy, fields.core_proxy,
                fields.pptp_server, fields.pptp_username, fields.pptp_password,
                fields.pptp_timeout, fields.pptp_routes, fields.pptp_mppe,
                tasks, toast,
                on_back);
        });
}
[[huxerui::composable]] huxerui::View ResponsiveProfileCreateSurface(
    bool open, ProfileCreateFields fields, huxerui::TaskScope tasks,
    FluxToastHandle toast, std::shared_ptr<huxerui::FilePicker> picker,
    std::shared_ptr<AppHttpClient> http, bool pptp_supported,
    std::function<void()> on_back) {
    return huxerui::Scope([=]() -> huxerui::View {
        // 同编辑页：未打开时不得挂载受控输入框（见 ResponsiveProfileEditSurface）。
        if (!open) return huxerui::Row{};
        return ProfileCreatePage(fields, tasks, toast, picker, http,
                                 pptp_supported, on_back);
    });
}
namespace {
huxerui::Task<void> SaveProfileDocument(
    std::int64_t id, std::shared_ptr<const std::string> text,
    huxerui::State<bool> saving, FluxToastHandle toast,
    std::shared_ptr<ProfilesModel> model, std::function<void()> done) {
    std::string error;
    try {
        error = co_await RunOnTaskThread([id, text] {
            auto& profiles = store::profilesStore();
            return profiles.saveYaml(id, *text) ? std::string{} : profiles.lastError();
        });
    } catch (const std::exception& exception) { error = exception.what(); }
    saving = false;
    if (!error.empty()) { toast.Show(error); co_return; }
    model->PublishCached();
    toast.Show(Localized("已保存"));
    if (done) done();
}

std::function<void()> UseDocumentClose(huxerui::State<TextDocumentHandle> document,
    huxerui::State<bool> saving, std::function<void()> close) {
    const auto initial = huxerui::UseState(std::shared_ptr<const std::string>{});
    huxerui::Lifecycle([document, initial] {
        if (document.Get() && !initial.Get()) initial = document.Get()->InitialText();
    }, document);
    return UseEditorClose([=] {
        return document.Get() && initial.Get() && *document.Get()->Snapshot() != *initial.Get();
    }, [saving] { return saving.Get(); }, close);
}

[[huxerui::composable]] huxerui::View ProfileDocumentBody(
    huxerui::State<TextDocumentHandle> content, bool loading, bool saving, float height) {
    if (loading) {
        huxerui::View pending = huxerui::Row{};
        if (height > 0.0F) return std::move(pending).With(huxerui::Frame{.height = height});
        return std::move(pending).With(huxerui::Grow(1.0F));
    }
    if (!content.Get()) return huxerui::Text(Localized("无法读取订阅文件"));
    return DocumentTextView(content.Get(), false, height).With(huxerui::Enabled(!saving));
}
}

[[huxerui::composable]] huxerui::View ProfileFilePage(
    std::int64_t id, huxerui::State<TextDocumentHandle> content,
    huxerui::State<bool> loading, huxerui::TaskScope pageTasks,
    FluxToastHandle toast, std::function<void()> on_back) {
    const auto profilesModel = huxerui::UseService<ProfilesModel>();
    auto tasks = huxerui::UseApplicationTaskScope();
    auto saving = huxerui::UseState(false);
    auto alive = huxerui::UseState(true);
    huxerui::Lifecycle([alive] { return [alive] { alive = false; }; }, 0);
    const auto back = [=] { if (alive.Get() && !saving.Get()) on_back(); };
    const auto close = UseDocumentClose(content, saving, back);
    const auto save = [=] {
        if (loading.Get() || saving.Get() || !content.Get() || !content.Get()->Ready()) return;
        const auto text = content.Get()->Snapshot();
        saving = true;
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await SaveProfileDocument(id, text, saving, toast, profilesModel, back);
        });
    };
    return ProfileFlowPage(
        huxerui::Text(Localized("编辑订阅文件"), huxerui::TextRole::Title),
        huxerui::IconButton(app::images::save, Localized("保存文件"))
            .With(huxerui::Tooltip(Localized("保存订阅文件")),
                  huxerui::Enabled(!loading.Get() && !saving.Get() && content.Get() != nullptr && content.Get()->Ready()))
            .OnClick(save),
        huxerui::Column {
            huxerui::Text(Localized("直接编辑订阅 YAML；保存后启用中的订阅会重启生效。")),
            ProfileDocumentBody(content, loading.Get(), saving.Get(), 0.0F),
        }.With(huxerui::Spacing(16.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
               huxerui::Grow(1.0F)),
        close, false,
        FormHeaderAction(app::images::save, Localized("保存"), save,
            !loading.Get() && !saving.Get() && content.Get() != nullptr && content.Get()->Ready(),
            saving.Get()));
}

[[huxerui::composable]] huxerui::View ProfileFileRoutePage(
    std::int64_t id, huxerui::State<TextDocumentHandle> content,
    huxerui::State<bool> loading, huxerui::TaskScope tasks,
    FluxToastHandle toast, std::function<void()> on_back) {
    return ProfileFilePage(id, content, loading, tasks, toast, std::move(on_back))
        .With(ProfileSecondaryTransition(huxerui::UseTheme().motion));
}

[[huxerui::composable]] huxerui::View ProfileFileDialog(
    std::int64_t id, huxerui::State<TextDocumentHandle> content,
    huxerui::State<bool> loading, FluxToastHandle toast, huxerui::DialogContext context) {
    const auto profilesModel = huxerui::UseService<ProfilesModel>();
    auto tasks = huxerui::UseApplicationTaskScope();
    auto saving = huxerui::UseState(false);
    const auto done = [context] { context.Dismiss(); };
    const auto close = UseDocumentClose(content, saving, done);
    const auto save = [=] {
        if (loading.Get() || saving.Get() || !content.Get() || !content.Get()->Ready()) return;
        const auto text = content.Get()->Snapshot();
        saving = true;
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await SaveProfileDocument(id, text, saving, toast, profilesModel, done);
        });
    };
    return DialogScaffold(DialogHeader(Localized("编辑文件")), huxerui::Column {
      huxerui::Text(Localized("直接编辑订阅原文，远程更新会覆盖这些改动。需要保留的规则请使用编辑规则。")),
      ProfileDocumentBody(content, loading.Get(), saving.Get(), 0.0F),
    }.With(huxerui::Spacing(12.0F), huxerui::Grow(1.0F),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
        DialogActions({DialogAction(Localized("取消"), close, false, !saving.Get()),
            DialogAction(Localized("保存"), save, true,
                !loading.Get() && !saving.Get() && content.Get() && content.Get()->Ready())}), DialogSize::Large, true);
}

} // namespace clashflux::ui
