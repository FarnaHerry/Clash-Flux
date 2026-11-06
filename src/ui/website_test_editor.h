#pragma once
#include "website_test_model.h"
#include "ui.h"
#include "page_layout.h"

namespace clashflux::ui {

inline huxerui::View WebsiteTestForm(std::shared_ptr<WebsiteTestModel> model) {
    return huxerui::Scope([model] {
        return huxerui::Column {
          huxerui::TextField(model->customUrl.Get()).Label(Localized("网站地址"))
              .Placeholder(Localized("example.com（默认 HTTPS）"))
              .InputConfiguration({.type = huxerui::TextInputType::Url})
              .Variant(huxerui::TextFieldVariant::Outlined)
              .OnChanged([model](const huxerui::TextEditingValue& value) { model->customUrl = value; })
              .Key("website-editor-url"),
          huxerui::TextField(model->customName.Get()).Label(Localized("备注（可选）"))
              .Variant(huxerui::TextFieldVariant::Outlined)
              .OnChanged([model](const huxerui::TextEditingValue& value) { model->customName = value; })
              .Key("website-editor-name"),
        }.With(huxerui::Spacing(16.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    });
}

inline huxerui::View WebsiteTestPageForm(std::shared_ptr<WebsiteTestModel> model) {
    return FormPageContent(ScrollablePageContent(WebsiteTestForm(model)).Key("website-form-scroll"));
}

inline std::function<void()> UseWebsiteEditorClose(std::shared_ptr<WebsiteTestModel> model,
    std::function<void()> close) {
    const auto initialUrl = huxerui::UseState(model->customUrl.Get().text);
    const auto initialName = huxerui::UseState(model->customName.Get().text);
    return UseEditorClose([model, initialUrl, initialName] {
        return model->customUrl.Get().text != initialUrl.Get() || model->customName.Get().text != initialName.Get();
    }, [] { return false; }, std::move(close));
}

inline huxerui::View WebsiteTestEditorDialog(huxerui::DialogContext context,
    std::shared_ptr<WebsiteTestModel> model, bool editing, std::function<bool()> submit) {
    return huxerui::Scope([context, model, editing, submit] {
        const auto close = UseWebsiteEditorClose(model, [context] { context.Dismiss(); });
        return DialogScaffold(DialogHeader(editing ? Localized("编辑网站") : Localized("添加网站")),
            WebsiteTestForm(model), DialogActions({
                DialogAction(Localized("取消"), close, false).Key("website-editor-cancel"),
                DialogAction(editing ? Localized("保存") : Localized("添加网站"),
                    [context, submit] { if (submit()) context.Dismiss(); }, true).Key("website-editor-submit"),
            }), DialogSize::Small);
    });
}

} // namespace clashflux::ui
