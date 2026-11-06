#include <huxerui/huxerui.h>
#include "app_resources.h"
#include "ui.h"
#include "page_layout.h"
#include "website_test_card.h"
#include "website_test_grid.h"
#include "website_test_editor.h"
#include "empty_state.h"

namespace clashflux::ui {

namespace {

// Add and edit share fields and validation across presentation sizes.
[[huxerui::composable]] huxerui::View WebsiteTestEditorPage(std::shared_ptr<WebsiteTestModel> model,
    bool editing, std::function<bool()> submit) {
    const auto navigation = huxerui::UseNavigation();
    const auto back = [navigation] { static_cast<void>(navigation.Pop()); };
    const auto close = UseWebsiteEditorClose(model, back);
    const auto title = editing ? Localized("编辑网站") : Localized("添加网站");
    const auto action = editing ? Localized("保存") : Localized("添加网站");
    const auto icon = editing ? app::images::check : app::images::add;
    const auto commit = [submit, back] { if (submit()) back(); };
    huxerui::View content = WebsiteTestPageForm(model);
    return SecondaryPageScaffold(huxerui::Text(title),
        FormHeaderAction(icon, action, commit),
        content, close);
}

} // namespace

[[huxerui::composable]] huxerui::View WebsiteTestsPage(std::function<void()> onBack, bool active) {
    const float pageInset = ResolveIslandTheme(huxerui::UseTheme()).nested_radius;
    const auto model = huxerui::UseService<WebsiteTestModel>();
    const auto http = huxerui::UseService<AppHttpClient>();
    const auto tasks = huxerui::UseApplicationTaskScope();
    const auto toast = UseFluxToast();
    const auto dialog = huxerui::UseDialog();
    const auto navigation = huxerui::UseNavigation();
    const auto offset = huxerui::UseState(std::make_shared<PageScrollOffset>()).Get();
    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    const auto rows = model->sites.Get();
    const bool anyActive = model->AnyActive();
    const auto invalidUrl = huxerui::UseString(Localized("请输入不重复的 HTTP(S) 网站地址，最多添加 24 项。"));
    const WebsiteTestModel::Probe probe = [http](std::string url) { return ProbeWebsite(http, std::move(url)); };
    if (!active) return huxerui::View{};

    const auto openEditor = [model, compact, navigation, dialog, toast, invalidUrl](std::optional<std::size_t> id) {
        if (id) {
            const auto& current = model->sites.Get();
            const auto row = std::find_if(current.begin(), current.end(), [id](const auto& site) { return site.id == *id; });
            if (row == current.end()) return;
            model->customUrl = huxerui::TextEditingValue::FromText(row->url);
            model->customName = huxerui::TextEditingValue::FromText(row->name);
        } else {
            model->customUrl = huxerui::TextEditingValue{};
            model->customName = huxerui::TextEditingValue{};
        }
        const auto submit = [model, toast, invalidUrl, id] {
            const auto url = model->customUrl.Get().text;
            const auto name = model->customName.Get().text;
            if (id ? model->Update(*id, url, name) : model->Add(url, name)) return true;
            toast.Show(invalidUrl);
            return false;
        };
        if (compact) {
            navigation.Push([model, submit, id] { return WebsiteTestEditorPage(model, id.has_value(), submit); });
        } else {
            ShowEditorDialog(dialog, [model, submit, id](huxerui::DialogContext context) {
                return WebsiteTestEditorDialog(context, model, id.has_value(), submit);
            });
        }
    };
    const auto testLabel = anyActive ? Localized("取消") : Localized("全部测试");
    const auto actions = huxerui::Row {
      huxerui::IconButton(app::images::add, Localized("添加网站"))
          .With(huxerui::Tooltip(Localized("添加网站")), huxerui::Enabled(rows.size() < 24))
          .OnClick([openEditor] { openEditor(std::nullopt); }),
      huxerui::IconButton(anyActive ? app::images::close : app::images::play, testLabel)
          .With(huxerui::Tooltip(testLabel), huxerui::Enabled(anyActive || !rows.empty()))
          .OnClick([model, tasks, probe, anyActive] {
              if (anyActive) {
                  model->CancelAll();
                  return;
              }
              std::vector<std::size_t> ids;
              for (const auto& row : model->sites.Get()) ids.push_back(row.id);
              model->Start(tasks, probe, std::move(ids));
          }),
    }.With(huxerui::Spacing(8.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    std::vector<huxerui::View> cards;
    for (const auto& row : rows)
        cards.push_back(Card(WebsiteTestCardContent(row, [model, tasks, probe, id = row.id] {
            model->Toggle(tasks, probe, id);
        }, [openEditor, id = row.id] { openEditor(id); },
           [model, id = row.id] { model->Remove(id); })).Key("website-card-" + std::to_string(row.id)));
    const auto content = cards.empty()
        ? EmptyState(Localized("还没有网站，点击右上角添加。"), app::images::globe)
        : WebsiteTestGrid(cards, compact, offset);
    if (onBack) return SecondaryPageScaffold(huxerui::Text(Localized("网站测试")), actions, content,
        onBack, false, false, false, true, pageInset);
    return PageScaffold(Localized("网站测试"), actions, content, false, false, true,
        pageInset, {}, {}, true, {}, true);
}

} // namespace clashflux::ui
