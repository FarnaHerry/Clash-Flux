// 页内分区共用的标签栏、滑动与方向过渡；独立编译供无窗口交互测试使用。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <memory>
#include <numbers>
#include <optional>
#include <functional>
#include <string>
#include <vector>

#include "ui.h"

namespace clashflux::ui {

class SectionTabMotion {
public:
    struct Page {
        std::size_t index;
        float offset;
    };
    struct Position {
        std::size_t left;
        std::size_t right;
        float progress;
    };
    huxerui::Rect viewport{};
    std::vector<Page> pages;
    std::weak_ptr<std::function<void()>> repaint;
    bool connected = false;

    std::optional<Position> PresentedPosition() const {
        if (!connected || pages.empty()) return std::nullopt;
        const auto [left, right] = std::minmax_element(pages.begin(), pages.end(),
            [](const Page& a, const Page& b) { return a.offset < b.offset; });
        const float distance = right->offset - left->offset;
        if (distance < 0.001F) {
            if (std::abs(left->offset) > 0.001F) return std::nullopt;
            return Position{left->index, left->index, 0.0F};
        }
        return Position{left->index, right->index,
            std::clamp(-left->offset / distance, 0.0F, 1.0F)};
    }

    void InvalidateIndicator() const {
        if (const auto callback = repaint.lock()) (*callback)();
    }
};

SectionTabMotionHandle UseSectionTabMotion() {
    return huxerui::UseState(std::make_shared<SectionTabMotion>()).Get();
}

namespace {

// Pager 的最终呈现几何已经包含拖动、提交、反向和取消回弹；只观察它，
// 不截获指针，也不另外积分手势。保留通道只有值，没有跨帧节点指针。
struct SectionPagerGeometry {
    class Extension;
    SectionTabMotionHandle motion;
    bool operator==(const SectionPagerGeometry&) const = default;
};

class SectionPagerGeometry::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode& node, const SectionPagerGeometry& spec) { Update(node, spec); }
    ~Extension() override { Disconnect(); }
    void Update(huxerui::ViewNode&, const SectionPagerGeometry& spec) {
        if (motion_ != spec.motion) {
            Disconnect();
            motion_ = spec.motion;
        }
        motion_->connected = true;
    }
    PaintInvalidation PrepareGeometry(huxerui::ViewNode& node, huxerui::TextMeasurer&) override {
        motion_->viewport = node.PresentationBounds();
        motion_->pages.clear();
        // 几何回调先父后子；页样本随后收齐，绘制发生在全部几何回调之后。
        motion_->InvalidateIndicator();
        return PaintInvalidation::None;
    }
private:
    void Disconnect() {
        if (!motion_) return;
        motion_->connected = false;
        motion_->pages.clear();
        motion_->InvalidateIndicator();
    }
    SectionTabMotionHandle motion_;
};

struct SectionPageGeometry {
    class Extension;
    SectionTabMotionHandle motion;
    std::size_t index;
    bool operator==(const SectionPageGeometry&) const = default;
};

class SectionPageGeometry::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode& node, const SectionPageGeometry& spec) { Update(node, spec); }
    void Update(huxerui::ViewNode&, const SectionPageGeometry& spec) {
        motion_ = spec.motion;
        index_ = spec.index;
    }
    PaintInvalidation PrepareGeometry(huxerui::ViewNode& node, huxerui::TextMeasurer&) override {
        const auto viewport = motion_->viewport;
        if (viewport.width > 0.0F) {
            motion_->pages.push_back({index_, (node.PresentationBounds().x - viewport.x) / viewport.width});
        }
        return PaintInvalidation::None;
    }
private:
    SectionTabMotionHandle motion_;
    std::size_t index_ = 0;
};

// ScrollView 没有按普通子节点 reveal 的 API；在挂载后的实际布局中定位选中标签。
// 只保存数值几何，不跨重组保留 ViewNode/child 指针。手动滚动不改变选中项，
// 因此不会被持续拉回；换标签、标签尺寸变化或窗口缩放才重新居中。
// 内容横滑期间在两端标签的居中偏移之间跟手插值，首尾受滚动边界限制。
struct RevealSectionTab {
    class Extension;
    huxerui::ScrollController scroll;
    std::string selected_key;
    std::size_t selected_index;
    huxerui::Color indicator_color;
    double indicator_duration;
    SectionTabMotionHandle motion;
    bool operator==(const RevealSectionTab&) const = default;
};

class RevealSectionTab::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode& node, const RevealSectionTab& spec)
        : scroll_(spec.scroll) {
        repaint_ = std::make_shared<std::function<void()>>([this] { InvalidatePaint(); });
        Update(node, spec);
    }

    void Update(huxerui::ViewNode&, const RevealSectionTab& spec) {
        pending_ = pending_ || selected_key_ != spec.selected_key ||
                   selected_index_ != spec.selected_index || scroll_ != spec.scroll;
        if (color_ != spec.indicator_color) InvalidatePaint();
        color_ = spec.indicator_color;
        duration_ = spec.indicator_duration;
        if (motion_ != spec.motion && motion_) motion_->repaint.reset();
        motion_ = spec.motion;
        if (motion_) motion_->repaint = repaint_;
        scroll_ = spec.scroll;
        selected_key_ = spec.selected_key;
        selected_index_ = spec.selected_index;
    }

    FrameResult OnFrame(huxerui::ViewNode&, const huxerui::FrameInfo& frame) override {
        if (motion_ && motion_->connected) {
            // 在帧更新阶段消费上一轮完整的呈现样本；不在绘制中发滚动请求，
            // 也不逐帧写组合 State 或让 Pager 内容重新布局。
            const auto position = motion_->PresentedPosition();
            if (!position) return {.needs_frame = pending_};
            const float value = std::lerp(static_cast<float>(position->left),
                static_cast<float>(position->right), position->progress);
            const bool changed = pager_position_initialized_ &&
                std::abs(value - pager_position_) > 0.00001F;
            pager_position_ = value;
            pager_position_initialized_ = true;
            if (changed) {
                if (const auto target = CenteredOffset(*position)) scroll_.ScrollTo(*target);
            }
            const bool moving = position->left != position->right &&
                position->progress > 0.0F && position->progress < 1.0F;
            return {.needs_frame = pending_ || moving};
        }
        pager_position_initialized_ = false;
        const auto x = indicator_x_.Advance(frame);
        const auto width = indicator_width_.Advance(frame);
        if (x.changed || width.changed) InvalidatePaint();
        return {.needs_frame = pending_ || x.needs_frame || width.needs_frame};
    }

    void PaintAboveContent(const huxerui::ViewNode&, huxerui::PaintContext& paint) const override {
        if (!indicator_visible_) return;
        paint.DrawRect(IndicatorBounds(),
                       huxerui::Brush{color_}, huxerui::CornerRadii{1.0F});
    }

    PaintInvalidation PrepareGeometry(huxerui::ViewNode& node,
                                       huxerui::TextMeasurer&) override {
        if (selected_index_ >= node.ChildCount()) {
            pending_ = false;
            indicator_visible_ = false;
            indicator_initialized_ = false;
            return PaintInvalidation::Foreground;
        }
        const auto metrics = scroll_.Metrics();
        if (!scroll_.IsConnected() || metrics.viewport_extent <= 0.0F) {
            return PaintInvalidation::None;
        }
        tab_bounds_.clear();
        tab_bounds_.reserve(node.ChildCount());
        for (std::size_t index = 0; index < node.ChildCount(); ++index) {
            const auto& tab = node.ChildAt(index);
            const auto& line = tab.ChildAt(1);
            tab_bounds_.push_back({tab.LayoutOffset().x + line.LayoutOffset().x,
                                   tab.LayoutOffset().y + line.LayoutOffset().y,
                                   line.LayoutSize().width, 2.0F});
        }
        const huxerui::ViewNode& selected = node.ChildAt(selected_index_);
        const Geometry geometry{selected.LayoutOffset().x,
                                 selected.LayoutSize().width,
                                 metrics.viewport_extent,
                                 metrics.content_extent};
        // 当前标签结构由本组件拥有，第二个子节点是透明的指示线占位。
        // 一条保留的指示线在实际布局间补间，不能为每个标签显隐一条线。
        const auto& line = selected.ChildAt(1);
        const float x = selected.LayoutOffset().x + line.LayoutOffset().x;
        const float width = line.LayoutSize().width;
        const float y = selected.LayoutOffset().y + line.LayoutOffset().y;
        const bool indicator_changed = !indicator_initialized_ ||
            x != indicator_x_.Target() || width != indicator_width_.Target() || y != indicator_y_;
        const bool snap_running = duration_ == 0.0 &&
            (indicator_x_.IsRunning() || indicator_width_.IsRunning());
        if (indicator_changed || snap_running) {
            if (indicator_initialized_ && pending_ && duration_ > 0.0 && !(motion_ && motion_->connected)) {
                const huxerui::TweenSpec tween{.duration = duration_, .easing = huxerui::Easing::EaseOut};
                indicator_x_.AnimateTo(x, tween);
                indicator_width_.AnimateTo(width, tween);
            } else {
                indicator_x_.Set(x);
                indicator_width_.Set(width);
            }
            indicator_y_ = y;
            indicator_visible_ = true;
            indicator_initialized_ = true;
        }
        const auto invalidation = indicator_changed || snap_running
            ? PaintInvalidation::Foreground : PaintInvalidation::None;
        if (!pending_ && geometry_ == geometry) return invalidation;
        geometry_ = geometry;
        pending_ = false;
        float target = std::clamp(geometry.start + geometry.width * 0.5F -
            metrics.viewport_extent * 0.5F, 0.0F, metrics.maximum_offset);
        // 点击/菜单可能先更新受控索引，而 Pager 还在原页。此时继续使用
        // 实际呈现位置，避免标签条先跳到目标，再被跟手轨道拉回。
        if (motion_) {
            if (const auto position = motion_->PresentedPosition()) {
                if (const auto centered = CenteredOffset(*position)) target = *centered;
            }
        }
        if (target != metrics.offset) pending_ = !scroll_.ScrollTo(target);
        return invalidation;
    }

private:
    std::optional<float> CenteredOffset(const SectionTabMotion::Position& position) const {
        if (!scroll_.IsConnected() || position.left >= tab_bounds_.size() ||
            position.right >= tab_bounds_.size()) return std::nullopt;
        const auto metrics = scroll_.Metrics();
        if (metrics.viewport_extent <= 0.0F) return std::nullopt;
        const auto center = [&](std::size_t index) {
            const auto& bounds = tab_bounds_[index];
            return std::clamp(bounds.x + bounds.width * 0.5F -
                metrics.viewport_extent * 0.5F, 0.0F, metrics.maximum_offset);
        };
        // 与 Flutter TabBar 一样，先限制两端偏移再插值；边缘标签不会
        // 为了强行居中产生空白，也不会直到翻页完成才揭示下一个标签。
        return std::lerp(center(position.left), center(position.right), position.progress);
    }

    huxerui::Rect IndicatorBounds() const {
        const huxerui::Rect fallback{indicator_x_.Value(), indicator_y_, indicator_width_.Value(), 2.0F};
        if (!motion_ || !motion_->connected || motion_->pages.empty()) return fallback;
        const SectionTabMotion::Page* left = nullptr;
        const SectionTabMotion::Page* right = nullptr;
        for (const auto& page : motion_->pages) {
            if (page.index >= tab_bounds_.size()) continue;
            if (!left || page.offset < left->offset) left = &page;
            if (!right || page.offset > right->offset) right = &page;
        }
        if (!left || !right) return fallback;
        const auto start = tab_bounds_[left->index];
        if (left == right || right->offset - left->offset < 0.001F) return start;
        const auto end = tab_bounds_[right->index];
        const float t = std::clamp(-left->offset / (right->offset - left->offset), 0.0F, 1.0F);
        // 两端不同速度：前端先伸向目标，后端随后收拢。正反拖动使用同一
        // 几何映射，因此指针反向、未提交回弹和快速改选都不会重启动画。
        const float phase = t * std::numbers::pi_v<float> * 0.5F;
        const float trailing = duration_ == 0.0 ? t : 1.0F - std::cos(phase);
        const float leading = duration_ == 0.0 ? t : std::sin(phase);
        const bool forward = end.x > start.x;
        const float x = std::lerp(start.x, end.x, forward ? trailing : leading);
        const float edge = std::lerp(start.x + start.width, end.x + end.width, forward ? leading : trailing);
        return {x, std::lerp(start.y, end.y, t), std::max(0.0F, edge - x), 2.0F};
    }

    struct Geometry {
        float start = 0.0F;
        float width = 0.0F;
        float viewport_extent = 0.0F;
        float content_extent = 0.0F;
        bool operator==(const Geometry&) const = default;
    };

    SectionTabMotionHandle motion_;
    std::shared_ptr<std::function<void()>> repaint_;
    std::vector<huxerui::Rect> tab_bounds_;
    huxerui::ScrollController scroll_;
    std::string selected_key_;
    std::size_t selected_index_ = static_cast<std::size_t>(-1);
    Geometry geometry_{};
    bool pending_ = true;
    float pager_position_ = 0.0F;
    bool pager_position_initialized_ = false;
    huxerui::MotionController indicator_x_;
    huxerui::MotionController indicator_width_;
    huxerui::Color color_{};
    double duration_ = 0.0;
    float indicator_y_ = 0.0F;
    bool indicator_visible_ = false;
    bool indicator_initialized_ = false;
};

} // namespace

// ---- 二级分区标签页（SectionTabBar + 手势切换）----

// 二级分区标签栏（全项目统一的页内分区切换样式，沿用代理页分组标签）：
// 横向纯文本标签，无外框、无填充——非选中为次级文字色，选中为主色文字 +
// 底部 2pt 主色短线；指示线在标签之间连续滑动，透明占位保证布局不跳。"选择中"
// （hover/press）只叠普通按钮那层填充，与选中态互不混淆。标签条本身横向可
// 滚动；**只在标签条溢出时**尾部出现下箭头入口，点开是全部标签的菜单（见
// 下方说明）。key 参与选中匹配与节点 Key，label 是展示文本。
[[huxerui::composable]] huxerui::View SectionTabBar(
    const std::vector<SectionTab>& tabs, const std::string& selectedKey,
    std::function<void(const std::string&)> onSelect, SectionTabMotionHandle motion) {
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
                // 统一留出指示线高度；实际指示线由标签行的保留扩展绘制。
                huxerui::Row{}.With(
                    huxerui::Frame{.height = kTabIndicatorHeight},
                    huxerui::Background(huxerui::Color::Transparent()),
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
                      RevealSectionTab{scroll, selectedKey, selectedIndex, theme.colors.primary,
                          theme.motion.reduced_motion ? 0.0 : 0.16, motion}))
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
            .OnClick([menu, entries = std::move(pickerEntries)] {
                menu.Show(entries);
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

// 与 ACGU 首页相同：标签栏留在 Pager 外，内容跟手移动；受控索引变化由
// 框架处理完整的出入场、反向重定向、取消回弹、嵌套滚动边界与 reduced motion。
// 页根由调用方以语义 Key 标识，Pager 传递有界高度，虚拟列表只构造视口附近的条目。
huxerui::View SectionTabPages(std::vector<huxerui::View> pages, std::size_t selectedIndex,
                              std::function<void(std::size_t)> onSelect, SectionTabMotionHandle motion) {
    if (pages.empty()) return huxerui::Column {}.With(huxerui::Grow(1.0F));
    selectedIndex = std::min(selectedIndex, pages.size() - 1);
    if (motion) {
        for (std::size_t index = 0; index < pages.size(); ++index) {
            pages[index] = huxerui::View{pages[index]}.With(SectionPageGeometry{motion, index});
        }
    }
    huxerui::View pager = huxerui::Pager(pages, selectedIndex)
        .OnChanged([onSelect](std::size_t index) { if (onSelect) onSelect(index); })
        .With(huxerui::Grow(1.0F))
        .Key("section-tab-pages");
    if (motion) pager = huxerui::View{pager}.With(SectionPagerGeometry{motion});
    return pager;
}

} // namespace clashflux::ui
