// test_page_transition.cpp — 页内分区切换动画的最小同构用例。
//
// 代理页只为当前分组构造内容（虚拟化），换组时新页是**全新挂载**的节点；而
// `AnimateTo` 只在目标值变化时才有动画、新挂载会直接落到目标值上——所以页面自带一条
// 本地进度：挂载后由 Lifecycle 从 0 推到 1（页 Key 随分区变化，新挂载即新 scope）。
// 这里用 huxerui 的无窗口 Runtime（真实帧 + 虚拟时间）验证这套结构下动画真的会播：
// 落帧后带着 12pt 偏移起步，推进时间逐步归位。
//
// 帧模型（与上游 ui_testing 的动画用例一致）：写状态后的第一帧只是让新目标落帧
// （动画从这一帧起算），之后必须再 Pump(时长) 才会有中间值。
#include <chrono>
#include <cmath>
#include <cstdio>

#include <huxerui/huxerui.h>
#include <huxerui/testing/ui_test.h>

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

// 与 src/ui/proxies_page.cpp 的 ProxyGroupPage 同构：挂载后把本地进度推到 1。
[[huxerui::composable]] huxerui::View TestPage(huxerui::View content) {
    auto progress = huxerui::UseState(0.0F);
    huxerui::Lifecycle([progress] {
        progress = 1.0F;
        return [] {};
    }, 0);
    huxerui::View page = std::move(content);
    return std::move(page).With(
        huxerui::Grow(1.0F),
        huxerui::Transition{huxerui::AnimateTo(
            progress.Get(),
            huxerui::TweenSpec{0.2, huxerui::Easing::EaseOut})}
            .Opacity(0.82F, 1.0F)
            .Offset({12.0F, 0.0F}, {}));
}

[[huxerui::composable]] huxerui::View TestRoot() {
    auto selected = huxerui::UseState(0);
    const int index = selected.Get();
    huxerui::View page = huxerui::Column {
        huxerui::Text(index == 0 ? "PAGE-A" : "PAGE-B"),
    }.With(huxerui::Grow(1.0F)).Key("page-content");
    return huxerui::Column {
        huxerui::Row {
            huxerui::Button("A").OnClick([selected] { selected = 0; }),
            huxerui::Button("B").OnClick([selected] { selected = 1; }),
        }.With(huxerui::Spacing(8.0F)),
        TestPage(std::move(page)).Key(index == 0 ? "page-a" : "page-b"),
    }.With(huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

const huxerui::Application kApplication{TestRoot};

} // namespace

int main() {
    huxerui::testing::UiTest ui(kApplication, {.viewport = {400.0F, 400.0F}});

    auto content = ui.Find(UiSelector::Key("page-content"));
    check(content.Exists(), "首帧挂载了分区内容");
    if (!content.Exists()) return 1;
    const float mounted = content.One().bounds.x;
    check(mounted > 4.0F, "挂载即处于入场起点（带横向偏移）");
    ui.Pump();                       // 让「进度=1」这个新目标落帧，动画从此帧起算
    ui.Pump(kStep);
    const float entering = content.One().bounds.x;
    check(entering < mounted, "首次挂载也播入场动画（偏移在收敛）");
    ui.Pump(kSettle);
    check(std::abs(content.One().bounds.x) <= 0.5F,
          "入场结束回到零偏移（进度确实推到 1，不会永久停在 0.82/12pt）");

    // 切到另一个分区：新页是全新挂载的节点，应重新播一遍 0 → 1。
    ui.Find(UiSelector::Text("B")).Tap();
    check(ui.Find(UiSelector::Text("PAGE-B")).Exists(), "切页后显示新分区内容");
    auto switched = ui.Find(UiSelector::Key("page-content"));
    if (!switched.Exists()) return 1;
    ui.Pump();                       // 新目标落帧
    const float start = switched.One().bounds.x;
    check(start > 4.0F, "换页后从右侧 12pt 起步");
    ui.Pump(kStep);
    const float moving = switched.One().bounds.x;
    check(moving < start, "换页动画在推进（偏移逐步收敛）");
    ui.Pump(kSettle);
    check(std::abs(switched.One().bounds.x) <= 0.5F, "换页动画结束回到零偏移");

    if (failures == 0) {
        std::printf("test_page_transition: ok\n");
        return 0;
    }
    std::printf("test_page_transition: %d failure(s)\n", failures);
    return 1;
}
