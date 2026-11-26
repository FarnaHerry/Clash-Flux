#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app_resources.h"
#include "card_hover_outline.h"
#include "country_flag.h"
#include "proxies_model.h"
#include "search_text.h"

namespace clashflux::ui {

struct HomeProxyChoice {
    std::string tag;
    std::string label;
    bool operator==(const HomeProxyChoice&) const = default;
};

struct HomeProxyExpansion {
    class Extension;
    struct Requested : huxerui::Event<void(bool)> {};
    bool expanded = false;
};

class HomeProxyExpansion::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode&, const HomeProxyExpansion& value) : value_(value) {}

    void Update(huxerui::ViewNode&, const HomeProxyExpansion& value) {
        if (value_.expanded != value.expanded) InvalidateSemantics();
        value_ = value;
    }

    void BuildSemantics(huxerui::SemanticBuilder& builder) const override {
        builder.SetOwner({});
        builder.AddAction(0, value_.expanded ? huxerui::SemanticActionKind::Collapse : huxerui::SemanticActionKind::Expand);
    }

    bool OnSemanticAction(std::uint64_t id, const huxerui::SemanticAction& action) override {
        if (id != 0) return false;
        const auto expected = value_.expanded ? huxerui::SemanticActionKind::Collapse : huxerui::SemanticActionKind::Expand;
        if (action.kind != expected) return false;
        EmitEvent<Requested>(!value_.expanded);
        return true;
    }

private:
    HomeProxyExpansion value_;
};

struct HomeProxyBounds {
    class Extension;
    huxerui::State<huxerui::Rect> bounds;
};

class HomeProxyBounds::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode&, const HomeProxyBounds& value) : bounds_(value.bounds) {}
    void Update(huxerui::ViewNode&, const HomeProxyBounds& value) { bounds_ = value.bounds; }

    PaintInvalidation PrepareGeometry(huxerui::ViewNode& node, huxerui::TextMeasurer&) override {
        const auto bounds = node.PresentationBounds();
        if (bounds_.Get() != bounds) bounds_ = bounds;
        return PaintInvalidation::None;
    }

private:
    huxerui::State<huxerui::Rect> bounds_;
};

struct HomeProxyPanelOrigin {
    using Value = huxerui::Point;
};

class HomeProxyPopupLayout final : public huxerui::Layout<HomeProxyPopupLayout> {
public:
    using Layout::Layout;

    static huxerui::LayoutResult Measure(huxerui::LayoutContext& context, huxerui::ViewNode& node,
                                        huxerui::Constraints constraints) {
        const huxerui::Size size{constraints.max_width, constraints.max_height};
        auto& backdrop = node.ChildAt(0);
        context.Measure(backdrop, {size.width, size.width, size.height, size.height});
        huxerui::LayoutResult result;
        result.SetSize(size).Place(backdrop, {});
        if (node.ChildCount() == 2) {
            auto& panel = node.ChildAt(1);
            context.Measure(panel, constraints.Loose());
            result.Place(panel, node.LayoutValueOr<HomeProxyPanelOrigin>({}));
        }
        return result;
    }
};

struct HomeProxyOpensUp {
    using Value = bool;
};

class HomeProxyPanelLayout final : public huxerui::Layout<HomeProxyPanelLayout> {
public:
    using Layout::Layout;

    static huxerui::LayoutResult Measure(huxerui::LayoutContext& context, huxerui::ViewNode& node,
                                        huxerui::Constraints constraints) {
        auto& input = node.ChildAt(0);
        auto& list = node.ChildAt(1);
        const float width = constraints.max_width;
        const auto inputSize = context.Measure(input, {width, width, 0.0F, constraints.max_height});
        const auto listSize = context.Measure(list,
            {width, width, 0.0F, std::max(0.0F, constraints.max_height - inputSize.height)});
        const bool opensUp = node.LayoutValueOr<HomeProxyOpensUp>(false);
        // Keep the editor first in focus order even when the list is drawn above it.
        huxerui::LayoutResult result;
        result.Place(input, {0.0F, opensUp ? listSize.height : 0.0F});
        result.Place(list, {0.0F, opensUp ? 0.0F : inputSize.height});
        return result.SetSize(constraints.Constrain({width, inputSize.height + listSize.height}));
    }
};

inline constexpr float kHomeProxyOptionHeight = 36.0F;
inline constexpr float kHomeProxyListHeight = 144.0F;
inline constexpr float kHomeProxyClosedChevronAngle = -90.0F;
inline constexpr std::string_view kHomeProxyGroupPickerId = "home-proxy-group";
inline constexpr std::string_view kHomeProxyNodePickerId = "home-proxy-node";

struct HomeProxyPicker {
    std::string_view id;
    std::vector<HomeProxyChoice> choices;
    std::string current;
    std::string label;
    huxerui::StringVariant noMatches;
    bool enabled;
    std::function<void(std::string)> select;
};

struct HomeProxyAnchor {
    huxerui::ViewHandle field;
    huxerui::State<huxerui::Rect> bounds;
    std::shared_ptr<float> chevronAngle;
};

inline HomeProxyAnchor UseHomeProxyAnchor() {
    return {huxerui::UseViewHandle(), huxerui::UseState(huxerui::Rect{}),
        huxerui::UseState(std::make_shared<float>(kHomeProxyClosedChevronAngle)).Get()};
}

struct HomeProxyChevronPresentation {
    class Extension;
    std::shared_ptr<float> angle;
    bool operator==(const HomeProxyChevronPresentation&) const = default;
};

class HomeProxyChevronPresentation::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode&, const HomeProxyChevronPresentation& value) : angle_(value.angle) {}
    void Update(huxerui::ViewNode&, const HomeProxyChevronPresentation& value) { angle_ = value.angle; }

    PaintInvalidation PrepareGeometry(huxerui::ViewNode& node, huxerui::TextMeasurer&) override {
        // Read the image's rotation in its unrotated wrapper, excluding ancestor transforms.
        const auto& image = node.ChildAt(0);
        const auto origin = node.WindowToLocal(image.LocalToWindow({0.0F, 0.0F}));
        const auto right = node.WindowToLocal(image.LocalToWindow({1.0F, 0.0F}));
        if (origin && right) {
            // All targets lie between -180 and 0 degrees, including their endpoints.
            const float radians = std::atan2(std::abs(right->y - origin->y), right->x - origin->x);
            *angle_ = -radians * (180.0F / std::numbers::pi_v<float>);
        }
        return PaintInvalidation::None;
    }

private:
    std::shared_ptr<float> angle_;
};

inline huxerui::TextField HomeProxyField(const huxerui::TextEditingValue& value,
                                       std::string_view current, const std::string& label,
                                       bool searchEnabled) {
    return huxerui::TextField(value)
        .Placeholder(current.empty() ? label : std::string{current})
        .Variant(huxerui::TextFieldVariant::Outlined)
        .InputConfiguration({.read_only = !searchEnabled});
}

inline huxerui::View HomeProxyFieldSurface(huxerui::View field, huxerui::TextFieldStyle style,
    const std::optional<ProxyNameFlag>& flag, std::shared_ptr<float> chevronAngle,
    bool enabled, bool covered, std::optional<bool> listOpensUp = {}, std::function<void()> toggle = {}) {
    huxerui::View leading;
    if (flag) {
        leading = huxerui::Row {UseCountryFlagTile(flag->index)}
            .With(huxerui::Padding(huxerui::EdgeInsets{.left = style.padding.left}));
        style.padding.left += kCountryFlagWidth + kCountryFlagTextGap;
    }
    huxerui::View trailing;
    if (!covered) {
        const float size = style.trailing_icon_size;
        const float inset = style.padding.right;
        const auto tint = !enabled ? style.disabled_trailing_icon :
            listOpensUp.has_value() ? style.focused_trailing_icon : style.trailing_icon;
        auto arrow = huxerui::Scope([chevronAngle, listOpensUp, size, inset, tint, toggle] {
            const bool reduced = huxerui::UseTheme().motion.reduced_motion;
            // The trigger and popup hand off the last painted angle for this field.
            const auto start = huxerui::UseState(*chevronAngle);
            const auto ready = huxerui::UseState(false);
            huxerui::Lifecycle([ready] {
                ready = true;
                return [] {};
            });
            const float target = !listOpensUp ? kHomeProxyClosedChevronAngle : *listOpensUp ? -180.0F : 0.0F;
            const float angle = reduced || ready.Get() ? target : start.Get();
            auto icon = huxerui::Stack {
              huxerui::Image(app::images::chevron_down).Tint(tint)
                  .With(huxerui::Frame{.width = size, .height = size},
                        huxerui::Rotation(huxerui::AnimateTo(angle, huxerui::TweenSpec{0.16, huxerui::Easing::EaseOut})))
                  .Key("home-proxy-chevron"),
            }.With(huxerui::Padding(huxerui::EdgeInsets{.right = inset}),
                   huxerui::Align(huxerui::HorizontalAlignment::End, huxerui::VerticalAlignment::Center),
                   HomeProxyChevronPresentation{chevronAngle});
            if (toggle) return huxerui::View{std::move(icon)
                .With(huxerui::Focusable(false), huxerui::Semantics{.hidden = true}).OnClick(toggle)};
            return huxerui::View{std::move(icon)};
        }).With(huxerui::Frame{.width = size + inset + style.icon_spacing,
                              .height = style.outlined.minimum_height});
        trailing = huxerui::Row {huxerui::Spacer().With(huxerui::Grow(1.0F)), arrow}
            .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    }
    style.padding.right += style.trailing_icon_size + style.icon_spacing;
    return huxerui::Stack {
      huxerui::ProvideEnvironment(style, field),
      leading,
      trailing,
    }.With(huxerui::Align(huxerui::HorizontalAlignment::Stretch, huxerui::VerticalAlignment::Center));
}

inline huxerui::View HomeProxyPanel(
    std::shared_ptr<const HomeProxyPicker> picker, HomeProxyAnchor trigger, HomeProxyAnchor other,
    bool searchEnabled, bool otherEnabled, huxerui::State<huxerui::Rect> viewportBounds,
    huxerui::State<huxerui::TextEditingValue> edit, std::function<void()> close, std::function<void()> switchPicker) {
    return huxerui::Scope([=] {
        auto active = huxerui::UseState(std::optional<std::string>{});
        const auto editor = huxerui::UseViewHandle();
        const auto scroll = huxerui::UseScrollController();
        const auto viewport = viewportBounds.Get();
        const bool viewportReady = viewport.width > 0.0F && viewport.height > 0.0F;
        huxerui::Lifecycle([editor, viewportReady] {
            if (viewportReady) editor.RequestFocus();
            return [] {};
        }, viewportReady);
        huxerui::Lifecycle([active] {
            active = std::nullopt;
            return [] {};
        }, searchEnabled);
        const auto otherBounds = other.bounds.Get();
        auto backdrop = huxerui::Row{}.On<huxerui::ViewEvents::Pointer>(
            [close, switchPicker, otherBounds, otherEnabled](const huxerui::PointerEvent& event) {
                if (event.type != huxerui::PointerEventType::Down) return;
                if (otherEnabled && event.changed_button == huxerui::PointerButton::Primary &&
                    otherBounds.Contains(event.position)) switchPicker();
                else close();
            });
        if (!viewportReady) {
            return huxerui::View{HomeProxyPopupLayout{std::move(backdrop)}.With(HomeProxyBounds{viewportBounds})};
        }

        const huxerui::TextEditingValue value = searchEnabled ? edit.Get() : huxerui::TextEditingValue{};
        const std::string_view query = value.text;
        std::vector<HomeProxyChoice> matches;
        for (const auto& choice : picker->choices) {
            if (SearchTextMatches(choice.label, query) || SearchTextMatches(choice.tag, query))
                matches.push_back(choice);
        }
        const auto anchor = trigger.bounds.Get();
        const float anchorBottom = anchor.y + anchor.height;
        const float viewportBottom = viewport.y + viewport.height;
        float above = std::max(0.0F, anchor.y - viewport.y);
        float below = std::max(0.0F, viewportBottom - anchorBottom);
        if (otherEnabled && otherBounds.height > 0.0F) {
            const float otherBottom = otherBounds.y + otherBounds.height;
            const float clearAbove = otherBottom <= anchor.y ? std::min(above, anchor.y - otherBottom) : above;
            const float clearBelow = otherBounds.y >= anchorBottom ? std::min(below, otherBounds.y - anchorBottom) : below;
            // Keep both fields reachable unless a short viewport would hide even one full option.
            if (std::max(clearAbove, clearBelow) >= kHomeProxyOptionHeight) {
                above = clearAbove;
                below = clearBelow;
            }
        }
        const float preferredHeight = std::min(kHomeProxyListHeight,
            kHomeProxyOptionHeight * static_cast<float>(std::max<std::size_t>(1, matches.size())));
        const bool opensUp = preferredHeight > below && above > below;
        const float listHeight = std::min(preferredHeight, opensUp ? above : below);
        const huxerui::Point origin{anchor.x - viewport.x,
            anchor.y - viewport.y - (opensUp ? listHeight : 0.0F)};
        const auto select = [close, picker](const std::string& tag) {
            close();
            picker->select(tag);
        };

        auto fieldStyle = huxerui::UseEnvironment<huxerui::TextFieldStyle>();
        const auto outline = fieldStyle.outlined;
        const float borderWidth = fieldStyle.focused_border_width;
        fieldStyle.outlined.background = huxerui::Color::Transparent();
        fieldStyle.outlined.disabled_background = huxerui::Color::Transparent();
        fieldStyle.outlined.corner_radii = {};
        fieldStyle.border_width = 0.0F;
        fieldStyle.focused_border_width = 0.0F;

        const bool nodePicker = picker->id == kHomeProxyNodePickerId;
        const auto flag = nodePicker ? UseProxyNameFlag(picker->current) : std::nullopt;
        auto field = HomeProxyField(value, flag ? flag->name : std::string_view{picker->current}, picker->label, searchEnabled)
            .OnChanged([edit, active, scroll, searchEnabled](const huxerui::TextEditingValue& next) {
                if (!searchEnabled) return;
                if (next.text != edit.Get().text) {
                    active = std::nullopt;
                    if (scroll.IsConnected()) static_cast<void>(scroll.ScrollTo(0.0F));
                }
                edit = next;
            })
            .OnSubmitted(close)
            .With(editor, huxerui::Frame{.height = anchor.height}, HomeProxyExpansion{true}, huxerui::Semantics{
                .role = huxerui::SemanticRole::ComboBox, .label = picker->label, .identifier = std::string{picker->id},
                .expanded = true})
            .On<HomeProxyExpansion::Requested>([close](bool show) { if (!show) close(); });
        if (!searchEnabled) field = std::move(field).OnClick(close);

        const auto listStyle = huxerui::UseEnvironment<huxerui::ComboBoxStyle>();
        std::vector<huxerui::View> rows;
        const auto renderTitle = [](std::string_view name) {
            return huxerui::Text(std::string{name}).LineLimit(1).Overflow(huxerui::TextOverflow::Ellipsis);
        };
        const auto& activeTag = active.Get();
        for (const auto& choice : matches) {
            const bool highlighted = activeTag == choice.tag;
            auto title = nodePicker ? UseProxyNameTitle(choice.label, renderTitle)
                                    : huxerui::View{renderTitle(choice.label)};
            rows.push_back(std::move(title)
                .With(huxerui::Frame{.height = kHomeProxyOptionHeight},
                      huxerui::Padding(huxerui::EdgeInsets::Symmetric(8.0F, 6.0F)),
                      huxerui::Background(highlighted ? listStyle.active_item_background : huxerui::Color::Transparent()),
                      listStyle.item_indication.value_or(huxerui::Indication{}),
                      huxerui::Focusable(false),
                      huxerui::Semantics{.role = huxerui::SemanticRole::ListItem, .label = choice.label,
                          .selected = highlighted})
                .OnClick([select, tag = choice.tag] { select(tag); })
                .Key(choice.tag));
        }
        if (rows.empty()) {
            rows.push_back(huxerui::Text(picker->noMatches)
                .LineLimit(1).Overflow(huxerui::TextOverflow::Ellipsis)
                .With(huxerui::Frame{.height = kHomeProxyOptionHeight},
                      huxerui::Padding(huxerui::EdgeInsets::Symmetric(8.0F, 6.0F))));
        }
        const auto list = huxerui::ScrollView(huxerui::Column {std::move(rows)}
            .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
            .Controller(scroll)
            .With(huxerui::Frame{.height = listHeight}, huxerui::Focusable(true),
                  huxerui::Semantics{.role = huxerui::SemanticRole::List,
                      .collection = huxerui::SemanticCollection{.item_count = matches.size()}});
        const auto& theme = huxerui::UseTheme();
        const bool reduced = theme.motion.reduced_motion;
        const auto& gradient = huxerui::UseEnvironment<FluxGradientStyle>();
        const auto suggestions = huxerui::Scope([list, reduced, opensUp] {
            auto revealed = huxerui::UseState(reduced);
            huxerui::Lifecycle([revealed] {
                revealed = true;
                return [] {};
            });
            return huxerui::Column {
              huxerui::View{list}.With(
                  huxerui::Transition(huxerui::AnimateTo(revealed.Get() ? 1.0F : 0.0F,
                      huxerui::TweenSpec{0.16, huxerui::Easing::EaseOut}))
                      .Offset({0.0F, opensUp ? 8.0F : -8.0F}, {}).Opacity(0.0F, 1.0F)),
            }.With(huxerui::ClipChildren(), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
        }).Key("home-proxy-suggestions");
        auto panel = HomeProxyPanelLayout {
          HomeProxyFieldSurface(field, fieldStyle, flag, trigger.chevronAngle, true, false, opensUp, close),
          suggestions,
        }
            .LayoutValue<HomeProxyOpensUp>(opensUp)
            .With(huxerui::Frame{.width = anchor.width},
               huxerui::Background(listStyle.popup_background),
               huxerui::CornerRadius(outline.corner_radii),
               FourColorOverlayOutline{true, gradient.enabled, theme.brightness, outline.focused_border,
                   outline.corner_radii, borderWidth, gradient.palette},
               huxerui::ClipChildren())
            .On<huxerui::ViewEvents::KeyIntercept>([edit, active, close, select, scroll,
                                                   matches = std::move(matches)](const huxerui::KeyEvent& key) {
                using huxerui::Key;
                if (key.type != huxerui::KeyEventType::Down || key.modifiers.control ||
                    key.modifiers.meta || key.modifiers.shift || edit.Get().composition) return false;
                if (key.key == Key::Escape && !key.modifiers.alt) {
                    close();
                    return true;
                }
                const auto& highlighted = active.Get();
                const auto current = highlighted ? std::ranges::find(matches, *highlighted, &HomeProxyChoice::tag)
                                                 : matches.end();
                if (key.key == Key::Enter && !key.modifiers.alt && current != matches.end()) {
                    if (!key.repeat) select(current->tag);
                    return true;
                }
                if (key.key != Key::ArrowDown && key.key != Key::ArrowUp) return false;
                if (key.modifiers.alt && key.key == Key::ArrowUp) {
                    close();
                    return true;
                }
                if (matches.empty()) return false;
                const auto index = static_cast<std::size_t>(current - matches.begin());
                const std::size_t last = matches.size() - 1;
                const std::size_t next = key.key == Key::ArrowDown
                    ? (current == matches.end() ? 0 : std::min(index + 1, last))
                    : (current == matches.end() ? last : index > 0 ? index - 1 : 0);
                active = matches[next].tag;
                if (scroll.IsConnected()) {
                    const auto metrics = scroll.Metrics();
                    const float top = kHomeProxyOptionHeight * static_cast<float>(next);
                    if (top < metrics.offset) static_cast<void>(scroll.ScrollTo(top));
                    else if (top + kHomeProxyOptionHeight > metrics.offset + metrics.viewport_extent)
                        static_cast<void>(scroll.ScrollTo(top + kHomeProxyOptionHeight - metrics.viewport_extent));
                }
                return true;
            })
            .Key("home-proxy-panel");
        return huxerui::View{HomeProxyPopupLayout{std::move(backdrop), std::move(panel)}
            .LayoutValue<HomeProxyPanelOrigin>(origin)
            .With(HomeProxyBounds{viewportBounds}, huxerui::ClipChildren())};
    }).Key("popup-" + std::string{picker->id});
}

inline huxerui::View HomeProxyCombo(
    std::shared_ptr<const HomeProxyPicker> picker, HomeProxyAnchor anchor, bool searchEnabled,
    huxerui::State<std::string> expandedPicker, huxerui::State<huxerui::Rect> viewportBounds,
    std::function<void(huxerui::TextEditingValue)> open) {
    return huxerui::Scope([=] {
        const auto viewport = viewportBounds.Get();
        const bool expanded = expandedPicker.Get() == picker->id && viewport.width > 0.0F && viewport.height > 0.0F;
        const auto flag = picker->id == kHomeProxyNodePickerId ? UseProxyNameFlag(picker->current) : std::nullopt;
        const auto displayed = flag ? flag->name : std::string_view{picker->current};
        const auto activate = [open] { open({}); };
        const auto field = HomeProxyField(picker->enabled ? huxerui::TextEditingValue{}
                                              : huxerui::TextEditingValue::FromText(std::string{displayed}),
                                          displayed, picker->label, searchEnabled)
            .OnChanged([open, searchEnabled](const huxerui::TextEditingValue& next) {
                if (searchEnabled && (!next.text.empty() || next.composition)) open(next);
            })
            .With(anchor.field, HomeProxyExpansion{false}, HomeProxyBounds{anchor.bounds},
                  huxerui::Semantics{.role = huxerui::SemanticRole::ComboBox, .label = picker->label,
                      .identifier = std::string{picker->id}, .expanded = false, .hidden = expanded})
            .On<HomeProxyExpansion::Requested>([activate](bool show) { if (show) activate(); })
            .OnClick(activate);
        const auto fieldStyle = huxerui::UseEnvironment<huxerui::TextFieldStyle>();
        return huxerui::Column {
          HomeProxyFieldSurface(field, fieldStyle, flag, anchor.chevronAngle, picker->enabled, expanded)
              .With(huxerui::Opacity(expanded ? 0.0F : 1.0F)),
        }
            .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch), huxerui::Enabled(picker->enabled))
            .On<huxerui::ViewEvents::KeyIntercept>([activate](const huxerui::KeyEvent& key) {
                using huxerui::Key;
                if (key.type != huxerui::KeyEventType::Down || key.modifiers.control ||
                    key.modifiers.meta || key.modifiers.shift ||
                    (key.key != Key::ArrowDown && (key.key != Key::ArrowUp || key.modifiers.alt))) return false;
                activate();
                return true;
            })
            .Key(std::string{picker->id});
    });
}

inline huxerui::View HomeProxyPickerControls(HomeProxyPicker group, HomeProxyPicker node, std::string groupName,
    bool searchEnabled, std::uint64_t revision, huxerui::State<std::string> expandedPicker) {
    return huxerui::Scope([group = std::move(group), node = std::move(node), groupName = std::move(groupName),
                          searchEnabled, revision, expandedPicker] {
        const auto groupAnchor = UseHomeProxyAnchor();
        const auto nodeAnchor = UseHomeProxyAnchor();
        const auto popup = huxerui::UsePopup();
        const auto layer = huxerui::UseState(huxerui::LayerId{});
        const auto viewport = huxerui::UseState(huxerui::Rect{});
        const auto edit = huxerui::UseState(huxerui::TextEditingValue{});
        const auto closedPicker = huxerui::UseState(std::string{});
        const auto openedRevision = huxerui::UseState(revision);
        const auto openedGroup = huxerui::UseState(groupName);
        const auto close = [=] {
            const auto id = layer.Get();
            if (id == 0) return;
            closedPicker = expandedPicker.Get();
            layer = huxerui::LayerId{};
            expandedPicker = std::string{};
            edit = huxerui::TextEditingValue{};
            viewport = huxerui::Rect{};
            popup.Dismiss(id);
        };
        const auto groupPicker = std::make_shared<const HomeProxyPicker>(group);
        const auto nodePicker = std::make_shared<const HomeProxyPicker>(node);
        const auto activate = [edit, expandedPicker, closedPicker, openedRevision, openedGroup, revision, groupName](
            std::string_view id, huxerui::TextEditingValue value = {}) {
            edit = std::move(value);
            openedRevision = revision;
            openedGroup = groupName;
            closedPicker = std::string{};
            expandedPicker = std::string{id};
        };
        const huxerui::ViewFactory panel = [=] {
            return huxerui::Scope([=] {
                const bool groupActive = expandedPicker.Get() == kHomeProxyGroupPickerId;
                const auto& current = groupActive ? groupPicker : nodePicker;
                const auto& other = groupActive ? nodePicker : groupPicker;
                return HomeProxyPanel(current, groupActive ? groupAnchor : nodeAnchor,
                    groupActive ? nodeAnchor : groupAnchor, searchEnabled, other->enabled, viewport, edit, close,
                    [activate, id = other->id] { activate(id); });
            });
        };
        const auto open = [=](const HomeProxyPicker& picker, const HomeProxyAnchor& trigger,
                              huxerui::TextEditingValue value) {
            if (!picker.enabled || layer.Get() != 0 || trigger.bounds.Get().width <= 0.0F) return;
            trigger.field.RequestFocus();
            // The popup occupies the safe viewport and positions the combined surface at the active field.
            const auto id = popup.ShowAt({}, panel, {
                .gap = 0.0F, .viewport_margin = 0.0F, .trap_focus = true, .on_dismiss_request = close});
            if (id != 0) {
                layer = id;
                activate(picker.id, std::move(value));
            }
        };
        huxerui::Lifecycle([close] { return close; });
        const auto restoreFocus = closedPicker.Get();
        huxerui::Lifecycle([=] {
            if (!restoreFocus.empty() && layer.Get() == 0) {
                const auto& trigger = restoreFocus == kHomeProxyGroupPickerId ? groupAnchor : nodeAnchor;
                trigger.field.RequestFocus();
            }
            return [] {};
        }, restoreFocus);
        huxerui::Lifecycle([=] {
            if (!searchEnabled) edit = huxerui::TextEditingValue{};
            if (const auto id = layer.Get(); id != 0) {
                const bool groupActive = expandedPicker.Get() == kHomeProxyGroupPickerId;
                const bool available = groupActive ? groupPicker->enabled
                    : nodePicker->enabled && openedGroup.Get() == groupName;
                if (!available || openedRevision.Get() != revision || !popup.Update(id, panel)) close();
            }
            return [] {};
        }, searchEnabled, revision, groupName, group.enabled, node.enabled,
            group.choices, group.current, group.label, node.choices, node.current, node.label);

        return huxerui::Column {
          HomeProxyCombo(groupPicker, groupAnchor, searchEnabled, expandedPicker, viewport,
              [=](huxerui::TextEditingValue value) { open(*groupPicker, groupAnchor, std::move(value)); }),
          HomeProxyCombo(nodePicker, nodeAnchor, searchEnabled, expandedPicker, viewport,
              [=](huxerui::TextEditingValue value) { open(*nodePicker, nodeAnchor, std::move(value)); }),
        }.With(huxerui::Spacing(5.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    });
}

inline huxerui::View HomeProxyPickers(
    huxerui::State<ProxiesSnapshot> snapshotState, std::uint64_t revision,
    bool hasProfile, bool globalMode, bool enabled, huxerui::State<std::string> expandedPicker,
    std::function<void(std::string, std::string, std::uint64_t)> onSelect) {
    return huxerui::Scope([snapshotState, revision, hasProfile, globalMode, enabled,
                          expandedPicker, onSelect = std::move(onSelect)] {
        constexpr float fieldHeight = 40.0F;
        auto groupTag = huxerui::UseState(std::string{});
        const bool searchEnabled = huxerui::UseViewportClass() != huxerui::ViewportClass::Compact;
        const auto& snapshot = snapshotState.Get();
        static const std::vector<ProxyGroupSnapshot> emptyGroups;
        const bool ready = hasProfile && snapshot.loaded && snapshot.profilesRevision == revision;
        const auto& groups = ready ? snapshot.groups : emptyGroups;
        const auto found = std::ranges::find(groups, groupTag.Get(), &ProxyGroupSnapshot::name);
        const auto* group = found != groups.end() ? &*found : DefaultProxyGroup(groups, globalMode);
        std::vector<HomeProxyChoice> groupChoices;
        for (const auto& option : groups) {
            groupChoices.push_back({option.name, option.displayName.empty() ? option.name : option.displayName});
        }
        const auto nodeLabel = [group](const std::string& tag) {
            const auto label = group->nodeLabels.find(tag);
            return label != group->nodeLabels.end() ? label->second : tag;
        };
        std::vector<HomeProxyChoice> nodeChoices;
        if (group && group->selectable) {
            for (const auto& tag : group->nodes) nodeChoices.push_back({tag, nodeLabel(tag)});
        }

        auto fields = huxerui::UseEnvironment<huxerui::TextFieldStyle>();
        fields.outlined.minimum_height = fieldHeight;
        fields.padding = huxerui::EdgeInsets::Symmetric(8.0F, 0.0F);
        std::string groupName = group ? group->name : "";
        return huxerui::ProvideEnvironment(fields, HomeProxyPickerControls(
            {kHomeProxyGroupPickerId, std::move(groupChoices),
             group ? (group->displayName.empty() ? group->name : group->displayName) : "",
             huxerui::UseString(searchEnabled ? Localized("搜索代理组") : Localized("策略组")),
             Localized("没有匹配的代理组"), enabled,
             [groupTag](std::string tag) { groupTag = std::move(tag); }},
            {kHomeProxyNodePickerId, std::move(nodeChoices), group ? nodeLabel(group->current) : "",
             huxerui::UseString(searchEnabled ? Localized("搜索节点") : Localized("节点")),
             Localized("没有匹配的节点"), enabled && (!group || group->selectable),
             [onSelect, groupName, revision](std::string tag) { onSelect(groupName, std::move(tag), revision); }},
            groupName, searchEnabled, revision, expandedPicker));
    });
}

} // namespace clashflux::ui
