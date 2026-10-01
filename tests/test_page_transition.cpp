// 使用生产标签栏、滑动处理器和代理页过渡，在无窗口 Runtime 中验证交互与实际几何。
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <optional>
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
    auto direction = huxerui::UseState(0);
    auto fewer = huxerui::UseState(false);
    auto origin = huxerui::UseState(huxerui::Point{});
    auto owned = huxerui::UseState(false);
    const int index = selected.Get();
    const int count = fewer.Get() ? 1 : 12;
    const auto select = [selected, direction](int next) {
        if (selected.Get() == next) return;
        direction = next > selected.Get() ? 1 : -1;
        selected = next;
    };
    std::vector<clashflux::ui::SectionTab> tabs;
    for (int i = 0; i < count; ++i) {
        tabs.push_back({key(i), label(i), i == 11 ? "!" : ""});
    }
    std::function<void()> prev;
    std::function<void()> next;
    if (index > 0) prev = [select, index] { select(index - 1); };
    if (index + 1 < count) next = [select, index] { select(index + 1); };
    huxerui::View page = huxerui::ScrollView(
        huxerui::Column {
          huxerui::Text("PAGE-" + std::to_string(index)),
          huxerui::Row {}.With(huxerui::Frame{.height = 600.0F}),
        })
        .With(huxerui::Grow(1.0F))
        .On<huxerui::ViewEvents::PointerIntercept>(
            clashflux::ui::SectionTabSwipeHandler(origin, owned, prev, next))
        .Key("page-content");
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
      clashflux::ui::ProxyGroupPage(page, direction.Get()).Key(key(index)),
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

void checkEntry(huxerui::testing::UiTest& ui, int expectedDirection) {
    auto content = ui.Find(UiSelector::Key("page-content"));
    ui.Pump(); // 将 Lifecycle 的新目标落帧，之后虚拟时间才推进动画。
    const float start = content.One().bounds.x - kPageInset;
    check(expectedDirection * start > 4.0F, "新页从正确的左右方向滑入");
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
        check(std::abs(ui.Find(UiSelector::Key("page-content")).One().bounds.x - kPageInset) <= 0.5F,
              "首次显示不制造无方向的入场运动");
        checkVisible(ui, 0, "初始标签可见");
        check(ui.Find(UiSelector::Key("section-tab-picker")).Exists(), "溢出时提供标签菜单");

        // 点击标签：前后方向不同，选择不需要等待模型泵。
        ui.Find(UiSelector::Key("section-tab-group-1")).Tap();
        check(ui.Find(UiSelector::Text("PAGE-1")).Exists(), "点击立即显示选中组");
        checkEntry(ui, 1);
        ui.Find(UiSelector::Key("section-tab-group-0")).Tap();
        checkEntry(ui, -1);

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
        check(ui.Find(UiSelector::Text("PAGE-11")).Exists(), "菜单选择立即切换内容");
        checkEntry(ui, 1);
        checkVisible(ui, 11, "菜单选中末尾标签后自动滚到完整可见");

        // 真正的内容横滑使用生产处理器，沿标签顺序返回并保持标签与内容同步。
        ui.Drag({140.0F, 210.0F}, {260.0F, 210.0F});
        check(ui.Find(UiSelector::Text("PAGE-10")).Exists(), "内容右滑切换到上一组");
        checkEntry(ui, -1);
        checkVisible(ui, 10, "内容滑动后选中标签可见");
        ui.Drag({260.0F, 210.0F}, {140.0F, 210.0F});
        check(ui.Find(UiSelector::Text("PAGE-11")).Exists(), "内容左滑切换到下一组");
        checkEntry(ui, 1);
        checkVisible(ui, 11, "滑回末尾标签仍完整可见");

        ui.Find(UiSelector::Key("section-tab-scroll")).ScrollBy({-300.0F, 0.0F});
        ui.PumpAndSettle();
        check(!ui.Find(UiSelector::Key("section-tab-group-11")).One().in_viewport,
              "手动滚动允许浏览其他标签，不会立即被拉回选中项");
        ui.Find(UiSelector::Text("First")).Tap();
        checkEntry(ui, -1);
        checkVisible(ui, 0, "从末尾切回首项时标签栏滚回开头");

        // 手机触摸打开菜单，并通过真实辅助功能动作选择，确保两条输入路径共用逻辑。
        ui.Find(UiSelector::Key("section-tab-picker")).Tap();
        ui.Pump(kSettle);
        ui.FindSemantics(huxerui::testing::UiSemanticSelector::AllOf(
            huxerui::testing::UiSemanticSelector::Role(huxerui::SemanticRole::MenuItem),
            huxerui::testing::UiSemanticSelector::Label(label(6))))
            .PerformSemanticAction({huxerui::SemanticActionKind::Activate, {}});
        checkEntry(ui, 1);
        checkVisible(ui, 6, "触摸菜单和辅助功能选择同样滚到目标标签");
        ui.Find(UiSelector::Text("First")).Tap();
        checkEntry(ui, -1);

        // 在过渡完成前反向切换，最终意图与方向仍正确。
        ui.Find(UiSelector::Text("Last")).Tap();
        ui.Pump(kStep);
        ui.Find(UiSelector::Text("First")).Tap();
        checkEntry(ui, -1);
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
        check(std::abs(ui.Find(UiSelector::Key("page-content")).One().bounds.x - kPageInset) <= 0.5F,
              "减少动态效果时直接落到目标位置");
        checkVisible(ui, 11, "减少动态效果仍保证选中标签可见");
    }
    std::printf("test_page_transition: %s\n", failures == 0 ? "ok" : "failed");
    return failures == 0 ? 0 : 1;
}
