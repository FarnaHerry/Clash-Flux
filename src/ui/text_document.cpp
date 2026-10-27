#include "text_document.h"

#include <atomic>
#include <utility>

#include "action_menu.h"
#include "app_resources.h"
#include "ui.h"
#include "task_bridge.h"

namespace clashflux::ui {

TextDocument::TextDocument(std::string text)
    : TextDocument(std::make_shared<const std::string>(std::move(text))) {}

TextDocument::TextDocument(std::shared_ptr<const std::string> text)
    : text_(text ? std::move(text) : std::make_shared<const std::string>()) {
    static std::atomic<std::uint64_t> next{0};
    key_ = "text-document-" + std::to_string(++next);
}

std::shared_ptr<const std::string> TextDocument::Snapshot() {
    if (dirty_ && document_) {
        text_ = std::make_shared<const std::string>(document_->getU8Text());
        dirty_ = false;
    }
    return text_;
}

void TextDocument::Prepare(huxerui::TaskScope tasks) {
    if (document_ || preparing_) return;
    preparing_ = true;
    error_ = std::string{};
    const auto self = shared_from_this();
    const auto text = text_;
    tasks.Launch([self, text]() -> huxerui::Task<void> {
        try {
            auto prepared = co_await RunOnTaskThread([text] {
                return huxerui::codeeditor::PrepareEditorDocument(*text);
            });
            self->document_ = std::move(prepared);
            self->ready_ = true;
        } catch (const std::exception& exception) { self->error_ = exception.what(); }
        self->preparing_ = false;
    });
}

[[huxerui::composable]] huxerui::View DocumentTextView(
    TextDocumentHandle document, bool readOnly, float height) {
    using namespace huxerui::codeeditor;
    const auto& theme = huxerui::UseTheme();
    const auto clipboard = huxerui::UseApplication().Clipboard();
    auto menu = UseActionMenu();
    auto searchVisible = huxerui::UseState(false);
    auto query = huxerui::UseState(huxerui::TextEditingValue{});
    auto tasks = huxerui::UseApplicationTaskScope();
    huxerui::Lifecycle([document, tasks, searchVisible, query] {
        searchVisible = false;
        query = huxerui::TextEditingValue{};
        document->Prepare(tasks);
    }, document);
    if (!document->Ready()) {
        huxerui::View pending = document->Error().empty()
            ? huxerui::View{huxerui::ProgressCircle()}
            : huxerui::View{huxerui::Text(document->Error())};
        if (height > 0.0F) return std::move(pending).With(huxerui::Frame{.height = height});
        return std::move(pending).With(huxerui::Grow(1.0F));
    }
    const auto controller = document->Controller();
    const auto showMenu = [=] {
        std::vector<ActionMenuEntry> items{
            ActionMenuItem(Localized("全选"), [controller] { controller.SelectAll(); }),
            ActionMenuItem(Localized("复制"), [controller] { controller.Copy(); }),
            ActionMenuItem(Localized("复制全部"), [document, clipboard] {
                clipboard->WriteText(*document->Snapshot());
            }),
        };
        if (!readOnly) {
            items.push_back(ActionMenuItem(Localized("剪切"), [controller] { controller.Cut(); }));
            items.push_back(ActionMenuItem(Localized("粘贴"), [controller] { controller.Paste(); }));
        }
        menu.Show(std::move(items));
    };
    EditorOptions options;
    options.initial_document = document->Prepared();
    options.document_key = document->Key();
    options.read_only = readOnly;
    options.font_size = font_size::kBody;
    options.wrap_mode = 0;
    options.scrollbar_mode = 1;
    // No full-document highlighter/diff: edits and scrolling stay local to the viewport.
    const std::weak_ptr<TextDocument> weak = document;
    auto editor = CodeEditor(std::move(options), controller)
        .On<EditorEvents::TextChanged>([weak] {
            if (const auto value = weak.lock()) value->MarkChanged();
        })
        // The app owns visibility; keyboard toggles and the external close button share it.
        .On<EditorEvents::SearchVisibilityChanged>([searchVisible, controller](bool) {
            searchVisible = !searchVisible.Get();
            if (!searchVisible.Get()) controller.ClearSearch();
        })
        .On<EditorEvents::ContextMenuRequested>([showMenu](const EditorContextMenuInfo&) { showMenu(); })
        .With(huxerui::Grow(1.0F))
        .Key("document-editor");
    huxerui::View search;
    if (searchVisible.Get()) {
        search = huxerui::Row {
          huxerui::TextField(query.Get()).Placeholder(Localized("搜索文本"))
              .OnChanged([query, controller](const huxerui::TextEditingValue& value) {
                  query = value;
                  controller.RunSearch(value.text);
              })
              .OnSubmitted([controller] { controller.FindNext(); })
              .With(huxerui::Grow(1.0F)).Key("document-search-input"),
          huxerui::Button(Localized("上一处")).OnClick([controller] { controller.FindPrevious(); }),
          huxerui::Button(Localized("下一处")).OnClick([controller] { controller.FindNext(); }),
          huxerui::IconButton(app::images::close, Localized("关闭"))
              .With(huxerui::Tooltip(Localized("关闭"))).Key("document-search-close").OnClick([=] {
              controller.ClearSearch();
              searchVisible = false;
          }),
        }.With(huxerui::Spacing(4.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    }
    huxerui::View content = huxerui::Column {
      huxerui::Row {
        huxerui::Spacer(),
        huxerui::IconButton(app::images::search, Localized("搜索文本"))
            .With(huxerui::Tooltip(Localized("搜索文本"))).Key("document-search-toggle")
            .OnClick([=] {
                searchVisible = !searchVisible.Get();
                if (!searchVisible.Get()) controller.ClearSearch();
            }),
        huxerui::IconButton(app::images::more_vertical, Localized("文本操作"))
            .With(menu.Anchor(), huxerui::Tooltip(Localized("文本操作"))).OnClick(showMenu),
      },
      search,
      editor,
    }.With(huxerui::Background(theme.colors.surface_container_low),
           huxerui::CornerRadius(theme.shapes.small), huxerui::ClipChildren(),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    if (height > 0.0F) return std::move(content).With(huxerui::Frame{.height = height});
    return std::move(content).With(huxerui::Grow(1.0F));
}

} // namespace clashflux::ui
