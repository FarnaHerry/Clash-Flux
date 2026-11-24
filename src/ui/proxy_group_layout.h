#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "app_resources.h"
#include "page_layout.h"

namespace clashflux::ui {

inline constexpr float kProxyGroupInset = 12.0F;
inline constexpr float kProxyGroupGap = 12.0F;
inline constexpr float kProxyNodeWidth = 196.0F;

inline huxerui::View ProxyGroupHeader(std::string title, std::string type, std::size_t nodeCount,
    bool degraded, bool expanded, std::string selectedNode, huxerui::View speed, std::function<void()> toggle) {
    return huxerui::Scope([=] {
        const auto& theme = huxerui::UseTheme();
        const auto islands = ResolveIslandTheme(theme);
        const huxerui::TextStyle caption{
            huxerui::Font::System(font_size::kCaption), theme.colors.on_surface_variant};
        const auto heading = huxerui::Text(title).LineLimit(1).Overflow(huxerui::TextOverflow::Ellipsis)
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kBody)
                .WithWeight(huxerui::FontWeight::Medium), theme.colors.on_surface})
            .With(huxerui::LayoutPriority(1.0F)).Key("proxy-group-title");
        const auto count = huxerui::Stack {
          huxerui::Text(std::to_string(nodeCount)).LineLimit(1).Style(caption),
        }.With(huxerui::Align(huxerui::HorizontalAlignment::Center, huxerui::VerticalAlignment::Center),
               huxerui::Padding(huxerui::EdgeInsets::Symmetric(6.0F, 0.0F)),
               huxerui::Frame{.min_width = 24.0F, .min_height = 22.0F},
               huxerui::Background(islands.active), huxerui::CornerRadius(theme.shapes.small))
            .Key("proxy-group-count");
        const auto action = Localized(expanded ? "收起" : "展开");
        huxerui::Semantics semantics{.label = title, .hint = action, .expanded = expanded};
        if (!selectedNode.empty()) semantics.state_description = selectedNode;
        auto arrow = huxerui::IconButton(app::images::chevron_down, title)
            .With(huxerui::Frame{.width = 32.0F, .height = 32.0F},
                  huxerui::Foreground(theme.colors.on_surface_variant),
                  huxerui::Rotation(expanded ? 180.0F : 0.0F), huxerui::Enabled(bool(toggle)),
                  huxerui::Tooltip(action), semantics)
            .Key("proxy-group-expand");
        if (toggle) arrow = std::move(arrow).OnClick(toggle);
        const auto warning = degraded ? huxerui::View{huxerui::Text("!").Style(
            huxerui::TextStyle{huxerui::Font::System(font_size::kCaption), theme.colors.error})}
            : huxerui::View{};
        return huxerui::Row {
          huxerui::Row {
            heading,
            huxerui::Text(type).LineLimit(1).Overflow(huxerui::TextOverflow::Ellipsis).Style(caption),
            warning,
          }.With(huxerui::Grow(1.0F), huxerui::Spacing(6.0F),
                 huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center), huxerui::ClipChildren()),
          count,
          speed,
          arrow,
        }.With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(kProxyGroupInset, 6.0F)),
               huxerui::Frame{.min_height = 48.0F},
               huxerui::Spacing(4.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    });
}

// The list virtualizes individual rows. Each row paints its part of the
// same panel, so a large expanded group never needs to mount all its nodes.
inline huxerui::View ProxyGroupPanelPart(huxerui::View content,
    bool first, bool last, bool followedByGroup) {
    return huxerui::Scope([=] {
        const auto islands = ResolveIslandTheme(huxerui::UseTheme());
        const float top = first ? islands.nested_radius : 0.0F;
        const float bottom = last ? islands.nested_radius : 0.0F;
        return huxerui::Column {
          huxerui::Column {content}.With(
              huxerui::Background(islands.raised), huxerui::CornerRadius(top, top, bottom, bottom),
              huxerui::ClipChildren(), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
        }.With(huxerui::Padding(huxerui::EdgeInsets{.bottom = followedByGroup ? kProxyGroupGap : 0.0F}),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    });
}

inline huxerui::View ProxyGroupNodeRow(std::vector<huxerui::View> nodes, std::size_t columns, bool last) {
    // Keep the last incomplete row aligned with the complete rows.
    while (nodes.size() < columns) {
        nodes.push_back(huxerui::Spacer().With(huxerui::Semantics{.hidden = true}));
    }
    return huxerui::Row(std::move(nodes)).With(
        huxerui::Padding(huxerui::EdgeInsets{
            .right = kProxyGroupInset, .bottom = last ? kProxyGroupInset : kSectionCardSpacing,
            .left = kProxyGroupInset}),
        huxerui::Spacing(kSectionCardSpacing),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

struct MeasureProxyGroupColumns {
    class Extension;
    huxerui::State<std::size_t> columns;
    huxerui::TaskScope tasks;
    bool compact;
};

class MeasureProxyGroupColumns::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode&, const MeasureProxyGroupColumns& spec) : spec_(spec) {}

    void Update(huxerui::ViewNode&, const MeasureProxyGroupColumns& spec) { spec_ = spec; }

    PaintInvalidation PrepareGeometry(huxerui::ViewNode& node, huxerui::TextMeasurer&) override {
        const float width = node.ContentBounds().width;
        if (width <= 0.0F || spec_.compact) return PaintInvalidation::None;
        const auto count = huxerui::GridColumns::Adaptive(kProxyNodeWidth).Resolve(
            std::max(0.0F, width - 2.0F * kProxyGroupInset), kSectionCardSpacing);
        if (spec_.columns.Get() != count) {
            spec_.tasks.Post([columns = spec_.columns, count] { columns = count; });
        }
        return PaintInvalidation::None;
    }

private:
    MeasureProxyGroupColumns spec_;
};

} // namespace clashflux::ui
