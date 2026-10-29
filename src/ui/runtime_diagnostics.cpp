#include <huxerui/huxerui.h>
#include "runtime_diagnostics.h"
#include "ui.h"
#include "text_document.h"

namespace clashflux::ui {
[[huxerui::composable]] huxerui::View RuntimeDiagnosticsContent(
    huxerui::State<huxerui::TextEditingValue> target, bool busy, bool canRun,
    std::shared_ptr<const std::string> report, std::function<void()> start, std::function<void()> cancel,
    std::function<void()> copy, std::function<void()> close,
    std::function<void()> save, bool canSave, bool saving, bool fullPage) {
    const auto& theme = huxerui::UseTheme();
    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    const bool hasReport = report && !report->empty();
    auto document = huxerui::UseState(TextDocumentHandle{});
    if (!document.Get() || document.Get()->InitialText() != report)
        document = std::make_shared<TextDocument>(report);
    auto secondary = ResolveFluxSecondaryButtonStyle(theme);
    secondary.minimum_height = 40.0F;

    huxerui::View reportArea;
    if (busy) {
        reportArea = huxerui::Column {
            huxerui::ProgressBar{},
            huxerui::Text(Localized("正在收集诊断证据…")),
        }.With(huxerui::Spacing(12.0F), huxerui::Padding(16.0F),
               huxerui::Background(theme.colors.surface_container_low),
               huxerui::CornerRadius(theme.shapes.small),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    } else if (hasReport) {
        reportArea = DocumentTextView(document.Get(), true, compact ? 200.0F : 260.0F)
            .Key("diagnostics-report");
    } else {
        reportArea = huxerui::Text(Localized("诊断完成后，可在此查看并导出脱敏报告。"))
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption),
                                      theme.colors.on_surface_variant});
    }

    huxerui::View saveButton;
    if (save) saveButton = huxerui::Button(Localized(saving ? "正在保存…" : "保存脱敏报告"))
        .With(huxerui::Enabled(canSave && !saving && !busy && hasReport))
        .OnClick(std::move(save));
    huxerui::View cancelButton;
    if (busy) cancelButton = huxerui::Button(Localized("取消")).OnClick(std::move(cancel));
    auto primary = ResolveFluxButtonStyle(theme);
    primary.minimum_height = 40.0F;
    huxerui::View content = huxerui::ScrollView(huxerui::Column {
        fullPage ? huxerui::View{} : huxerui::Text(Localized("运行诊断"), huxerui::TextRole::Title),
        huxerui::Text(Localized("留空仅检查运行状态；填写 HTTP(S) 目标后，通过当前代理入站检查。结果不代表全部网络。"))
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                      theme.colors.on_surface_variant}),
        huxerui::TextField(target.Get()).Label(Localized("检查目标（可选）"))
            .Variant(huxerui::TextFieldVariant::Outlined)
            .With(huxerui::Enabled(!busy)).OnChanged([target](const huxerui::TextEditingValue& value) { target = value; })
            .Key("diagnostics-target"),
        std::move(reportArea),
        huxerui::ProvideEnvironment(secondary, huxerui::Flow {
            huxerui::Button(Localized("复制脱敏报告"))
                .With(huxerui::Enabled(!busy && hasReport)).OnClick(std::move(copy)),
            std::move(saveButton),
        }.With(huxerui::Spacing(8.0F)).Key("diagnostics-export-actions")),
        huxerui::Flow {
            huxerui::Button(Localized("开始诊断"))
                .With(huxerui::Enabled(!busy && canRun)).OnClick(std::move(start)),
            huxerui::ProvideEnvironment(secondary, huxerui::Flow {
                std::move(cancelButton),
                fullPage ? huxerui::View{} : huxerui::Button(Localized("关闭")).OnClick(std::move(close)),
            }.With(huxerui::Spacing(8.0F))),
        }.With(huxerui::Spacing(8.0F)).Key("diagnostics-run-actions"),
    }.With(huxerui::Spacing(16.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
        .With(huxerui::ScrollBar())
        .Key("runtime-diagnostics-content");
    if (fullPage) content = huxerui::View{content}.With(huxerui::Grow(1.0F));
    else content = huxerui::View{content}.With(huxerui::Frame{
        .width = compact ? 320.0F : 520.0F, .max_height = 560.0F});
    return huxerui::ProvideEnvironment(primary, content);
}
}
