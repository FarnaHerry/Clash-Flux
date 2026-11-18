#pragma once

#include <huxerui/huxerui.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "page_layout.h"
#include "settings_group_layout.h"

namespace clashflux::ui {

inline huxerui::View NetworkDnsFormFields(
    std::vector<huxerui::View> notices, bool overrideDns, std::function<void(bool)> onOverrideChanged,
    huxerui::TextEditingValue remoteDns, std::string remoteDnsError,
    std::function<void(const huxerui::TextEditingValue&)> onRemoteDnsChanged, bool busy,
    float horizontalInset) {
    return huxerui::Scope([notices, overrideDns, onOverrideChanged, remoteDns, remoteDnsError,
                           onRemoteDnsChanged, busy, horizontalInset] {
        const auto& theme = huxerui::UseTheme();
        std::vector<huxerui::View> rows;
        if (!notices.empty()) {
            rows.push_back(SettingsContentRow(huxerui::Column(notices).With(
                huxerui::Spacing(4.0F),
                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)).Key("network-dns-notices"), 4.0F));
        }
        rows.push_back(SettingSwitchRow(
            Localized("覆写 DNS"),
            Localized("启用后，以下设置覆盖未自带 DNS 的订阅；关闭则跟随系统 DNS"),
            huxerui::Switch(overrideDns)
                .OnChanged(onOverrideChanged)
                .Key("network-dns-override")));
        if (overrideDns) {
            rows.push_back(SettingsContentRow(
                huxerui::TextField(remoteDns)
                    .Label(Localized("远程 DNS（DoH，可空）"))
                    .Variant(huxerui::TextFieldVariant::Outlined)
                    .Validation(remoteDnsError.empty()
                        ? huxerui::ValidationResult::Valid()
                        : huxerui::ValidationResult::Invalid(remoteDnsError))
                    .LineLimits(huxerui::TextFieldLineLimits::SingleLine())
                    .OnChanged(onRemoteDnsChanged)
                    .Key("network-field-remote_dns")));
            rows.push_back(SettingsContentRow(
                huxerui::Text(Localized(
                    "用于需要本地解析的域名；留空跟随系统 DNS，仅对未自带 DNS 的订阅生效，支持 https://1.1.1.1/dns-query 等 IP 地址形式。"))
                    .Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kCaption),
                        theme.colors.on_surface_variant}),
                4.0F).Key("network-dns-hint"));
        }
        return huxerui::Column {
          huxerui::ProvideEnvironment(
              SettingsRowInsets{horizontalInset, 8.0F, 48.0F},
              huxerui::Column(std::move(rows)).With(
                  huxerui::Spacing(0.0F),
                  huxerui::Enabled(!busy),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))),
        }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)).Key("network-dns-form");
    });
}

inline huxerui::View NetworkDnsPageBody(
    std::vector<huxerui::View> notices, bool overrideDns, std::function<void(bool)> onOverrideChanged,
    huxerui::TextEditingValue remoteDns, std::string remoteDnsError,
    std::function<void(const huxerui::TextEditingValue&)> onRemoteDnsChanged, bool busy,
    std::shared_ptr<PageScrollOffset> scroll) {
    return huxerui::Scope([notices, overrideDns, onOverrideChanged, remoteDns, remoteDnsError,
                           onRemoteDnsChanged, busy, scroll] {
        const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
        huxerui::View formContent = NetworkDnsFormFields(
            notices, overrideDns, onOverrideChanged, remoteDns, remoteDnsError,
            onRemoteDnsChanged, busy, compact ? 12.0F : 0.0F);
        if (!compact) {
            formContent = huxerui::Row {
              huxerui::Spacer(),
              huxerui::View{formContent}.With(huxerui::Frame{.max_width = 720.0F}),
              huxerui::Spacer(),
            }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Start));
        }
        huxerui::View body = ScrollablePageContent(formContent, scroll).Key("network-dns-scroll");
        if (compact) body = FormPageContent(body);
        return body;
    });
}

inline huxerui::View NetworkDnsDialogView(
    std::vector<huxerui::View> notices, bool overrideDns, std::function<void(bool)> onOverrideChanged,
    huxerui::TextEditingValue remoteDns, std::string remoteDnsError,
    std::function<void(const huxerui::TextEditingValue&)> onRemoteDnsChanged,
    bool busy, bool valid, bool canApply,
    std::function<void()> onCancel, std::function<void(bool)> onSave,
    std::shared_ptr<PageScrollOffset> scroll) {
    const auto fields = NetworkDnsFormFields(
        std::move(notices), overrideDns, std::move(onOverrideChanged),
        std::move(remoteDns), std::move(remoteDnsError),
        std::move(onRemoteDnsChanged), busy, 0.0F);
    const auto actions = DialogActions({
        DialogAction(Localized("取消"), onCancel, false, !busy).Key("network-dns-cancel"),
        DialogAction(Localized("保存"), [onSave] { onSave(false); }, !canApply, valid).Key("network-dns-save"),
        canApply ? DialogAction(Localized("保存并应用"), [onSave] { onSave(true); }, true, valid)
                       .Key("network-dns-save-apply")
                 : huxerui::View{},
    });
    return DialogScaffold(
        DialogHeader(Localized("DNS 设置")),
        PageScrollView(fields, {}, std::move(scroll)).With(huxerui::Frame{.max_height = 420.0F})
            .Key("network-dns-scroll"),
        actions,
        DialogSize::Form)
        .On<huxerui::ViewEvents::BackRequested>(std::move(onCancel))
        .Key("settings-network-dns-dialog");
}

} // namespace clashflux::ui
