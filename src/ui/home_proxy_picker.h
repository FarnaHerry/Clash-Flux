#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app_resources.h"
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
inline constexpr std::string_view kHomeProxyNodePickerId = "home-proxy-node";

inline huxerui::TextField HomeProxyField(const huxerui::TextEditingValue& value,
                                       std::string_view current, const std::string& label,
                                       bool searchEnabled) {
    return huxerui::TextField(value)
        .Placeholder(current.empty() ? label : std::string{current})
        .Variant(huxerui::TextFieldVariant::Outlined)
        .InputConfiguration({.read_only = !searchEnabled})
        .TrailingIcon(app::images::chevron_down);
}

inline huxerui::View HomeProxyFieldSurface(huxerui::View field, huxerui::TextFieldStyle style,
                                         const std::optional<ProxyNameFlag>& flag) {
    huxerui::View leading;
    if (flag) {
        leading = huxerui::Row {UseCountryFlagTile(flag->index)}
            .With(huxerui::Padding(huxerui::EdgeInsets{.left = style.padding.left}));
        style.padding.left += kCountryFlagWidth + kCountryFlagTextGap;
    }
    return huxerui::Stack {
      huxerui::ProvideEnvironment(style, field),
      leading,
    }.With(huxerui::Align(huxerui::HorizontalAlignment::Stretch, huxerui::VerticalAlignment::Center));
}

inline huxerui::View HomeProxyPanel(
    std::vector<HomeProxyChoice> choices, std::string current, std::string label,
    huxerui::StringVariant noMatches, std::string id, bool searchEnabled, huxerui::State<huxerui::Rect> anchorBounds,
    huxerui::State<huxerui::Rect> viewportBounds,
    huxerui::State<huxerui::TextEditingValue> edit, std::function<void()> close,
    std::function<void(std::string)> onSelect) {
    return huxerui::Scope([choices = std::move(choices), current = std::move(current), label = std::move(label),
                          noMatches, id = std::move(id), searchEnabled, anchorBounds, viewportBounds, edit,
                          close = std::move(close), onSelect = std::move(onSelect)] {
        auto active = huxerui::UseState(std::optional<std::size_t>{});
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
        auto backdrop = huxerui::Row{}.On<huxerui::ViewEvents::Pointer>([close](const huxerui::PointerEvent& event) {
            if (event.type == huxerui::PointerEventType::Down) close();
        });
        if (!viewportReady) {
            return huxerui::View{HomeProxyPopupLayout{std::move(backdrop)}.With(HomeProxyBounds{viewportBounds})};
        }

        const huxerui::TextEditingValue value = searchEnabled ? edit.Get() : huxerui::TextEditingValue{};
        const std::string_view query = value.text;
        std::vector<HomeProxyChoice> matches;
        for (const auto& choice : choices) {
            if (SearchTextMatches(choice.label, query) || SearchTextMatches(choice.tag, query))
                matches.push_back(choice);
        }
        const auto anchor = anchorBounds.Get();
        const float above = std::max(0.0F, anchor.y - viewport.y);
        const float below = std::max(0.0F, viewport.y + viewport.height - anchor.y - anchor.height);
        const float preferredHeight = std::min(kHomeProxyListHeight,
            kHomeProxyOptionHeight * static_cast<float>(std::max<std::size_t>(1, matches.size())));
        const bool opensUp = preferredHeight > below && above > below;
        const float listHeight = std::min(preferredHeight, opensUp ? above : below);
        const huxerui::Point origin{anchor.x - viewport.x,
            anchor.y - viewport.y - (opensUp ? listHeight : 0.0F)};
        const auto select = [close, onSelect](const std::string& tag) {
            close();
            onSelect(tag);
        };

        auto fieldStyle = huxerui::UseEnvironment<huxerui::TextFieldStyle>();
        const auto outline = fieldStyle.outlined;
        const float borderWidth = fieldStyle.focused_border_width;
        fieldStyle.outlined.background = huxerui::Color::Transparent();
        fieldStyle.outlined.disabled_background = huxerui::Color::Transparent();
        fieldStyle.outlined.corner_radii = {};
        fieldStyle.border_width = 0.0F;
        fieldStyle.focused_border_width = 0.0F;

        const bool nodePicker = id == kHomeProxyNodePickerId;
        const auto flag = nodePicker ? UseProxyNameFlag(current) : std::nullopt;
        const auto field = HomeProxyField(value, flag ? flag->name : std::string_view{current}, label, searchEnabled)
            .OnChanged([edit, active, searchEnabled](const huxerui::TextEditingValue& next) {
                if (!searchEnabled) return;
                if (next.text != edit.Get().text) active = std::nullopt;
                edit = next;
            })
            .OnSubmitted(close)
            .With(editor, huxerui::Frame{.height = anchor.height}, HomeProxyExpansion{true}, huxerui::Semantics{
                .role = huxerui::SemanticRole::ComboBox, .label = label, .identifier = id,
                .expanded = true})
            .On<HomeProxyExpansion::Requested>([close](bool show) { if (!show) close(); })
            .OnClick(close);

        const auto listStyle = huxerui::UseEnvironment<huxerui::ComboBoxStyle>();
        std::vector<huxerui::View> rows;
        const auto renderTitle = [](std::string_view name) {
            return huxerui::Text(std::string{name}).LineLimit(1).Overflow(huxerui::TextOverflow::Ellipsis);
        };
        for (std::size_t index = 0; index < matches.size(); ++index) {
            const auto& choice = matches[index];
            const bool highlighted = active.Get() == index;
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
            rows.push_back(huxerui::Text(noMatches)
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
        const bool reduced = huxerui::UseTheme().motion.reduced_motion;
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
        auto panel = HomeProxyPanelLayout {HomeProxyFieldSurface(field, fieldStyle, flag), suggestions}
            .LayoutValue<HomeProxyOpensUp>(opensUp)
            .With(huxerui::Frame{.width = anchor.width},
               huxerui::Background(listStyle.popup_background),
               huxerui::CornerRadius(outline.corner_radii), huxerui::Border(outline.focused_border, borderWidth),
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
                if (key.key == Key::Enter && !key.modifiers.alt && active.Get() && *active.Get() < matches.size()) {
                    if (!key.repeat) select(matches[*active.Get()].tag);
                    return true;
                }
                if (key.key != Key::ArrowDown && key.key != Key::ArrowUp) return false;
                if (key.modifiers.alt && key.key == Key::ArrowUp) {
                    close();
                    return true;
                }
                if (matches.empty()) return false;
                const auto index = active.Get();
                const std::size_t last = matches.size() - 1;
                const std::size_t next = key.key == Key::ArrowDown
                    ? (index ? std::min(*index + 1, last) : 0)
                    : (index && *index > 0 ? std::min(*index - 1, last) : index ? 0 : last);
                active = next;
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
    });
}

inline huxerui::View HomeProxyCombo(
    std::vector<HomeProxyChoice> choices, std::string current,
    huxerui::StringVariant fieldLabel, huxerui::StringVariant noMatches,
    std::string id, bool enabled, bool searchEnabled, huxerui::State<std::string> expandedPicker,
    std::uint64_t revision, std::function<void(std::string)> onSelect) {
    return huxerui::Scope([choices = std::move(choices), current = std::move(current),
                          fieldLabel, noMatches, id, enabled, searchEnabled, expandedPicker, revision,
                          onSelect = std::move(onSelect)] {
        const auto anchor = huxerui::UseViewHandle();
        const auto popup = huxerui::UsePopup();
        auto anchorBounds = huxerui::UseState(huxerui::Rect{});
        auto viewportBounds = huxerui::UseState(huxerui::Rect{});
        auto layer = huxerui::UseState(huxerui::LayerId{});
        auto edit = huxerui::UseState(huxerui::TextEditingValue{});
        const auto close = [popup, layer, edit, expandedPicker, id, viewportBounds] {
            if (layer.Get() == 0) return;
            const auto previous = layer.Get();
            layer = huxerui::LayerId{};
            popup.Dismiss(previous);
            if (expandedPicker.Get() == id) expandedPicker = std::string{};
            edit = huxerui::TextEditingValue{};
            viewportBounds = huxerui::Rect{};
        };
        const std::string label = huxerui::UseString(fieldLabel);
        const huxerui::ViewFactory panel = [choices, current, label, noMatches, id, searchEnabled, anchorBounds,
                                           viewportBounds, edit, close, onSelect] {
            return HomeProxyPanel(choices, current, label, noMatches, id, searchEnabled, anchorBounds, viewportBounds,
                                  edit, close, onSelect);
        };
        const auto open = [anchor, popup, panel, layer, anchorBounds, enabled, expandedPicker, id, close] {
            if (!enabled || layer.Get() != 0 || anchorBounds.Get().width <= 0.0F) return;
            anchor.RequestFocus();
            // The popup occupies the safe viewport and positions the combined
            // surface against the editor's current window bounds.
            layer = popup.ShowAt({}, panel, {
                .gap = 0.0F, .viewport_margin = 0.0F, .trap_focus = true, .on_dismiss_request = close});
            expandedPicker = id;
        };
        huxerui::Lifecycle([close] { return close; });
        huxerui::Lifecycle([enabled, searchEnabled, edit, popup, layer, panel, close] {
            if (!searchEnabled) edit = huxerui::TextEditingValue{};
            if (layer.Get() != 0 && (!enabled || !popup.Update(layer.Get(), panel))) close();
            return [] {};
        }, enabled, searchEnabled, choices, current, label, revision);

        const bool expanded = layer.Get() != 0 && viewportBounds.Get().height > 0.0F;
        const auto flag = id == kHomeProxyNodePickerId ? UseProxyNameFlag(current) : std::nullopt;
        const auto displayed = flag ? flag->name : std::string_view{current};
        const auto field = HomeProxyField(enabled ? huxerui::TextEditingValue{}
                                              : huxerui::TextEditingValue::FromText(std::string{displayed}),
                                          displayed, label, searchEnabled)
            .OnChanged([edit, open, searchEnabled](const huxerui::TextEditingValue& next) {
                if (searchEnabled && (!next.text.empty() || next.composition)) {
                    edit = next;
                    open();
                }
            })
            .With(anchor, HomeProxyExpansion{false}, HomeProxyBounds{anchorBounds},
                  huxerui::Semantics{.role = huxerui::SemanticRole::ComboBox, .label = label,
                      .identifier = id, .expanded = false, .hidden = expanded})
            .On<HomeProxyExpansion::Requested>([open](bool show) { if (show) open(); })
            .OnClick(open);
        const auto fieldStyle = huxerui::UseEnvironment<huxerui::TextFieldStyle>();
        return huxerui::Column {
          HomeProxyFieldSurface(field, fieldStyle, flag).With(huxerui::Opacity(expanded ? 0.0F : 1.0F)),
        }
            .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch), huxerui::Enabled(enabled))
            .On<huxerui::ViewEvents::KeyIntercept>([open](const huxerui::KeyEvent& key) {
                using huxerui::Key;
                if (key.type != huxerui::KeyEventType::Down || key.modifiers.control ||
                    key.modifiers.meta || key.modifiers.shift ||
                    (key.key != Key::ArrowDown && (key.key != Key::ArrowUp || key.modifiers.alt))) return false;
                open();
                return true;
            })
            .Key(id);
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
        return huxerui::ProvideEnvironment(fields, huxerui::Column {
          HomeProxyCombo(std::move(groupChoices),
                         group ? (group->displayName.empty() ? group->name : group->displayName) : "",
                         searchEnabled ? Localized("搜索代理组") : Localized("策略组"),
                         Localized("没有匹配的代理组"), "home-proxy-group", enabled, searchEnabled,
                         expandedPicker, revision, [groupTag](std::string tag) { groupTag = std::move(tag); }),
          HomeProxyCombo(std::move(nodeChoices), group ? nodeLabel(group->current) : "",
                         searchEnabled ? Localized("搜索节点") : Localized("节点"),
                         Localized("没有匹配的节点"), std::string{kHomeProxyNodePickerId},
                         enabled && (!group || group->selectable), searchEnabled, expandedPicker, revision,
                         [onSelect, name = group ? group->name : "", revision](std::string tag) {
                             onSelect(name, std::move(tag), revision);
                         }).Key(group ? group->name : ""),
        }.With(huxerui::Spacing(5.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
    });
}

} // namespace clashflux::ui
