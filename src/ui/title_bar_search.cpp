// title_bar_search.cpp — 共用页面搜索工具栏与聚焦描边。
// 单独成 TU：无头几何回归（test_page_transition）直链本文件，不拉入
// common.cpp 的存储与页面依赖。
#include "title_bar_search.h"

#include "app_resources.h"
#include "page_layout.h"
#include "ui.h"

#include <algorithm>
#include <chrono>

namespace clashflux::ui {

namespace {

struct SearchFocusOutline {
    class Extension;
    bool focused = false;
    huxerui::Color color;
    bool animated = true;
    bool four_color = false;
    huxerui::ThemeBrightness brightness = huxerui::ThemeBrightness::Light;
    std::optional<FluxMultiColorPalette> palette = std::nullopt;
    bool operator==(const SearchFocusOutline&) const = default;
};

class SearchFocusOutline::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode& node, const SearchFocusOutline& value) { Update(node, value); }

    void Update(huxerui::ViewNode&, const SearchFocusOutline& value) {
        if (!value.focused || !value.animated) {
            reveal_.Set(value.focused ? 1.0F : 0.0F);
        } else if (!focused_) {
            reveal_.Set(0.0F);
            reveal_.AnimateTo(1.0F, huxerui::TweenSpec{0.25, huxerui::Easing::EaseOut});
        }
        focused_ = value.focused;
        color_ = value.color;
        four_color_ = value.four_color;
        brightness_ = value.brightness;
        palette_ = value.palette;
        InvalidatePaint(PaintInvalidation::Foreground);
    }

    FrameResult OnFrame(huxerui::ViewNode&, const huxerui::FrameInfo& frame) override {
        const auto result = reveal_.Advance(frame);
        if (result.changed) InvalidatePaint(PaintInvalidation::Foreground);
        return {.needs_frame = result.needs_frame, .wake_after = result.wake_after};
    }

    void PaintAboveContent(const huxerui::ViewNode& node, huxerui::PaintContext& paint) const override {
        const float progress = std::clamp(reveal_.Value(), 0.0F, 1.0F);
        if (!focused_ || progress <= 0.0F) return;
        const auto bounds = node.Bounds();
        const huxerui::CornerRadii corners{bounds.height / 2.0F};
        // Reveal both horizontal edges together, keeping the original capsule
        // geometry. Only this foreground paint sequence changes each frame.
        paint.PushClip({bounds.x, bounds.y, bounds.width * progress, bounds.height});
        if (four_color_) {
            PaintMultiColorStroke(paint, bounds, corners,
                                  palette_.value_or(DefaultMultiColorPalette()), brightness_, 1.0F);
        } else {
            paint.DrawBorder(bounds, color_, huxerui::StrokeStyle{.width = 1.0F}, corners);
        }
        paint.PopClip();
    }

private:
    bool focused_ = false;
    huxerui::Color color_;
    bool four_color_ = false;
    huxerui::ThemeBrightness brightness_ = huxerui::ThemeBrightness::Light;
    std::optional<FluxMultiColorPalette> palette_{};
    huxerui::MotionController reveal_{0.0F};
};

} // namespace

huxerui::View WithSearchFocusOutline(huxerui::View content, bool focused,
                                     huxerui::Color color, bool animated,
                                     bool fourColor, huxerui::ThemeBrightness brightness,
                                     std::optional<FluxMultiColorPalette> palette) {
    return std::move(content).With(SearchFocusOutline{
        focused, color, animated, fourColor, brightness, std::move(palette)});
}

namespace {

constexpr float kSearchButtonWidth = 40.0F;
constexpr float kSearchActionGap = 4.0F;
constexpr float kSearchActionHoverDiameter = 28.0F;
constexpr float kSearchIconSize = 20.0F;
constexpr float kSearchLeadingInset = 14.0F;
constexpr double kSearchOpenSeconds = 0.18;
constexpr double kSearchCloseSeconds = 0.14;
constexpr double kHeaderFadeSeconds = 0.08;

struct SearchToolbarLayoutOptions {
    struct Value {
        bool compact;
        float height;
        float pillHeight;
        bool operator==(const Value&) const = default;
    };
};

struct SearchToolbarGeometry {
    huxerui::Rect button;
    huxerui::Rect pill;
    bool operator==(const SearchToolbarGeometry&) const = default;
};

class SearchToolbarLayout final : public huxerui::Layout<SearchToolbarLayout> {
public:
    using Layout::Layout;

    static huxerui::LayoutResult Measure(huxerui::LayoutContext& context, huxerui::ViewNode& node,
                                        huxerui::Constraints constraints) {
        const auto [compact, preferredHeight, preferredPillHeight] =
            *node.LayoutValue<SearchToolbarLayoutOptions>();
        auto& title = node.ChildAt(0);
        auto& button = node.ChildAt(1);
        auto& actions = node.ChildAt(2);
        auto& editor = node.ChildAt(3);
        const float height = constraints.ConstrainHeight(preferredHeight);
        const auto actionSize = context.Measure(actions, {
            0.0F, std::max(0.0F, constraints.max_width - kSearchButtonWidth - kSearchActionGap),
            0.0F, height});
        const float gap = actionSize.width > 0.0F ? kSearchActionGap : 0.0F;
        const float titleSpace = std::max(0.0F,
            constraints.max_width - actionSize.width - gap - kSearchButtonWidth - node.Spacing());
        const auto titleSize = context.Measure(title, {0.0F, titleSpace, 0.0F, height});
        const float width = constraints.HasBoundedWidth() ? constraints.max_width
            : constraints.ConstrainWidth(titleSize.width + node.Spacing() +
                                        kDesktopTitleBarSearchWidth + gap + actionSize.width);
        const float actionX = width - actionSize.width;
        const float searchRight = std::max(0.0F, actionX - gap);
        const float buttonWidth = std::min(searchRight, kSearchButtonWidth);
        const float pillWidth = compact ? width : std::min(searchRight, kDesktopTitleBarSearchWidth);
        const float pillHeight = std::min(height, preferredPillHeight);
        const auto buttonRect = huxerui::Rect{searchRight - buttonWidth, 0.0F, buttonWidth, height};
        const auto pillRect = huxerui::Rect{compact ? 0.0F : searchRight - pillWidth,
            (height - pillHeight) / 2.0F, pillWidth, pillHeight};
        node.Cache<SearchToolbarGeometry>() = {buttonRect, pillRect};

        context.Measure(button, {buttonWidth, buttonWidth, height, height});
        context.Measure(editor, {pillWidth, pillWidth, pillHeight, pillHeight});
        huxerui::LayoutResult result;
        return result.SetSize({width, height})
            .Place(title, {0.0F, (height - titleSize.height) / 2.0F})
            .Place(button, {buttonRect.x, buttonRect.y})
            .Place(actions, {actionX, (height - actionSize.height) / 2.0F})
            .Place(editor, {pillRect.x, pillRect.y});
    }
};

struct SearchReveal {
    class Extension;
    bool open;
    bool animated;
    huxerui::Color fill;
    huxerui::Color border;
    huxerui::Color foreground;
    huxerui::VectorAsset icon;
    float buttonIconSize;
    bool operator==(const SearchReveal&) const = default;
};

class SearchReveal::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode&, const SearchReveal& value)
        : style_(value), progress_(value.open ? 1.0F : 0.0F) {}

    void Update(huxerui::ViewNode&, const SearchReveal& value) {
        const float target = value.open ? 1.0F : 0.0F;
        if (!value.animated) {
            progress_.Set(target);
        } else if (progress_.Target() != target) {
            progress_.AnimateTo(target, huxerui::TweenSpec{
                value.open ? kSearchOpenSeconds : kSearchCloseSeconds,
                value.open ? huxerui::Easing::EaseOut : huxerui::Easing::EaseIn});
        }
        style_ = value;
        InvalidatePaint(PaintInvalidation::Both);
    }

    FrameResult OnFrame(huxerui::ViewNode&, const huxerui::FrameInfo& frame) override {
        const auto result = progress_.Advance(frame);
        if (result.changed) InvalidatePaint(PaintInvalidation::Both);
        return {result.needs_frame, result.wake_after};
    }

    PaintInvalidation PrepareGeometry(huxerui::ViewNode& node, huxerui::TextMeasurer&) override {
        const auto& geometry = node.Cache<SearchToolbarGeometry>();
        if (geometry == geometry_) return PaintInvalidation::None;
        geometry_ = geometry;
        return PaintInvalidation::Both;
    }

    void PaintBehindContent(const huxerui::ViewNode&, huxerui::PaintContext& paint) const override {
        const float progress = progress_.Value();
        const auto [button, pill] = geometry_;
        if (progress <= 0.0F || pill.IsEmpty()) return;

        // Both edges travel at the same rate until they reach their final limits.
        // Wide toolbars cap the right edge at the action group throughout.
        const float leftTravel = button.x - pill.x;
        const float rightTravel = pill.x + pill.width - button.x - button.width;
        const float travel = std::max(leftTravel, rightTravel) * progress;
        const float left = button.x - std::min(leftTravel, travel);
        const float right = button.x + button.width + std::min(rightTravel, travel);
        const float height = std::lerp(std::min(button.width, pill.height), pill.height, progress);
        const huxerui::Rect bounds{left, pill.y + (pill.height - height) / 2.0F, right - left, height};
        const huxerui::CornerRadii corners{std::min(bounds.width, bounds.height) / 2.0F};
        const float opacity = std::min(progress * 4.0F, 1.0F);
        auto fill = style_.fill;
        auto border = style_.border;
        fill.alpha *= opacity;
        border.alpha *= opacity;
        paint.DrawRect(bounds, fill, corners);
        paint.DrawBorder(bounds, border, huxerui::StrokeStyle{.width = 1.0F}, corners);
    }

    void PaintAboveContent(const huxerui::ViewNode&, huxerui::PaintContext& paint) const override {
        const auto [button, pill] = geometry_;
        if (button.IsEmpty() || pill.IsEmpty()) return;
        const float progress = progress_.Value();
        const float start = button.x + button.width / 2.0F;
        const float end = pill.x + std::min(kSearchLeadingInset + kSearchIconSize / 2.0F, pill.width / 2.0F);
        const float center = std::lerp(start, end, progress * progress);
        const float size = std::lerp(std::min(style_.buttonIconSize, button.width),
            std::min(kSearchIconSize, pill.width), progress);
        paint.DrawImage(style_.icon, {center - size / 2.0F, (button.height - size) / 2.0F, size, size},
                        style_.foreground);
    }

private:
    SearchReveal style_;
    SearchToolbarGeometry geometry_;
    huxerui::MotionController progress_;
};

[[huxerui::composable]] huxerui::View SearchPill(
    huxerui::State<huxerui::TextEditingValue> value, huxerui::StringVariant placeholder,
    huxerui::ViewHandle focus, huxerui::State<bool> inputFocused,
    std::function<void()> close, float height, bool animated) {
    const auto& theme = huxerui::UseTheme();
    const bool focused = inputFocused.Get();
    huxerui::TextFieldStyle inputStyle = huxerui::UseEnvironment<huxerui::TextFieldStyle>();
    inputStyle.border_width = 0.0F;
    inputStyle.focused_border_width = 0.0F;
    inputStyle.validation_border_width = 0.0F;
    inputStyle.focused_validation_border_width = 0.0F;
    inputStyle.standard.background = huxerui::Color::Transparent();
    inputStyle.standard.border = huxerui::Color::Transparent();
    inputStyle.standard.hovered_border = huxerui::Color::Transparent();
    inputStyle.standard.focused_border = huxerui::Color::Transparent();
    inputStyle.standard.disabled_border = huxerui::Color::Transparent();
    inputStyle.standard.minimum_height = height;
    inputStyle.padding = huxerui::EdgeInsets{};
    huxerui::ThemeDefinition inputTheme;
    inputTheme.Set(inputStyle);
    const auto closeLabel = Localized("退出搜索");
    auto closeStyle = huxerui::UseEnvironment<huxerui::IconButtonStyle>();
    closeStyle.icon_size = 16.0F;
    huxerui::View pill = huxerui::Row{
        huxerui::Row {}.With(huxerui::Frame{.width = kSearchIconSize}),
        huxerui::Theme(std::move(inputTheme), huxerui::TextField(value.Get())
            .Placeholder(placeholder)
            .Variant(huxerui::TextFieldVariant::Standard)
            .OnChanged([value](const huxerui::TextEditingValue& next) { value = next; })
            .On<huxerui::ViewEvents::FocusChanged>([inputFocused](bool next) { inputFocused = next; })
            .With(focus, huxerui::Grow(1.0F))
            .Key("page-search-input")),
        huxerui::ProvideEnvironment(closeStyle, huxerui::IconButton(app::images::close, closeLabel)
            .OnClick(close)
            .With(huxerui::Frame{.width = 32.0F, .height = 32.0F}, huxerui::Tooltip(closeLabel))
            .Key("page-search-close")),
    }.With(huxerui::Frame{.height = height},
           huxerui::Padding(huxerui::EdgeInsets{.right = 4.0F, .left = kSearchLeadingInset}),
           huxerui::Spacing(6.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
           huxerui::CornerRadius(height / 2.0F),
           huxerui::ClipChildren(),
           huxerui::Grow(1.0F))
        .On<huxerui::ViewEvents::KeyIntercept>([close](const huxerui::KeyEvent& event) {
            if (event.type != huxerui::KeyEventType::Down || event.key != huxerui::Key::Escape) return false;
            close();
            return true;
        }).Key("pill-search-field");
    const auto& gradient = huxerui::UseEnvironment<FluxGradientStyle>();
    return WithSearchFocusOutline(std::move(pill), focused, theme.colors.primary, animated,
                                  gradient.enabled, theme.brightness, gradient.palette);
}

} // namespace

PageSearchState UsePageSearch(bool active) {
    const auto value = huxerui::UseState(huxerui::TextEditingValue{});
    const auto expanded = huxerui::UseState(false);
    // The Pager keeps the page scope mounted while its toolbar is removed.
    huxerui::Lifecycle([active, value, expanded] {
        if (!active) {
            value = huxerui::TextEditingValue{};
            expanded = false;
        }
        return [] {};
    }, active);
    return {value, expanded};
}

[[huxerui::composable]] huxerui::View PageSearchToolbar(
    huxerui::State<huxerui::TextEditingValue> value, huxerui::State<bool> expanded,
    huxerui::StringVariant title, huxerui::StringVariant placeholder, huxerui::View actions) {
    const auto& theme = huxerui::UseTheme();
    auto icons = huxerui::UseEnvironment<huxerui::IconButtonStyle>();
    // Leave room inside the 32pt close and speed buttons so their state layers stay circular.
    icons.state_layer_size = kSearchActionHoverDiameter;
    icons.corner_radius = std::max(icons.corner_radius, kSearchActionHoverDiameter / 2.0F);
    huxerui::ThemeDefinition toolbarTheme;
    toolbarTheme.Set(icons);
    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    const auto& leading = huxerui::UseEnvironment<CompactSearchHeaderLeading>().content;
    const bool desktopTitle = kPageTitlesInWindow && !compact;
    const bool open = expanded.Get();
    const bool animated = !theme.motion.reduced_motion &&
        huxerui::UseWindow().LifecycleState() != huxerui::WindowLifecycleState::Background;
    const auto inputFocused = huxerui::UseState(false);
    const auto input = huxerui::UseViewHandle();
    const auto toggle = huxerui::UseViewHandle();
    const auto tasks = huxerui::UseTaskScope();
    const auto restoreFocus = huxerui::UseState(false);
    huxerui::Lifecycle([open, input, toggle, tasks, restoreFocus, animated] {
        huxerui::TaskHandle pending;
        if (open || restoreFocus.Get()) {
            pending = tasks.Launch([open, input, toggle, restoreFocus, animated]() -> huxerui::Task<void> {
                // Wait for the editor to arrive before showing the caret or keyboard.
                if (animated) {
                    const auto duration = open ? kSearchOpenSeconds : kSearchCloseSeconds;
                    co_await huxerui::Delay(std::chrono::duration<double>{duration + 0.02});
                }
                if (open) {
                    input.RequestFocus();
                } else {
                    toggle.RequestFocus();
                    restoreFocus = false;
                }
            });
        }
        return [pending] { pending.Cancel(); };
    }, open, animated);

    const auto close = [expanded, value, inputFocused, restoreFocus] {
        value = huxerui::TextEditingValue{};
        inputFocused = false;
        restoreFocus = true;
        expanded = false;
    };
    const float pillHeight = desktopTitle ? kDesktopTitleBarSearchHeight : kPageHeaderHeight;
    huxerui::View button;
    huxerui::View editor;
    if (open) {
        editor = SearchPill(value, placeholder, input, inputFocused, close, pillHeight, animated)
            .With(huxerui::Grow(1.0F));
    } else {
        // The retained magnifier draws at the same center, including during motion.
        button = huxerui::IconButton(app::images::search, placeholder)
            .OnClick([expanded] { expanded = true; })
            .With(toggle, huxerui::Tooltip(placeholder), huxerui::Foreground(huxerui::Color::Transparent()))
            .Key("page-search-toggle");
    }

    const bool hideHeaderItems = compact && open;
    const float hiddenLeadingWidth = hideHeaderItems && leading
        ? std::max(icons.icon_size, icons.minimum_interactive_size) + theme.spacing.small : 0.0F;
    const auto headerMotion = animated && compact
        ? huxerui::AnimationSpec{huxerui::TweenSpec{kHeaderFadeSeconds, huxerui::Easing::Linear}}
        : huxerui::AnimationSpec{huxerui::SnapSpec{}};
    const double fadeDelay = animated && compact && !open ? kSearchCloseSeconds - kHeaderFadeSeconds : 0.0;
    const auto headerOpacity = huxerui::Opacity(huxerui::AnimateTo(hideHeaderItems ? 0.0F : 1.0F,
        headerMotion, huxerui::AnimationPlayback{.delay = fadeDelay}));
    const huxerui::Semantics visibility{.hidden = hideHeaderItems};
    const auto editorMotion = animated && open
        ? huxerui::AnimationSpec{huxerui::TweenSpec{kHeaderFadeSeconds, huxerui::Easing::EaseOut}}
        : huxerui::AnimationSpec{huxerui::SnapSpec{}};
    const double editorDelay = animated && open ? kSearchOpenSeconds - kHeaderFadeSeconds : 0.0;
    const auto editorOpacity = huxerui::Opacity(huxerui::AnimateTo(open ? 1.0F : 0.0F, editorMotion,
        huxerui::AnimationPlayback{.delay = editorDelay}));
    huxerui::View toolbar = SearchToolbarLayout {
      huxerui::Row {
        leading && !hideHeaderItems ? *leading : huxerui::View{},
        compact ? PageTitleText(theme, title) : huxerui::View{},
      }.With(huxerui::Spacing(theme.spacing.small), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
             huxerui::Padding(huxerui::EdgeInsets{.left = hiddenLeadingWidth}), headerOpacity, visibility)
          .Key("page-search-title"),
      huxerui::Row {button}.With(huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
          huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
          .Key("page-search-trigger"),
      huxerui::Row {actions}.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
          headerOpacity, huxerui::Enabled(!hideHeaderItems), visibility)
          .Key("page-search-actions"),
      huxerui::Row {editor}.With(editorOpacity, huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
          .Key("page-search-editor"),
    }.LayoutValue<SearchToolbarLayoutOptions>(
        {compact, desktopTitle ? kDesktopTitleBarHeight : kPageHeaderHeight, pillHeight})
        .With(huxerui::Spacing(theme.spacing.small), SearchReveal{
            open, animated,
            inputFocused.Get() ? huxerui::Color::Transparent() : theme.colors.surface_container_highest,
            theme.colors.outline, theme.colors.on_surface_variant, huxerui::UseVectorImage(app::images::search),
            icons.icon_size})
        .Key("page-search-toolbar");
    return huxerui::Theme(std::move(toolbarTheme), toolbar);
}

} // namespace clashflux::ui
