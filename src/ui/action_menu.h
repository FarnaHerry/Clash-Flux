#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "ui.h"

namespace clashflux::ui {

// 在组合期使用 View/Surface；事件回调只构造拥有型菜单描述并调用 handle。
inline huxerui::View ActionMenuItemView(huxerui::StringVariant label,
    std::function<void()> action, std::string suffix = {},
    std::optional<bool> checked = {}, bool enabled = true, bool danger = false,
    std::optional<huxerui::ImageVariant> icon = {}) {
    const auto style = huxerui::UseEnvironment<huxerui::MenuStyle>();
    const auto foreground = danger ? huxerui::UseTheme().colors.error : style.foreground;
    huxerui::Semantics semantics{.role = huxerui::SemanticRole::MenuItem, .label = label,
        .descendants = huxerui::SemanticDescendantPolicy::Exclude};
    if (checked) semantics.checked = *checked ? huxerui::SemanticCheckedState::Checked
                                             : huxerui::SemanticCheckedState::Unchecked;
    std::vector<huxerui::View> content;
    if (icon) content.push_back(huxerui::Image(*icon).Fit(huxerui::ImageFit::Contain)
        .Tint(danger ? foreground : style.icon_tint)
        .With(huxerui::Frame{.width = style.icon_size, .height = style.icon_size}));
    content.push_back(huxerui::Text(label).Style({huxerui::Font::System(font_size::kBody), foreground})
            .With(huxerui::Grow(1.0F)));
    content.push_back(huxerui::Text(std::move(suffix)).Style({huxerui::Font::System(font_size::kBody), foreground}));
    return huxerui::Row(std::move(content)).With(huxerui::Frame{.min_height = style.minimum_item_height},
           huxerui::Padding(style.item_padding), huxerui::Spacing(style.item_content_spacing),
           huxerui::CornerRadius(style.corner_radii), style.item_indication,
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
           std::move(semantics), huxerui::Focusable(enabled), huxerui::Enabled(enabled))
        .OnClick(std::move(action));
}

inline huxerui::View ActionMenuSurface(std::vector<huxerui::View> items) {
    const auto style = huxerui::UseEnvironment<huxerui::MenuStyle>();
    const float breathing = huxerui::UseTheme().spacing.extra_small;
    auto padding = style.content_padding;
    padding.top += breathing;
    padding.right += breathing;
    padding.bottom += breathing;
    padding.left += breathing;
    return huxerui::ScrollView(huxerui::Column(std::move(items)).With(
        huxerui::Spacing(breathing * 0.5F),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))).With(
        huxerui::Frame{.width = style.minimum_width}, huxerui::Padding(padding),
        huxerui::Semantics{.role = huxerui::SemanticRole::Menu},
        huxerui::Background(style.background), huxerui::CornerRadius(style.corner_radii),
        // 圆角背景本身不会裁剪子项：同时约束 hover 绘制和子项命中范围。
        style.shadow, huxerui::ClipChildren(),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)).Key("action-menu-surface");
}

struct ActionMenuItem {
    huxerui::StringVariant label;
    std::function<void()> action;
    std::optional<huxerui::ImageVariant> icon;
    std::optional<bool> checked;
    bool enabled = true;
    bool danger = false;

    ActionMenuItem(huxerui::StringVariant text, std::function<void()> callback)
        : label(std::move(text)), action(std::move(callback)) {}
    ActionMenuItem(huxerui::ImageVariant image, huxerui::StringVariant text, std::function<void()> callback)
        : label(std::move(text)), action(std::move(callback)), icon(std::move(image)) {}
    ActionMenuItem Enabled(bool value) && { enabled = value; return std::move(*this); }
    ActionMenuItem Checked(bool value) && { checked = value; return std::move(*this); }
    ActionMenuItem Danger(bool value = true) && { danger = value; return std::move(*this); }
};
struct ActionMenuSection {};
using ActionMenuEntry = std::variant<ActionMenuItem, ActionMenuSection>;

// 同一视觉组件也可由多级菜单自行管理层级，如日志菜单的 hover 子面板。
inline huxerui::View ActionMenuPanel(huxerui::PopupContext context, const std::vector<ActionMenuEntry>& entries) {
    const auto style = huxerui::UseEnvironment<huxerui::MenuStyle>();
    std::vector<huxerui::View> items;
    for (const auto& entry : entries) {
        if (const auto* item = std::get_if<ActionMenuItem>(&entry)) {
            items.push_back(ActionMenuItemView(item->label, [context, action = item->action] {
                context.Dismiss();
                if (action) action();
            }, item->checked.value_or(false) ? "✓" : "", item->checked,
               item->enabled, item->danger, item->icon));
        } else {
            items.push_back(huxerui::Row{}.With(huxerui::Frame{.height = style.separator_thickness},
                huxerui::Background(style.separator_color)));
        }
    }
    return ActionMenuSurface(std::move(items));
}

class ActionMenuHandle {
public:
    explicit ActionMenuHandle(huxerui::PopupHandle popup) : popup_(std::move(popup)) {}
    huxerui::LayerAnchor Anchor() const { return popup_.Anchor(); }
    huxerui::LayerId Show(std::vector<ActionMenuEntry> entries) const {
        return popup_.Show(Factory(std::move(entries)));
    }
    huxerui::LayerId ShowAt(huxerui::Point point, std::vector<ActionMenuEntry> entries) const {
        return popup_.ShowAt(point, Factory(std::move(entries)));
    }
private:
    static huxerui::PopupFactory Factory(std::vector<ActionMenuEntry> entries) {
        return [entries = std::move(entries)](huxerui::PopupContext context) {
            return huxerui::Scope([context, entries] { return ActionMenuPanel(context, entries); });
        };
    }
    huxerui::PopupHandle popup_;
};
inline ActionMenuHandle UseActionMenu() { return ActionMenuHandle{huxerui::UsePopup()}; }

} // namespace clashflux::ui
