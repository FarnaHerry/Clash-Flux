// 使用生产标签栏与内容 Pager，在无窗口 Runtime 中验证跟手翻页、嵌套边界和实际几何。
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include <huxerui/huxerui.h>
#include <huxerui/testing/ui_test.h>

#include "ui.h"

namespace {
using huxerui::testing::UiSelector;
constexpr auto kStep = std::chrono::milliseconds(100);
constexpr auto kSettle = std::chrono::milliseconds(400);
int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

std::string key(int index) { return "group-" + std::to_string(index); }
std::string label(int index) { return "Group " + std::to_string(index) + " nodes"; }

[[huxerui::composable]] huxerui::View TestContent() {
    auto selected = huxerui::UseState(0);
    auto fewer = huxerui::UseState(false);
    const int index = selected.Get();
    const int count = fewer.Get() ? 1 : 12;
    const auto select = [selected](int next) {
        if (selected.Get() == next) return;
        selected = next;
    };
    std::vector<clashflux::ui::SectionTab> tabs;
    for (int i = 0; i < count; ++i) {
        tabs.push_back({key(i), label(i), i == 11 ? "!" : ""});
    }
    std::vector<huxerui::View> pages;
    for (int i = 0; i < count; ++i) {
        pages.push_back(huxerui::Column {
          huxerui::ScrollView(huxerui::Column {
            huxerui::Text("PAGE-" + std::to_string(i)),
            huxerui::Row {}.With(huxerui::Frame{.height = 1400.0F}),
          }).With(huxerui::Grow(1.0F)).Key("page-content-" + std::to_string(i)),
        }.With(huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
         .Key(key(i)));
    }
    return huxerui::Column {
      huxerui::Row {
        huxerui::Button("First").OnClick([select] { select(0); }),
        huxerui::Button("Last").OnClick([select] { select(11); }),
        huxerui::Button("Fewer").OnClick([select, fewer] {
            select(0);
            fewer = true;
        }),
      },
      clashflux::ui::SectionTabBar(tabs, key(index), [select](const std::string& name) {
          select(std::stoi(name.substr(6)));
      }),
      huxerui::Text("SELECTED-" + std::to_string(index)),
      clashflux::ui::SectionTabPages(pages, static_cast<std::size_t>(index),
          [select](std::size_t next) { select(static_cast<int>(next)); }),
    }.With(huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

constexpr float kPageInset = 16.0F;

class AppResourceProvider final : public huxerui::PlatformResources {
public:
    huxerui::ResourceConfiguration Configuration() const override {
        return {};
    }

    std::optional<huxerui::InputStream> OpenRead(
        std::string_view packagePath) override {
        const huxerui::File file =
            huxerui::File(CLASHFLUX_TEST_RESOURCE_PACKAGE).Resolve(packagePath);
        auto result = file.OpenRead();
        if (!result.Succeeded()) return std::nullopt;
        return std::move(result).Value();
    }
};

std::shared_ptr<AppResourceProvider> AppResources() {
    static const auto provider = std::make_shared<AppResourceProvider>();
    return provider;
}

huxerui::View TestRoot() {
    return huxerui::FlatTheme {
      huxerui::Column {TestContent()}.With(huxerui::Padding(kPageInset), huxerui::Grow(1.0F)),
    };
}
huxerui::View ReducedRoot() {
    huxerui::ThemeSpec theme = huxerui::FlatLightThemeSpec();
    theme.motion.reduced_motion = true;
    return huxerui::FlatTheme(theme,
        huxerui::Column {TestContent()}.With(huxerui::Padding(kPageInset), huxerui::Grow(1.0F)));
}
// 生产页面同样由虚拟网格声明每个组：大量节点不能被 Pager 测成无界高度。
int constructed[12]{};
std::size_t greatestItem[12]{};
[[huxerui::composable]] huxerui::View LargePages() {
    auto selected = huxerui::UseState<std::size_t>(0);
    std::vector<huxerui::View> pages;
    for (std::size_t page = 0; page < 12; ++page) {
        pages.push_back(huxerui::Column {
          huxerui::VirtualGrid(10000, [page](std::size_t item) {
            ++constructed[page];
            greatestItem[page] = std::max(greatestItem[page], item);
            return huxerui::Text(std::to_string(item))
                .With(huxerui::Frame{.height = 40.0F}).Key(item);
          }).Columns(huxerui::GridColumns::Fixed(2)).RowExtent(40.0F)
            .With(huxerui::Grow(1.0F)).Key("large-grid-" + std::to_string(page)),
        }.With(huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
         .Key(page));
    }
    return huxerui::Column {
      huxerui::Button("Large last").OnClick([selected] { selected = 11; }),
      clashflux::ui::SectionTabPages(pages, selected.Get(), [selected](std::size_t next) { selected = next; }),
    }.With(huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}
[[huxerui::composable]] huxerui::View NestedContent() {
    auto outer = huxerui::UseState<std::size_t>(1);
    return huxerui::Column {
      huxerui::Button("Reset outer").OnClick([outer] { outer = 1; }),
      huxerui::Text("OUTER-" + std::to_string(outer.Get())),
      huxerui::Pager({huxerui::Column {}, TestContent(), huxerui::Column {}}, outer.Get())
          .OnChanged([outer](std::size_t next) { outer = next; })
          .With(huxerui::Grow(1.0F)),
    }.With(huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}
huxerui::View NestedRoot() { return huxerui::FlatTheme {NestedContent()}; }
huxerui::View LargeRoot() { return huxerui::FlatTheme {LargePages()}; }
const huxerui::Application kNestedApplication{NestedRoot};
const huxerui::Application kLargeApplication{LargeRoot};
const huxerui::Application kApplication{TestRoot};
const huxerui::Application kReducedApplication{ReducedRoot};

void checkVisible(huxerui::testing::UiTest& ui, int index, const char* what) {
    const auto tab = ui.Find(UiSelector::Key("section-tab-" + key(index))).One();
    const auto strip = ui.Find(UiSelector::Key("section-tab-scroll")).One();
    const auto picker = ui.Find(UiSelector::Key("section-tab-picker"));
    const float right = picker.Exists() ? picker.One().bounds.x : strip.bounds.x + strip.bounds.width;
    check(tab.bounds.x >= strip.bounds.x - 0.5F &&
          tab.bounds.x + tab.bounds.width <= right + 0.5F, what);
}

// UiSnapshot 公开导出绘制意图；标签栏前景中只能有一条 2pt 指示线。
// 检查绘制几何才能发现 MotionController 没有使缓存失效的“状态动了、画面没动”。
huxerui::Rect indicator(huxerui::testing::UiTest& ui) {
    const auto snapshot = ui.CaptureSnapshot().ToString();
    static const std::regex line{
        R"(foreground\s+\{\s+rect\s+\{\s+bounds\s+\{\s+x=([\d.-]+)\s+y=([\d.-]+)\s+width=([\d.-]+)\s+height=2\.000000\s+\})"};
    const std::sregex_iterator begin(snapshot.begin(), snapshot.end(), line), end;
    check(std::distance(begin, end) == 1, "整个标签栏仅绘制一条选中指示线");
    if (begin == end) return {};
    return {std::stof((*begin)[1].str()), std::stof((*begin)[2].str()), std::stof((*begin)[3].str()), 2.0F};
}

void checkEntry(huxerui::testing::UiTest& ui, int index, int expectedDirection) {
    auto content = ui.Find(UiSelector::Key("page-content-" + std::to_string(index)));
    ui.Pump(); // 受控索引已经落帧，推进框架保留的分页动画。
    const float start = content.One().bounds.x - kPageInset;
    check(expectedDirection * start > 4.0F, "目标页从正确的左右方向滑入");
    ui.Pump(kStep);
    const float moving = content.One().bounds.x - kPageInset;
    check(expectedDirection * moving >= 0.0F && std::abs(moving) < std::abs(start),
          "横向过渡逐步收敛，过程中不反向跳动");
    ui.Pump(kSettle);
    check(std::abs(content.One().bounds.x - kPageInset) <= 0.5F, "滑入结束回到零偏移");
}
} // namespace

int main() {
    {
        huxerui::testing::UiTest ui(kApplication, {
            .viewport = {400.0F, 700.0F},
            .resource_provider = AppResources(),
        });
        ui.PumpAndSettle();
        check(std::abs(ui.Find(UiSelector::Key("page-content-0")).One().bounds.x - kPageInset) <= 0.5F,
              "首次显示不制造无方向的入场运动");
        checkVisible(ui, 0, "初始标签可见");
        check(ui.Find(UiSelector::Key("section-tab-picker")).Exists(), "溢出时提供标签菜单");

        const auto initialIndicator = indicator(ui);
        // 关闭后重新打开仍须保有完整选项，不能在第一次 Show 时移空事件闭包。
        ui.Find(UiSelector::Key("section-tab-picker")).Tap();
        ui.PumpAndSettle();
        ui.PressKey(huxerui::Key::Escape);
        ui.PumpAndSettle();
        ui.Find(UiSelector::Key("section-tab-picker")).Tap();
        ui.PumpAndSettle();
        check(ui.FindSemantics(huxerui::testing::UiSemanticSelector::Role(huxerui::SemanticRole::MenuItem)).Count() == 12,
              "标签菜单关闭再打开仍包含全部选项");
        ui.PressKey(huxerui::Key::Escape);
        ui.PumpAndSettle();

        // 点击标签：前后方向不同，选择不需要等待模型泵。
        ui.Find(UiSelector::Key("section-tab-group-1")).Tap();
        check(ui.Find(UiSelector::Text("SELECTED-1")).Exists(), "点击立即显示选中组");
        const auto indicatorStart = indicator(ui);
        check(std::abs(indicatorStart.x - initialIndicator.x) < 0.5F, "选中变化时指示线从原位起步");
        ui.Pump(); // 建立 MotionController 的起始帧，再推进虚拟时间。
        ui.Pump(std::chrono::milliseconds(40));
        const auto indicatorMoving = indicator(ui);
        check(indicatorMoving.x > indicatorStart.x + 1.0F, "指示线在动画中逐步移动并重绘");
        checkEntry(ui, 1, 1);
        check(indicator(ui).x > indicatorMoving.x + 1.0F, "指示线收敛到新标签而非瞬时显隐");
        ui.Find(UiSelector::Key("section-tab-group-0")).Tap();
        checkEntry(ui, 0, -1);

        // 菜单可以选择当前视口以外的标签；选中项自动揭示且含角标的整项不被裁切。
        ui.Find(UiSelector::Key("section-tab-picker")).Tap({.device_kind = huxerui::PointerDeviceKind::Mouse});
        ui.Pump(kSettle);
        bool menuItemTapped = false;
        for (const auto& item : ui.Find(UiSelector::Text(label(11))).All()) {
            if (!item.in_viewport) continue;
            ui.TapAt({item.bounds.x + item.bounds.width * 0.5F,
                      item.bounds.y + item.bounds.height * 0.5F});
            menuItemTapped = true;
            break;
        }
        check(menuItemTapped, "菜单包含可点击的目标分组");
        check(ui.Find(UiSelector::Text("SELECTED-11")).Exists(), "菜单选择立即切换内容");
        checkEntry(ui, 11, 1);
        checkVisible(ui, 11, "菜单选中末尾标签后自动滚到完整可见");

        // 真正的内容横滑使用框架 Pager，沿标签顺序返回并保持标签与内容同步。
        ui.Drag({100.0F, 210.0F}, {300.0F, 210.0F});
        ui.Pump();
        check(ui.Find(UiSelector::Text("SELECTED-10")).Exists(), "内容右滑切换到上一组");
        checkEntry(ui, 10, -1);
        checkVisible(ui, 10, "内容滑动后选中标签可见");
        ui.Drag({300.0F, 210.0F}, {100.0F, 210.0F});
        ui.Pump();
        check(ui.Find(UiSelector::Text("SELECTED-11")).Exists(), "内容左滑切换到下一组");
        checkEntry(ui, 11, 1);
        checkVisible(ui, 11, "滑回末尾标签仍完整可见");

        ui.Find(UiSelector::Key("section-tab-scroll")).ScrollBy({-300.0F, 0.0F});
        ui.PumpAndSettle();
        check(!ui.Find(UiSelector::Key("section-tab-group-11")).One().in_viewport,
              "手动滚动允许浏览其他标签，不会立即被拉回选中项");
        ui.Find(UiSelector::Text("First")).Tap();
        checkEntry(ui, 0, -1);
        checkVisible(ui, 0, "从末尾切回首项时标签栏滚回开头");

        // 手机触摸打开菜单，并通过真实辅助功能动作选择，确保两条输入路径共用逻辑。
        ui.Find(UiSelector::Key("section-tab-picker")).Tap();
        ui.Pump(kSettle);
        ui.FindSemantics(huxerui::testing::UiSemanticSelector::AllOf(
            huxerui::testing::UiSemanticSelector::Role(huxerui::SemanticRole::MenuItem),
            huxerui::testing::UiSemanticSelector::Label(label(6))))
            .PerformSemanticAction({huxerui::SemanticActionKind::Activate, {}});
        checkEntry(ui, 6, 1);
        checkVisible(ui, 6, "触摸菜单和辅助功能选择同样滚到目标标签");
        ui.Find(UiSelector::Text("First")).Tap();
        checkEntry(ui, 0, -1);

        // 在过渡完成前反向切换，最终意图与方向仍正确。
        ui.Find(UiSelector::Text("Last")).Tap();
        ui.Pump(kStep);
        ui.Find(UiSelector::Text("First")).Tap();
        checkEntry(ui, 0, -1);
        checkVisible(ui, 0, "快速反向切换后首项可见");

        ui.Find(UiSelector::Text("Last")).Tap();
        ui.PumpAndSettle();
        ui.SetWindowMetrics({.viewport = {260.0F, 700.0F}});
        ui.PumpAndSettle();
        checkVisible(ui, 11, "缩窄窗口后重新保证选中标签可见");
        ui.Find(UiSelector::Text("Fewer")).Tap();
        ui.PumpAndSettle();
        checkVisible(ui, 0, "标签数量减少后滚动偏移正确回落");
        check(!ui.Find(UiSelector::Key("section-tab-picker")).Exists(), "不溢出时隐藏菜单入口");
    }
    {
        huxerui::testing::UiTest ui(kReducedApplication, {
            .viewport = {400.0F, 700.0F},
            .resource_provider = AppResources(),
        });
        ui.PumpAndSettle();
        ui.Find(UiSelector::Text("Last")).Tap();
        ui.PumpAndSettle();
        check(std::abs(ui.Find(UiSelector::Key("page-content-11")).One().bounds.x - kPageInset) <= 0.5F,
              "减少动态效果时直接落到目标位置");
        checkVisible(ui, 11, "减少动态效果仍保证选中标签可见");
        const auto settledIndicator = indicator(ui);
        ui.Pump(kStep);
        check(indicator(ui) == settledIndicator, "减少动态效果时指示线直接定位、不再运行动画");
    }
    {
        huxerui::testing::UiTest ui(kApplication, {.viewport = {400.0F, 700.0F}, .resource_provider = AppResources()});
        ui.PumpAndSettle();
        const auto content = ui.Find(UiSelector::Key("page-content-0"));
        const auto tab = ui.Find(UiSelector::Key("section-tab-group-0")).One();
        huxerui::PointerEvent event{.type = huxerui::PointerEventType::Down,
            .pointer_id = 7, .position = {300.0F, 210.0F},
            .device_kind = huxerui::PointerDeviceKind::Touch,
            .changed_button = huxerui::PointerButton::Primary, .pressed_buttons = huxerui::PointerButton::Primary};
        ui.SendPointer(event);
        event.type = huxerui::PointerEventType::Move;
        event.position.x = 240.0F;
        ui.SendPointer(event);
        ui.Pump();
        check(content.One().bounds.x < kPageInset - 40.0F, "按住拖动时内容跟手移动");
        check(ui.Find(UiSelector::Key("section-tab-group-0")).One().bounds == tab.bounds,
              "拖动只移动内容，标签栏固定");
        event.type = huxerui::PointerEventType::Cancel;
        event.pressed_buttons = huxerui::PointerButton::None;
        ui.SendPointer(event);
        ui.PumpAndSettle();
        check(ui.Find(UiSelector::Text("SELECTED-0")).Exists(), "取消拖动不会提交新标签");
        check(std::abs(content.One().bounds.x - kPageInset) < 0.5F, "取消拖动回弹到原页");

        content.ScrollBy({0.0F, 180.0F});
        ui.PumpAndSettle();
        const float scrolledY = ui.Find(UiSelector::Text("PAGE-0")).One().bounds.y;
        ui.Find(UiSelector::Key("section-tab-group-1")).Tap();
        ui.PumpAndSettle();
        ui.Find(UiSelector::Key("section-tab-group-0")).Tap();
        ui.PumpAndSettle();
        check(std::abs(ui.Find(UiSelector::Text("PAGE-0")).One().bounds.y - scrolledY) < 0.5F,
              "切回原分区后保留滚动位置");
    }
    {
        huxerui::testing::UiTest ui(kNestedApplication, {.viewport = {400.0F, 700.0F}, .resource_provider = AppResources()});
        ui.PumpAndSettle();
        ui.Find(UiSelector::Key("section-tab-group-1")).Tap();
        ui.PumpAndSettle();
        ui.Drag({300.0F, 280.0F}, {100.0F, 280.0F});
        ui.PumpAndSettle();
        check(ui.Find(UiSelector::Text("SELECTED-2")).Exists(), "嵌套横滑优先切内层分区");
        check(ui.Find(UiSelector::Text("OUTER-1")).Exists(), "内层可翻页时外层不切换");
        ui.Find(UiSelector::Text("First")).Tap();
        ui.PumpAndSettle();
        ui.Drag({100.0F, 280.0F}, {300.0F, 280.0F});
        ui.PumpAndSettle();
        check(ui.Find(UiSelector::Text("OUTER-0")).Exists(), "首个分区继续右滑交给外层 Pager");
        ui.Find(UiSelector::Text("Reset outer")).Tap();
        ui.PumpAndSettle();
        ui.Find(UiSelector::Text("Last")).Tap();
        ui.PumpAndSettle();
        ui.Drag({300.0F, 280.0F}, {100.0F, 280.0F});
        ui.PumpAndSettle();
        check(ui.Find(UiSelector::Text("OUTER-2")).Exists(), "末尾分区继续左滑交给外层 Pager");
    }
    {
        huxerui::testing::UiTest ui(kLargeApplication, {.viewport = {400.0F, 700.0F}});
        ui.PumpAndSettle();
        check(constructed[0] > 0 && greatestItem[0] < 100, "一万节点的当前组仅构造视口附近卡片");
        for (int i = 1; i < 12; ++i) check(constructed[i] == 0, "初始隐藏组不构造节点卡片");
        ui.Find(UiSelector::Text("Large last")).Tap();
        ui.PumpAndSettle();
        check(constructed[11] > 0 && greatestItem[11] < 100, "跳到末尾组仍保持有界虚拟化");
        for (int i = 1; i < 11; ++i) check(constructed[i] == 0, "跨组跳转不构造中间组的节点");
    }
    std::printf("test_page_transition: %s\n", failures == 0 ? "ok" : "failed");
    return failures == 0 ? 0 : 1;
}
