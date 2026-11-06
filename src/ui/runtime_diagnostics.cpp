#include <huxerui/huxerui.h>
#include "runtime_diagnostics.h"
#include "ui.h"
#include "page_layout.h"
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
        }.With(huxerui::Spacing(12.0F), huxerui::Padding(huxerui::EdgeInsets::Symmetric(0.0F, 8.0F)),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    } else if (hasReport) {
        reportArea = DocumentTextView(document.Get(), true, compact ? 200.0F : 260.0F)
            .Key("diagnostics-report");
    } else {
        reportArea = huxerui::Text(Localized("诊断完成后，可在此查看并导出脱敏报告。"))
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption),
                                      theme.colors.on_surface_variant});
    }

    const auto fields = huxerui::Column {
      huxerui::Text(Localized("留空仅检查运行状态；填写 HTTP(S) 目标后，通过当前代理入站检查。结果不代表全部网络。"))
          .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kBody), theme.colors.on_surface_variant}),
      huxerui::TextField(target.Get()).Label(Localized("检查目标（可选）"))
          .Variant(huxerui::TextFieldVariant::Outlined).With(huxerui::Enabled(!busy))
          .OnChanged([target](const huxerui::TextEditingValue& value) { target = value; }).Key("diagnostics-target"),
      reportArea,
      huxerui::Flow {
        DialogAction(Localized("复制脱敏报告"), copy, false, !busy && hasReport),
        save ? DialogAction(Localized(saving ? "正在保存…" : "保存脱敏报告"), save, false,
            canSave && !saving && !busy && hasReport) : huxerui::View{},
      }.With(huxerui::Spacing(8.0F)).Key("diagnostics-export-actions"),
    }.With(huxerui::Spacing(16.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    const auto body = (fullPage ? ScrollablePageContent(fields) : huxerui::View{huxerui::ScrollView(fields)})
        .With(huxerui::Grow(1.0F), huxerui::ScrollBar()).Key("runtime-diagnostics-content");
    const auto actions = DialogActions({
      fullPage ? huxerui::View{} : DialogAction(Localized("关闭"), close, false),
      busy ? DialogAction(Localized("取消"), cancel, false) : huxerui::View{},
      DialogAction(Localized("开始诊断"), start, true, !busy && canRun),
    });
    return huxerui::Column {
      fullPage ? huxerui::View{} : DialogHeader(Localized("运行诊断")),
      body,
      actions,
    }.With(huxerui::Grow(1.0F), huxerui::Spacing(16.0F),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}
}
