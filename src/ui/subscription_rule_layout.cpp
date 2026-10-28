#include "subscription_rule_layout.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "ui.h"

namespace clashflux::ui {
namespace {

// Text currently has no single-line ellipsis contract. Keep ordinary Row/Column
// measurement, then fit this label against its final local bounds without State,
// a second layout pass, or retaining the composition's borrowed TextMeasurer.
struct FittedRuleLabel {
    class Extension;
    std::string text;
    huxerui::TextStyle style;
    huxerui::TextAlign align = huxerui::TextAlign::Leading;
    bool operator==(const FittedRuleLabel&) const = default;
};

class FittedRuleLabel::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode& node, const FittedRuleLabel& value) { Update(node, value); }

    void Update(huxerui::ViewNode&, const FittedRuleLabel& value) {
        value_ = value;
        width_ = -1.0F;
        InvalidatePaint(PaintInvalidation::Foreground);
    }

    PaintInvalidation PrepareGeometry(huxerui::ViewNode& node, huxerui::TextMeasurer& measurer) override {
        const float width = node.ContentBounds().width;
        if (width == width_) return PaintInvalidation::None;
        width_ = width;
        const auto fits = [&](const std::string& text) {
            return measurer.MeasureText(text, value_.style).size.width <= width;
        };
        rendered_ = value_.text;
        if (!fits(rendered_)) {
            rendered_.clear();
            if (fits("…")) {
                std::vector<std::size_t> ends{0};
                for (std::size_t index = 1; index <= value_.text.size(); ++index)
                    if (index == value_.text.size() ||
                        (static_cast<unsigned char>(value_.text[index]) & 0xc0U) != 0x80U)
                        ends.push_back(index);
                std::size_t low = 0, high = ends.size();
                while (low + 1 < high) {
                    const auto middle = (low + high) / 2;
                    if (fits(value_.text.substr(0, ends[middle]) + "…")) low = middle;
                    else high = middle;
                }
                rendered_ = value_.text.substr(0, ends[low]) + "…";
            }
        }
        return PaintInvalidation::Foreground;
    }

    void PaintAboveContent(const huxerui::ViewNode& node, huxerui::PaintContext& paint) const override {
        const auto bounds = node.ContentBounds();
        paint.PushClip(bounds);
        paint.DrawText(bounds, rendered_, value_.style,
            {.align = value_.align, .wrap = huxerui::TextWrap::NoWrap});
        paint.PopClip();
    }

private:
    FittedRuleLabel value_;
    float width_ = -1.0F;
    std::string rendered_;
};

} // namespace

huxerui::View FittedSingleLineLabel(std::string text, huxerui::TextStyle style, huxerui::TextAlign align) {
    return huxerui::Scope([text, style, align] {
        // Explicit source line breaks must not turn a regex/name into extra rows.
        std::string line = text;
        std::replace(line.begin(), line.end(), '\n', ' ');
        std::replace(line.begin(), line.end(), '\r', ' ');
        auto& measurer = huxerui::UseTextMeasurer();
        const auto metrics = measurer.MeasureText(line.empty() ? " " : line, style);
        huxerui::View label = huxerui::Row {}.With(
            huxerui::Frame{.width = std::ceil(metrics.size.width), .height = std::ceil(metrics.size.height)},
            FittedRuleLabel{line, style, align});
        if (!text.empty()) label = std::move(label).With(huxerui::Tooltip(text));
        return label;
    }).With(huxerui::Semantics{.role = huxerui::SemanticRole::Text, .label = text});
}

huxerui::View SubscriptionRulesToolbar(std::vector<std::string> sources, std::size_t source,
    std::vector<std::string> views, std::size_t view, bool busy,
    std::function<void(std::size_t)> selectSource, std::function<void(std::size_t)> selectView,
    huxerui::View more) {
    return huxerui::Scope([=] {
        const auto& theme = huxerui::UseTheme();
        const huxerui::TextStyle style{huxerui::Font::System(font_size::kBodySmall), theme.colors.on_surface};
        auto selects = huxerui::UseEnvironment<huxerui::SelectStyle>();
        selects.minimum_width = 0.0F;
        selects.trigger_padding = huxerui::EdgeInsets::Symmetric(8.0F, 8.0F);
        auto& measurer = huxerui::UseTextMeasurer();
        const float chrome = 16.0F + selects.content_spacing + selects.indicator_size;
        const float sourceWidth = std::min(240.0F,
            measurer.MeasureText(sources.at(source), style).size.width + chrome);
        float viewWidth = 0.0F;
        for (const auto& label : views)
            viewWidth = std::max(viewWidth, measurer.MeasureText(label, style).size.width + chrome);
        const auto label = [style](const std::string& text) { return FittedSingleLineLabel(text, style); };
        const float moreWidth = huxerui::UseEnvironment<huxerui::IconButtonStyle>().minimum_interactive_size;
        // Cap the non-growing group, not its growing source slot: tight Grow
        // constraints would override a max_width applied directly to that slot.
        return huxerui::Row {
          huxerui::Theme(huxerui::ThemeDefinition{}.Set(selects), huxerui::Row {
            huxerui::Row {
              huxerui::Select(sources, source, label)
                  .With(huxerui::Grow(1.0F), huxerui::Enabled(!busy))
                  .OnChanged(selectSource).Key("subscription-rule-source"),
            }.With(huxerui::Grow(1.0F)),
            huxerui::Select(views, view, label).With(huxerui::Frame{.width = viewWidth})
                .OnChanged(selectView).Key("subscription-rule-view"),
            more,
          }.With(huxerui::Spacing(8.0F),
                 huxerui::Frame{.max_width = sourceWidth + viewWidth + moreWidth + 16.0F},
                 huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))),
          huxerui::Spacer(),
        }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    });
}

huxerui::View SubscriptionRuleContent(std::string kind, std::string pattern, std::string target,
    std::string status, huxerui::View more, bool enabled) {
    return huxerui::Scope([=] {
        const auto& theme = huxerui::UseTheme();
        const huxerui::TextStyle body{huxerui::Font::System(font_size::kBodySmall),
            enabled ? theme.colors.on_surface : theme.colors.on_surface_variant};
        const huxerui::TextStyle secondary{huxerui::Font::System(font_size::kCaption), theme.colors.on_surface_variant};
        const huxerui::TextStyle destination{huxerui::Font::System(font_size::kBodySmall), theme.colors.primary};
        const float kindWidth = std::min(88.0F, std::ceil(huxerui::UseTextMeasurer().MeasureText(kind, body).size.width));
        return huxerui::Row {
          huxerui::Column {
            huxerui::Row {
              FittedSingleLineLabel(kind, body).With(huxerui::Frame{.width = kindWidth}).Key("rule-kind"),
              FittedSingleLineLabel(pattern, body).With(huxerui::Grow(1.0F)).Key("rule-pattern"),
              huxerui::Row {
                FittedSingleLineLabel(target, destination).With(huxerui::Frame{.max_width = 160.0F}).Key("rule-target"),
              }.With(huxerui::Grow(0.45F)),
            }.With(huxerui::Spacing(6.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
            huxerui::Row {
              huxerui::Row {}.With(huxerui::Frame{.width = kindWidth}),
              FittedSingleLineLabel(status, secondary).With(huxerui::Grow(1.0F)).Key("rule-status"),
            }.With(huxerui::Spacing(6.0F)),
          }.With(huxerui::Spacing(4.0F), huxerui::Grow(1.0F),
                 huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
          more,
        }.With(huxerui::Spacing(6.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    });
}

} // namespace clashflux::ui
