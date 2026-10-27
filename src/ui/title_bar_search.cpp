// title_bar_search.cpp — 桌面标题栏搜索胶囊的响应式叠放层。
// 单独成 TU：无头几何回归（test_page_transition）直链本文件，不拉入
// common.cpp 的存储与页面依赖。
#include "title_bar_search.h"

#include "ui.h"

#include <algorithm>

namespace clashflux::ui {

namespace {

struct SearchFocusOutline {
    class Extension;
    bool focused = false;
    huxerui::Color color;
    bool animated = true;
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
        // Reveal both horizontal edges together, keeping the original capsule
        // geometry. Only this foreground paint sequence changes each frame.
        paint.PushClip({bounds.x, bounds.y, bounds.width * progress, bounds.height});
        paint.DrawBorder(bounds, color_, huxerui::StrokeStyle{.width = 1.0F},
                         huxerui::CornerRadii{bounds.height / 2.0F});
        paint.PopClip();
    }

private:
    bool focused_ = false;
    huxerui::Color color_;
    huxerui::MotionController reveal_{0.0F};
};

} // namespace

huxerui::View WithSearchFocusOutline(huxerui::View content, bool focused,
                                     huxerui::Color color, bool animated) {
    return std::move(content).With(SearchFocusOutline{focused, color, animated});
}

huxerui::View TitleBarSearchStack(huxerui::View title_bar_content,
                                  huxerui::View title_search_field) {
    return huxerui::Stack{
        std::move(title_bar_content),
        huxerui::Row{
            huxerui::Spacer{}.With(huxerui::Grow(0.35F)),
            // 三层结构，绕开两个框架语义：
            // 1) Grow 子节点拿紧约束，Frame.max_width 会被压过——封顶不能写在
            //    任何 Grow 子节点上；
            // 2) 胶囊（PillSearchField）自身带 Grow(1.0F)，直接放进 Row 会成为
            //    Grow 子节点按权重分宽（0.3×1/3 ≈ 0.1 栏宽，缩成一小块）。
            // 因此：容量槽以 Grow(0.3) 拿标题栏剩余空间的 30%（紧约束）；
            // 槽内非 Grow 的 Column 按松约束测量、宽度 = min(槽宽, 320)（封顶在
            // Column 的 Frame 上生效）；胶囊在 Column 内横向铺满。
            huxerui::Row{
                huxerui::Spacer{}.With(huxerui::Grow(1.0F)),
                huxerui::Column{std::move(title_search_field)}
                    .With(huxerui::Frame{.max_width = kDesktopTitleBarSearchWidth},
                          huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
                huxerui::Spacer{}.With(huxerui::Grow(1.0F)),
            }
                .With(huxerui::Grow(0.3F),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
                .Key("title-search-slot"),
            huxerui::Spacer{}.With(huxerui::Grow(0.35F)),
        }
            .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
    }
        .With(huxerui::Grow(1.0F),
              huxerui::Align(huxerui::HorizontalAlignment::Stretch,
                             huxerui::VerticalAlignment::Stretch));
}

} // namespace clashflux::ui
