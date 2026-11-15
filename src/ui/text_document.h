#pragma once

#include <memory>
#include <optional>
#include <string>

#include <huxerui/codeeditor.h>
#include <huxerui/huxerui.h>

namespace clashflux::ui {

// UI-thread session. UTF-8 line storage is prepared once on a worker before mount.
// Live edits survive unmount; full snapshots are materialized only on explicit use.
class TextDocument : public std::enable_shared_from_this<TextDocument> {
public:
    explicit TextDocument(std::string text = {});
    explicit TextDocument(std::shared_ptr<const std::string> text);

    [[nodiscard]] const std::shared_ptr<const std::string>& InitialText() const { return text_; }
    [[nodiscard]] const std::string& Key() const { return key_; }
    [[nodiscard]] const huxerui::codeeditor::EditorController& Controller() const { return controller_; }
    [[nodiscard]] const huxerui::codeeditor::Document& Prepared() const { return document_.value(); }
    [[nodiscard]] bool Ready() const { return ready_.Get(); }
    [[nodiscard]] const std::string& Error() const { return error_.Get(); }
    void BindController(huxerui::codeeditor::EditorController controller) { controller_ = std::move(controller); }
    void Prepare(huxerui::TaskScope tasks);
    [[nodiscard]] std::shared_ptr<const std::string> Snapshot();
    void MarkChanged() { dirty_ = true; }

private:
    std::shared_ptr<const std::string> text_;
    std::string key_;
    huxerui::codeeditor::EditorController controller_;
    std::optional<huxerui::codeeditor::Document> document_;
    huxerui::State<bool> ready_{false};
    huxerui::State<std::string> error_{std::string{}};
    bool preparing_ = false;
    bool dirty_ = false;
};

using TextDocumentHandle = std::shared_ptr<TextDocument>;

// Bounded viewport; CodeEditor owns scrolling, selection, IME and undo history.
huxerui::View DocumentTextView(TextDocumentHandle document, bool readOnly = false, float height = 320.0F);

} // namespace clashflux::ui
