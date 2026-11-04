#include <huxerui/huxerui.h>

#include "ui.h"
#include "page_layout.h"

namespace clashflux::ui {

namespace {
struct EditorDismissBinding {
    std::shared_ptr<std::function<void()>> request;
    static EditorDismissBinding Default() { return {}; }
    bool operator==(const EditorDismissBinding&) const = default;
};
}

[[huxerui::composable]] huxerui::View DialogCard(huxerui::View content) {
    const auto& theme = huxerui::UseTheme();
    const auto islands = ResolveIslandTheme(theme);
    // Environment is a transparent scope; the surface owns all view behavior.
    return huxerui::Column {huxerui::ProvideEnvironment(DialogSurfaceContext{true}, content)}.With(
        kPageTitlesInWindow ? ResolveFluxShadows(theme).dialog
            : huxerui::Shadow{ThemeShadowColor(theme, 0.30F), {}, 28.0F, 2.0F},
        huxerui::Background(islands.overlay),
        huxerui::CornerRadius(islands.island_radius),
        huxerui::Border(islands.outline_soft, kCardOutlineWidth),
        huxerui::ClipChildren(),
        huxerui::Padding(islands.island_padding),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}


[[huxerui::composable]] huxerui::View DialogHeader(huxerui::StringVariant title,
    huxerui::View actions, huxerui::View leading) {
    return DialogHeaderContent(huxerui::Text(title, huxerui::TextRole::Title), actions, leading);
}

[[huxerui::composable]] huxerui::View DialogHeaderContent(huxerui::View title,
    huxerui::View actions, huxerui::View leading) {
    return huxerui::Row {
      leading,
      huxerui::View{title}.With(huxerui::Grow(1.0F)),
      actions,

    }.With(huxerui::Frame{.min_height = 40.0F}, huxerui::Spacing(8.0F),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)).Key("dialog-header");
}

[[huxerui::composable]] huxerui::View DialogAction(huxerui::StringVariant label,
    std::function<void()> action, bool primary, bool enabled, bool danger) {
    const auto& theme = huxerui::UseTheme();
    auto style = ResolveFluxButtonStyle(theme);
    style.minimum_height = 40.0F;
    if (!primary) {
        style.background = huxerui::Color::Transparent();
        style.label_style.foreground = theme.colors.primary;
        style.disabled_background = huxerui::Color::Transparent();
        style.indication = ResolveFluxIndication(theme);
    }
    if (danger) {
        style.background = theme.colors.error;
        style.label_style.foreground = OnErrorColor(theme);
        style.indication = ResolveFluxIndication(theme, OnErrorColor(theme));
    }
    const bool outlined = !primary && !danger;
    const auto outline = enabled ? theme.colors.outline : FluxStateLayerColor(theme.colors.outline, 0.38F);
    return huxerui::ProvideEnvironment(style,
        huxerui::Button(label).OnClick(action).With(huxerui::Enabled(enabled),
            huxerui::CornerRadius(style.corner_radii),
            huxerui::Border(outlined ? outline : huxerui::Color::Transparent(), outlined ? 1.0F : 0.0F)));
}

huxerui::View DialogActions(std::vector<huxerui::View> actions) {
    return huxerui::Flow(actions).With(huxerui::Spacing(8.0F),
        huxerui::MainAlign(huxerui::MainAxisAlignment::End),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)).Key("dialog-actions");
}

huxerui::View DialogScaffold(huxerui::View header, huxerui::View body,
    huxerui::View actions, DialogSize size, bool fillHeight) {
    const float width = size == DialogSize::Small ? 360.0F : size == DialogSize::Form ? 520.0F : 720.0F;
    huxerui::Frame frame{.width = width, .max_height = 620.0F};
    if (fillHeight) frame.height = 620.0F;
    return DialogCard(huxerui::Column {
      header,
      body,
      actions,
    }.With(frame, huxerui::Spacing(16.0F),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)).Key("dialog-scaffold"));
}

void ConfirmDiscardChanges(huxerui::DialogHandle dialog, std::function<void()> discard) {
    dialog.Show([discard](huxerui::DialogContext context) {
        return DialogScaffold(DialogHeader(Localized("放弃未保存的修改？")),
            huxerui::Text(Localized("本次修改尚未保存，离开后将丢失。")),
            DialogActions({
              DialogAction(Localized("继续编辑"), [context] { context.Dismiss(); }, false).Key("discard-keep"),
              DialogAction(Localized("放弃修改"), [context, discard] { context.Dismiss(); discard(); }, true, true, true)
                  .Key("discard-confirm"),
            }), DialogSize::Small);
    }, {.dismiss_on_outside_press = false});
}

void ShowEditorDialog(huxerui::DialogHandle dialog, huxerui::DialogFactory content) {
    auto request = std::make_shared<std::function<void()>>();
    dialog.Show([content, request](huxerui::DialogContext context) {
        return huxerui::ProvideEnvironment(EditorDismissBinding{request}, content(context));
    }, {.on_dismiss_request = [request] { if (*request) (*request)(); }});
}

// This is a hook in its composable caller's lifetime, not another View boundary.
std::function<void()> UseEditorClose(std::function<bool()> changed,
    std::function<bool()> busy, std::function<void()> close) {
    const auto dialog = huxerui::UseDialog();
    const auto request = huxerui::UseEnvironment<EditorDismissBinding>().request;
    const auto alive = huxerui::UseState(true);
    huxerui::Lifecycle([alive] { return [alive] { alive = false; }; }, 0);
    const auto guarded = [=] {
        if (!alive.Get() || busy()) return;
        if (!changed()) { close(); return; }
        ConfirmDiscardChanges(dialog, [alive, close] { if (alive.Get()) close(); });
    };
    // Publish the latest baseline/callback without putting mutable view behavior on an Environment node.
    huxerui::Lifecycle([request, guarded] {
        if (request) *request = guarded;
        return [request] { if (request) *request = {}; };
    });
    return guarded;
}

} // namespace clashflux::ui
