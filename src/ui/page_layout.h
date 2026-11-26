#pragma once

#include <vector>

#include "app_resources.h"
#include "ui.h"
#include "scroll_area.h"

namespace clashflux::ui {

inline constexpr float kDesktopPageHorizontalInset = 24.0F;
inline constexpr float kDesktopPageLeadingInset = 8.0F;
inline constexpr float kPageHeaderHeight = 48.0F;
inline constexpr float kSettingsContentMaxWidth = 720.0F;

inline huxerui::View ConstrainSettingsContentWidth(huxerui::View content, bool constrain) {
    if (!constrain) return content;
    return huxerui::Row {
      huxerui::Spacer(),
      huxerui::View{std::move(content)}.With(huxerui::Frame{.max_width = kSettingsContentMaxWidth}),
      huxerui::Spacer(),
    }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Start));
}

// Put the inset around the full header slot rather than inside its min-height.
inline huxerui::View PageHeaderWithInset(huxerui::View header, float inset) {
    return huxerui::Column {header}.With(huxerui::Padding(huxerui::EdgeInsets{.top = inset}),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// A scaffold changes hierarchy across desktop/compact layouts. Record real
// scroll activity without observable writes, then seed a newly mounted scroll
// container. A controller alone does not restore its offset after reattachment.
struct RememberPageScroll {
    class Extension;
    std::shared_ptr<PageScrollOffset> offset;
    bool operator==(const RememberPageScroll&) const = default;
};
class RememberPageScroll::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode&, const RememberPageScroll& value) : offset_(value.offset) {}
    void Update(huxerui::ViewNode&, const RememberPageScroll& value) { offset_ = value.offset; }
    void OnScrollActivity(huxerui::ViewNode&, const huxerui::ScrollActivity& activity) override {
        if (activity.phase != huxerui::ScrollPhase::Cancel &&
            activity.source != huxerui::ScrollSource::Programmatic)
            offset_->value = activity.metrics.offset;
    }
private:
    std::shared_ptr<PageScrollOffset> offset_;
};

inline huxerui::View ScrollablePageContent(huxerui::View content,
    std::shared_ptr<PageScrollOffset> offset, ScrollAreaInsets insets = {}) {
    return huxerui::Scope([content, offset, insets] {
        const auto controller = huxerui::UseScrollController(offset->value);
        return ScrollArea(huxerui::ScrollView(PageScrollContent(content))
            .With(RememberPageScroll{offset}), controller, offset, {}, insets).With(huxerui::Grow(1.0F));
    }).With(huxerui::Grow(1.0F));
}

inline huxerui::View ScrollablePageContent(huxerui::View content, ScrollAreaInsets insets = {}) {
    return huxerui::Scope([content, insets] {
        const auto offset = huxerui::UseState(std::make_shared<PageScrollOffset>()).Get();
        return ScrollablePageContent(content, offset, insets);
    }).With(huxerui::Grow(1.0F));
}

#if defined(__ANDROID__) || defined(CLASHFLUX_IOS)
inline constexpr bool kPageTitlesInWindow = false;
#else
inline constexpr bool kPageTitlesInWindow = true;
#endif

inline huxerui::EdgeInsets PageContentInsets(const huxerui::ThemeSpec& theme,
    bool compact, float horizontal) {
    return {
        .top = compact ? theme.spacing.medium : kDesktopTopContentGap,
        .right = horizontal,
        // 页面铺到窗口（Compact 含系统安全区）底边；桌面右下角悬浮按钮与
        // Compact 悬浮导航的避让由可滚动内容尾部的 footer 提供，不缩短整页容器。
        .bottom = 0.0F,
        .left = horizontal,
    };
}

inline huxerui::View PageTitleText(const huxerui::ThemeSpec& theme,
    huxerui::StringVariant title) {
    return huxerui::Text(std::move(title)).Style(huxerui::TextStyle{
        huxerui::Font::System(font_size::kTitle).WithWeight(huxerui::FontWeight::Medium),
        theme.colors.on_surface}).Key("page-title");
}

inline huxerui::View PageBackButton(std::function<void()> onBack) {
    return huxerui::Scope([onBack = std::move(onBack)] {
        const auto label = Localized("返回上一页");
        auto style = huxerui::UseEnvironment<huxerui::IconButtonStyle>();
        style.corner_radius = style.state_layer_size / 2.0F;
        return huxerui::ProvideEnvironment(style, huxerui::IconButton(app::images::arrow_back, label)
            .With(huxerui::Tooltip(label)).OnClick(onBack).Key("page-back"));
    });
}

inline huxerui::View PageHeaderLayout(const huxerui::ThemeSpec& theme,
    huxerui::View title, huxerui::View actions, bool stackedActions) {
    if (!title) {
        if (!actions) return {};
        return huxerui::Row{huxerui::Spacer(), std::move(actions)}
            .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
            .Key("page-actions");
    }
    if (stackedActions) {
        return huxerui::Column{
            huxerui::Row{std::move(title)}.With(
                huxerui::Frame{.min_height = kPageHeaderHeight},
                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
            std::move(actions),
        }.With(huxerui::Spacing(theme.spacing.small),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
            .Key("page-header");
    }
    return huxerui::Row{
        std::move(title), huxerui::Spacer(), std::move(actions),
    }.With(huxerui::Frame{.min_height = kPageHeaderHeight},
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
        .Key("page-header");
}

inline huxerui::View DesktopPageTitleContent(huxerui::View title, huxerui::View actions,
                                            huxerui::View searchToolbar) {
    std::vector<huxerui::View> children;
    children.push_back(std::move(title));
    huxerui::View toolbar;
    if (searchToolbar) {
        toolbar = huxerui::Row{std::move(searchToolbar).With(huxerui::Grow(1.0F))}
            .With(huxerui::Grow(1.0F));
    } else {
        children.push_back(huxerui::Spacer{});
        toolbar = huxerui::Row{std::move(actions)};
    }
    children.push_back(std::move(toolbar).With(
        huxerui::Padding(huxerui::EdgeInsets{.right = 8.0F}),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
    return huxerui::Row(std::move(children)).With(huxerui::Spacing(12.0F), huxerui::Grow(1.0F),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

struct CompactSearchHeaderLeading {
    std::shared_ptr<const huxerui::View> content;
    static CompactSearchHeaderLeading Default() { return {}; }
    bool operator==(const CompactSearchHeaderLeading&) const = default;
};

inline huxerui::View SearchPageHeader(const huxerui::ThemeSpec& theme,
    huxerui::View leading, huxerui::View toolbar) {
    toolbar = std::move(toolbar).With(huxerui::Grow(1.0F)).Key("page-header-search");
    std::vector<huxerui::View> children;
    if (leading && huxerui::UseViewportClass() == huxerui::ViewportClass::Compact) {
        toolbar = huxerui::ProvideEnvironment(
            CompactSearchHeaderLeading{std::make_shared<const huxerui::View>(std::move(leading))}, toolbar);
    } else if (leading) {
        children.push_back(leading);
    }
    children.push_back(toolbar);
    return huxerui::Row(std::move(children)).With(huxerui::Frame{.height = kPageHeaderHeight},
           huxerui::Spacing(theme.spacing.small),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
        .Key("page-header");
}

inline huxerui::View CenteredPageHeader(const huxerui::ThemeSpec& theme,
    huxerui::View title, huxerui::View leading, huxerui::View trailing) {
    const auto actionSlot = [](huxerui::View action) {
        return huxerui::Row{std::move(action)}
            .With(huxerui::Frame{.width = 40.0F, .height = 40.0F},
                  huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    };
    return huxerui::Row{
        actionSlot(std::move(leading)),
        huxerui::Row{std::move(title)}.With(huxerui::Grow(1.0F),
            huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        actionSlot(std::move(trailing)),
    }.With(huxerui::Frame{.min_height = kPageHeaderHeight},
           huxerui::Spacing(theme.spacing.small),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
        .Key("centered-page-header");
}

} // namespace clashflux::ui
