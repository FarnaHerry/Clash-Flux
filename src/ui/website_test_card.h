#pragma once
#include "website_test_model.h"
#include "ui.h"
#include "app_resources.h"

namespace clashflux::ui {

[[huxerui::composable]] inline huxerui::View WebsiteTestCardContent(
    WebsiteTestSite site, bool enabled, std::function<void()> test) {
    const auto& theme = huxerui::UseTheme();
    const auto& result = site.result;
    auto status = Localized("未测试");
    auto color = theme.colors.on_surface_variant;
    if (result.phase == WebsiteTestPhase::Queued) status = Localized("等待测试");
    if (result.phase == WebsiteTestPhase::Running) { status = Localized("测试中…"); color = theme.colors.primary; }
    if (result.phase == WebsiteTestPhase::Cancelled) status = Localized("已取消");
    if (result.phase == WebsiteTestPhase::Failed) { status = Localized("连接失败"); color = theme.colors.error; }
    if (result.phase == WebsiteTestPhase::Responded) {
        status = result.status >= 200 && result.status < 400 ? Localized("访问成功") : Localized("网站已响应");
        color = result.status >= 200 && result.status < 400 ? SemanticSuccessColor(theme) : SemanticWarningColor(theme);
    }
    return huxerui::Column {
      huxerui::Row {
        huxerui::Image(app::images::globe).Tint(theme.colors.primary).With(huxerui::Frame{.width = 20, .height = 20}),
        huxerui::Text(site.name, huxerui::TextRole::Title).With(huxerui::Grow(1.0F)),
        DialogAction(Localized("测试"), test, false, enabled).Key("website-test-" + site.name),
      }.With(huxerui::Spacing(8.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
      huxerui::Text(site.url).Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption),
          theme.colors.on_surface_variant}),
      huxerui::Row {
        huxerui::Text(status).With(huxerui::Foreground(color), huxerui::Grow(1.0F)),
        result.phase == WebsiteTestPhase::Responded
            ? huxerui::View{huxerui::Text("HTTP " + std::to_string(result.status))} : huxerui::View{},
        (result.phase == WebsiteTestPhase::Responded || result.phase == WebsiteTestPhase::Failed)
            ? huxerui::View{huxerui::Text(LocalizedFormat("{} ms", result.elapsedMs))} : huxerui::View{},
      }.With(huxerui::Spacing(8.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
      result.error.empty() ? huxerui::View{} : huxerui::View{
        huxerui::Text(result.error)
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption), theme.colors.on_surface_variant})},
    }.With(huxerui::Spacing(8.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace clashflux::ui
