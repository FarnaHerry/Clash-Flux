// 页内分区共用的标签栏、滑动与方向过渡；独立编译供无窗口交互测试使用。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "section_swipe.h"
#include "ui.h"

namespace clashflux::ui {
namespace {

// ScrollView 没有按普通子节点 reveal 的 API；在挂载后的实际布局中定位选中标签。
// 只保存数值几何，不跨重组保留 ViewNode/child 指针。手动滚动不改变选中项，
// 因此不会被持续拉回；换标签、标签尺寸变化或窗口缩放才重新保证完整可见。
struct RevealSectionTab {
    class Extension;
    huxerui::ScrollController scroll;
    std::string selected_key;
    std::size_t selected_index;
    bool operator==(const RevealSectionTab&) const = default;
};

class RevealSectionTab::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode& node, const RevealSectionTab& spec)
        : scroll_(spec.scroll) {
        Update(node, spec);
    }

    void Update(huxerui::ViewNode&, const RevealSectionTab& spec) {
        pending_ = pending_ || selected_key_ != spec.selected_key ||
                   selected_index_ != spec.selected_index || scroll_ != spec.scroll;
        scroll_ = spec.scroll;
        selected_key_ = spec.selected_key;
        selected_index_ = spec.selected_index;
    }

    FrameResult OnFrame(huxerui::ViewNode&, const huxerui::FrameInfo&) override {
        return {.needs_frame = pending_};
    }

    PaintInvalidation PrepareGeometry(huxerui::ViewNode& node,
                                       huxerui::TextMeasurer&) override {
        if (selected_index_ >= node.ChildCount()) {
            pending_ = false;
            return PaintInvalidation::None;
        }
        const auto metrics = scroll_.Metrics();
        if (!scroll_.IsConnected() || metrics.viewport_extent <= 0.0F) {
            return PaintInvalidation::None;
        }
        const huxerui::ViewNode& selected = node.ChildAt(selected_index_);
        const Geometry geometry{selected.LayoutOffset().x,
                                 selected.LayoutSize().width,
                                 metrics.viewport_extent,
                                 metrics.content_extent};
        if (!pending_ && geometry_ == geometry) return PaintInvalidation::None;
        geometry_ = geometry;
        pending_ = false;
        constexpr float kRevealMargin = 8.0F;
        const float start = geometry.start;
        const float end = start + geometry.width;
        float target = metrics.offset;
        if (geometry.width + 2.0F * kRevealMargin >= metrics.viewport_extent) {
            target = start;
        } else if (start < target + kRevealMargin) {
            target = start - kRevealMargin;
        } else if (end > target + metrics.viewport_extent - kRevealMargin) {
            target = end - metrics.viewport_extent + kRevealMargin;
        }
        target = std::clamp(target, 0.0F, metrics.maximum_offset);
        if (target != metrics.offset) pending_ = !scroll_.ScrollTo(target);
        return PaintInvalidation::None;
    }

private:
    struct Geometry {
        float start = 0.0F;
        float width = 0.0F;
        float viewport_extent = 0.0F;
        float content_extent = 0.0F;
        bool operator==(const Geometry&) const = default;
    };

    huxerui::ScrollController scroll_;
    std::string selected_key_;
    std::size_t selected_index_ = static_cast<std::size_t>(-1);
    Geometry geometry_{};
    bool pending_ = true;
};

} // namespace

// ---- 二级分区标签页（SectionTabBar + 手势切换）----

// 页内分区滑动的判定阈值：规则与「认领必须早于 Pager」的不变量见 section_swipe.h。
constexpr SectionSwipeRules kSectionSwipeRules{};

// 分区标签的左右滑动切换处理器：使用 SectionTabBar 的页面共用同一套手势阈值
// （判定规则与「认领必须早于 Pager」的不变量见 section_swipe.h）。拦截必须挂在
// 内容滚动节点本身（PointerIntercept 按深度优先注册，挂在滚动节点上才能先于
// 它自己的纵向滚动识别器运行）。手势状态（按下点、会话认领）由页面持有一对，
// 避免 hooks 出现在循环里。
std::function<bool(const huxerui::PointerEvent&)> SectionTabSwipeHandler(
    huxerui::State<huxerui::Point> origin, huxerui::State<bool> owned,
    std::function<void()> onPrev, std::function<void()> onNext) {
    return [origin, owned, onPrev = std::move(onPrev),
            onNext = std::move(onNext)](const huxerui::PointerEvent& event) {
        const float dx = event.position.x - origin.Get().x;
        const float dy = event.position.y - origin.Get().y;
        switch (event.type) {
        case huxerui::PointerEventType::Down:
            origin = event.position;
            owned = false;
            return false;
        case huxerui::PointerEventType::Move: {
            const SectionSwipeAction action =
                SectionSwipeOnMove(kSectionSwipeRules, dx, dy, owned.Get(),
                                   static_cast<bool>(onPrev),
                                   static_cast<bool>(onNext));
            if (action != SectionSwipeAction::Claim) return false;
            owned = true;
            return true;
        }
        case huxerui::PointerEventType::Up: {
            const SectionSwipeAction action =
                SectionSwipeOnUp(kSectionSwipeRules, dx, owned.Get());
            owned = false;
            if (action == SectionSwipeAction::CommitNext && onNext) onNext();
            if (action == SectionSwipeAction::CommitPrev && onPrev) onPrev();
            return false;
        }
        case huxerui::PointerEventType::Cancel:
            owned = false;
            return false;
        }
        return false;
    };
}

// 二级分区标签栏（全项目统一的页内分区切换样式，沿用代理页分组标签）：
// 横向纯文本标签，无外框、无填充——非选中为次级文字色，选中为主色文字 +
// 底部 2pt 主色短线；未选中画同高透明线，切换时布局零跳动。"选择中"
// （hover/press）只叠普通按钮那层填充，与选中态互不混淆。标签条本身横向可
// 滚动；**只在标签条溢出时**尾部出现下箭头入口，点开是全部标签的菜单（见
// 下方说明）。key 参与选中匹配与节点 Key，label 是展示文本。
[[huxerui::composable]] huxerui::View SectionTabBar(
    const std::vector<SectionTab>& tabs, const std::string& selectedKey,
    std::function<void(const std::string&)> onSelect) {
    constexpr float kTabIndicatorHeight = 2.0F;
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto scroll = huxerui::UseScrollController();
    auto menu = huxerui::UseMenu();
    huxerui::Color hoverFill = theme.colors.on_surface;
    hoverFill.alpha = 0.08F;
    huxerui::Color pressFill = theme.colors.on_surface;
    pressFill.alpha = 0.14F;
    const huxerui::Indication indication{
        .hover = huxerui::IndicationLayer{
            .fill = huxerui::VisualFill{huxerui::Brush{hoverFill}}},
        .press = huxerui::IndicationLayer{
            .fill = huxerui::VisualFill{huxerui::Brush{pressFill}}},
    };

    std::vector<huxerui::View> items;
    items.reserve(tabs.size());
    std::size_t selectedIndex = tabs.size();
    for (const SectionTab& tab : tabs) {
        const bool active = tab.key == selectedKey;
        if (active) selectedIndex = items.size();
        const huxerui::Color labelColor =
            active ? theme.colors.primary : theme.colors.on_surface_variant;
        huxerui::View labelView =
            huxerui::Text(tab.label, huxerui::TextRole::Label)
                .With(huxerui::Foreground(labelColor));
        if (!tab.badge.empty()) {
            // 角标用语义警示色，和 label 同字号但更小的心智权重：它只是提示
            // 「这个分区有降级信息」，明细在设置页「配置保真度」。
            labelView = huxerui::Row {
                labelView,
                huxerui::Text(tab.badge, huxerui::TextRole::Label)
                    .With(huxerui::Foreground(SemanticWarningColor(theme))),
            }.With(huxerui::Spacing(3.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
        }
        items.push_back(
            huxerui::Column {
                labelView,
                // 两态同高的短线：选中显示主色，未选中透明占位，无布局跳动。
                huxerui::Row{}.With(
                    huxerui::Frame{.height = kTabIndicatorHeight},
                    active ? huxerui::Background(theme.colors.primary)
                           : huxerui::Background(huxerui::Color::Transparent()),
                    huxerui::CornerRadius(theme.shapes.full)),
            }
                .With(huxerui::Spacing(3.0F),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
                .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(8.0F, 4.0F)),
                      // 未选中用透明描边（宽度恒为 1pt）保住几何，避免切换时
                      // 整项尺寸跳动；本样式不画框，描边始终透明。
                      huxerui::Border(huxerui::Color::Transparent(), 1.0F),
                      huxerui::CornerRadius(theme.shapes.small),
                      indication,
                      huxerui::Focusable(true),
                      huxerui::Semantics{.role = huxerui::SemanticRole::Tab,
                                         .label = tab.label,
                                         .selected = active})
                .OnClick([onSelect, key = tab.key] { onSelect(key); })
                .Key("section-tab-" + tab.key));
    }
    huxerui::View strip =
        huxerui::ScrollView(
            huxerui::Row(items)
                .With(huxerui::Spacing(theme.spacing.extra_small),
                      huxerui::CrossAlign(
                          huxerui::CrossAxisAlignment::Center),
                      RevealSectionTab{scroll, selectedKey, selectedIndex}))
            .ScrollAxis(huxerui::Axis::Horizontal)
            .Controller(scroll)
            .With(huxerui::ClipChildren())
            .Key("section-tab-scroll");

    // 尾部"无尾巴下箭头"：**只在标签条真的溢出时**才出现（不溢出就不该有入口）。
    // 溢出判定用滚动控制器的 MaxOffset——内容比视口宽才会有可滚动余量。这里读
    // MaxOffset 会把本组件订阅到标签条的滚动几何上，代价只限这一条标签栏子树，
    // 而且只在标签条自身滚动时触发（真机 profiler 未见此项开销）。
    // 箭头旋转只作用于字形；外层保持普通点击区域和辅助功能语义。
    std::vector<huxerui::MenuEntry> pickerEntries;
    pickerEntries.reserve(tabs.size());
    for (const SectionTab& tab : tabs) {
        const std::string label = huxerui::UseString(tab.label);
        pickerEntries.emplace_back(huxerui::MenuItem(
            tab.key == selectedKey ? "✓ " + label : label,
            [onSelect, key = tab.key] { onSelect(key); }));
    }
    huxerui::View picker =
        huxerui::Row {
          huxerui::Text("›")
              .Style(huxerui::TextStyle{
                  huxerui::Font::System(font_size::kBody), theme.colors.on_surface_variant})
              .With(huxerui::Rotation(90.0F)),
        }
            .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(6.0F, 2.0F)),
                  huxerui::CornerRadius(theme.shapes.small), indication,
                  huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                     .label = huxerui::UseString(
                                         Localized("选择标签"))},
                  huxerui::Focusable(true), huxerui::Enabled(true), menu.Anchor())
            .OnClick([menu, entries = std::move(pickerEntries)]() mutable {
                menu.Show(std::move(entries));
            })
            .Key("section-tab-picker");
    // 始终保留同一 Row/ScrollView 结构；箭头出现/消失不会重挂载标签条，
    // 避免滚动连接重建后偏移归零，或在溢出判定附近反复切换布局。
    std::vector<huxerui::View> children{huxerui::View{strip}.With(huxerui::Grow(1.0F))};
    if (scroll.MaxOffset() > 1.0F) children.push_back(picker);
    return huxerui::Row(children).With(
        huxerui::Spacing(2.0F),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

// 只挂载当前代理组：Key 随分组变化，挂载后才推进进度，确保 AnimateTo 播放。
// 入场方向在挂载时冻结，后续数据刷新和快速反向切换不会翻转正在播放的轨道。
[[huxerui::composable]] huxerui::View ProxyGroupPage(huxerui::View content, int direction) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto entryDirection = huxerui::UseState(direction);
    auto progress = huxerui::UseState(direction == 0 ? 1.0F : 0.0F);
    huxerui::Lifecycle([progress] {
        progress = 1.0F;
        return [] {};
    }, 0);
    return huxerui::View{content}.With(
        huxerui::Grow(1.0F),
        huxerui::Transition{huxerui::AnimateTo(
            progress.Get(),
            huxerui::TweenSpec{
                .duration = theme.motion.reduced_motion ? 0.0 : theme.motion.normal,
                .easing = huxerui::Easing::EaseOut})}
            .Offset({48.0F * static_cast<float>(entryDirection.Get()), 0.0F}, {}));
}

} // namespace clashflux::ui
