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
#include "rule_target_picker.h"
#include "log_actions.h"
#include "profile_file_picker.h"
#include "profile_import_task.h"
#include "section_tab_picker.h"
#include "responsive_shell.h"
#include "empty_state.h"
#include "theme_colors.h"
#include "theme_color_card.h"

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

[[huxerui::composable]] huxerui::View TestContent(bool followMotion = false, bool groupDrawer = false) {
    auto selected = huxerui::UseState(0);
    auto sharedMotion = clashflux::ui::UseSectionTabMotion();
    auto motion = followMotion ? sharedMotion : nullptr;
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
      }, motion, groupDrawer ? clashflux::ui::SectionTabPickerMode::ResponsiveGroups
                            : clashflux::ui::SectionTabPickerMode::Menu),
      huxerui::Text("SELECTED-" + std::to_string(index)),
      clashflux::ui::SectionTabPages(pages, static_cast<std::size_t>(index),
          [select](std::size_t next) { select(static_cast<int>(next)); }, motion),
    }.With(huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

constexpr float kPageInset = clashflux::ui::kSectionCardSpacing * 0.5F;

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
      huxerui::ProvideEnvironment(clashflux::ui::SectionTabContentInsets{clashflux::ui::kSectionCardSpacing},
          huxerui::Column {TestContent()}.With(
              huxerui::Padding(huxerui::EdgeInsets::Symmetric(0.0F, kPageInset)), huxerui::Grow(1.0F))),
    };
}
huxerui::View DrawerRoot() {
    return huxerui::FlatTheme{huxerui::Scope([] {
        auto clicks = huxerui::UseState(0);
        return huxerui::Column{
            huxerui::Button("TITLE-" + std::to_string(clicks.Get()))
                .OnClick([clicks] { clicks = clicks.Get() + 1; })
                .With(huxerui::Frame{.height = clashflux::ui::kDesktopTitleBarHeight}),
            huxerui::Row{}.With(huxerui::Frame{.height = 1.0F}),
            huxerui::ProvideEnvironment(
                clashflux::ui::SectionTabPickerInsets{clashflux::ui::kDesktopTitleBarHeight + 1.0F},
                TestContent(false, true)),
        }.With(huxerui::Grow(1.0F), huxerui::Spacing(0.0F));
    })};
}
const huxerui::Application kDrawerApplication{DrawerRoot,
    {.show_debug_overlay = false, .window_hooks = {clashflux::ui::InstallSectionPickerLayers}}};

huxerui::View MotionRoot() { return huxerui::FlatTheme{TestContent(true)}; }
const huxerui::Application kMotionApplication{MotionRoot};

huxerui::View ReducedRoot() {
    huxerui::ThemeSpec theme = huxerui::FlatLightThemeSpec();
    theme.motion.reduced_motion = true;
    return huxerui::FlatTheme(theme,
        huxerui::ProvideEnvironment(clashflux::ui::SectionTabContentInsets{clashflux::ui::kSectionCardSpacing},
            huxerui::Column {TestContent()}.With(
                huxerui::Padding(huxerui::EdgeInsets::Symmetric(0.0F, kPageInset)), huxerui::Grow(1.0F))));
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

huxerui::Color paintedTextColor(huxerui::testing::UiTest& ui, const std::string& text, const std::string& channel = "foreground") {
    const auto snapshot = ui.CaptureSnapshot().ToString();
    const std::regex pattern{channel + R"(\s+\{\s+text\s+\{\s+bounds\s+\{[^}]*\}\s+content\s+\{\s+text=")" + text +
        R"("[\s\S]*?foreground\s+\{\s+red=([\d.-]+)\s+green=([\d.-]+)\s+blue=([\d.-]+)\s+alpha=([\d.-]+))"};
    std::smatch match;
    check(std::regex_search(snapshot, match, pattern), "标签实际绘制颜色可读取");
    if (match.empty()) return {};
    return {std::stof(match[1]), std::stof(match[2]), std::stof(match[3]), std::stof(match[4])};
}
huxerui::Color labelColor(huxerui::testing::UiTest& ui, int index) {
    return paintedTextColor(ui, label(index));
}
float colorDistance(huxerui::Color a, huxerui::Color b) {
    return std::abs(a.red - b.red) + std::abs(a.green - b.green) + std::abs(a.blue - b.blue);
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
clashflux::ui::RuleTargetCatalog PickerCatalog() {
    clashflux::ui::RuleTargetCatalog result{.profileId = 1, .ready = true,
        .objects = {{1, "Same"}, {1, "Group two"}, {2, "Same"}, {2, "Node only"}}};
    for (int i = 0; i < 220; ++i) result.objects.push_back({2, "bulk-" + std::to_string(i)});
    return result;
}
[[huxerui::composable]] huxerui::View PickerContent() {
    auto catalog = huxerui::UseState(PickerCatalog());
    auto kind = huxerui::UseState<std::size_t>(1);
    auto object = huxerui::UseState(huxerui::TextEditingValue{});
    auto search = huxerui::UseState(huxerui::TextEditingValue{});
    return huxerui::Column{
        huxerui::Row{
            huxerui::Button("Refresh missing").OnClick([catalog] {
                auto next = catalog.Get(); next.ready = true; next.objects.clear(); catalog = next;
            }),
            huxerui::Button("Native source").OnClick([catalog] {
                catalog = clashflux::ui::RuleTargetCatalog{.profileId = 1, .ready = true, .defaultOnly = true};
            }),
            huxerui::Button("Switch source").OnClick([catalog, object, search] {
                clashflux::ui::ResetRuleTargetSelection(catalog, object, search);
                catalog = clashflux::ui::RuleTargetCatalog{.profileId = 2, .ready = true, .objects = {{2, "Other"}}};
            }),
        },
        huxerui::Button("Loading source").OnClick([catalog] {
            auto next = catalog.Get(); next.ready = false; catalog = next;
        }),
        clashflux::ui::RuleTargetPicker(catalog, kind, object, search),
        huxerui::Text("VALUE-" + std::to_string(kind.Get()) + "-" + object.Get().text),
        huxerui::Text("SEARCH-" + search.Get().text),
        huxerui::Text(clashflux::ui::RuleTargetSelectionValid(catalog.Get(), catalog.Get().profileId,
            kind.Get(), object.Get().text) ? "VALID" : "INVALID"),
    }.With(huxerui::Spacing(8.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}
huxerui::View PickerRoot() { return huxerui::FlatTheme{PickerContent()}; }
const huxerui::Application kPickerApplication{PickerRoot};

void testRuleTargetPicker() {
    {
        huxerui::testing::UiTest ui(kPickerApplication, {.viewport = {700.0F, 800.0F}, .resources = {.locale = huxerui::Locale::FromLanguageTag("zh")}, .resource_provider = AppResources()});
        ui.PumpAndSettle();
        check(ui.Find(UiSelector::Text("INVALID")).Exists(), "对象目录加载不自动选择第一个对象");
        ui.Find(UiSelector::Key("rule-target-object")).Tap();
        ui.PumpAndSettle();
        ui.Find(UiSelector::Text("Same")).Tap();
        ui.PumpAndSettle();
        check(ui.Find(UiSelector::Text("VALUE-1-Same")).Exists() && ui.Find(UiSelector::Text("VALID")).Exists(),
              "通过生产下拉组件选择策略组");
        ui.Find(UiSelector::Text("Loading source")).Tap();
        ui.PumpAndSettle();
        ui.Find(UiSelector::Key("rule-target-kind")).Tap();
        ui.PumpAndSettle();
        check(!ui.Find(UiSelector::Text("节点")).Exists(), "目录读取完成前不开放新对象类型");
        ui.PressKey(huxerui::Key::Escape);
        ui.PumpAndSettle();
        check(ui.Find(UiSelector::Text("VALUE-1-Same")).Exists(), "读取期间保留原对象名称");
        ui.Find(UiSelector::Text("Refresh missing")).Tap();
        ui.PumpAndSettle();
        check(ui.Find(UiSelector::Text("VALUE-1-Same")).Exists() && ui.Find(UiSelector::Text("INVALID")).Exists(),
              "目录刷新后失效引用保留原名，不重绑其它对象");
        ui.Find(UiSelector::Text("Native source")).Tap();
        ui.PumpAndSettle();
        ui.Find(UiSelector::Key("rule-target-kind")).Tap();
        ui.PumpAndSettle();
        check(!ui.Find(UiSelector::Text("节点")).Exists(), "原生连接/JSON/手机目录不暴露节点选择");
        ui.Find(UiSelector::Text("默认出口")).Tap();
        ui.PumpAndSettle();
        check(ui.Find(UiSelector::Text("VALUE-0-")).Exists() && ui.Find(UiSelector::Text("VALID")).Exists(),
              "失效引用可显式切换到默认出口");
    }
    {
        huxerui::testing::UiTest ui(kPickerApplication, {.viewport = {700.0F, 800.0F}, .resources = {.locale = huxerui::Locale::FromLanguageTag("zh")}, .resource_provider = AppResources()});
        ui.PumpAndSettle();
        ui.Find(UiSelector::Key("rule-target-kind")).Tap();
        ui.PumpAndSettle();
        ui.Find(UiSelector::Text("节点")).Tap();
        ui.PumpAndSettle();
        check(ui.Find(UiSelector::Text("VALUE-2-")).Exists(), "改变目标类型清空原对象");
        const auto search = ui.Find(UiSelector::Key("rule-target-search"));
        search.Tap(); search.EnterText("bulk-219");
        ui.Find(UiSelector::Key("rule-target-object")).Tap();
        ui.PumpAndSettle();
        ui.Find(UiSelector::Text("bulk-219")).Tap();
        ui.PumpAndSettle();
        check(ui.Find(UiSelector::Text("VALUE-2-bulk-219")).Exists(), "搜索可以选择超过初始二百项上限的对象");
        search.Tap(); search.ReplaceText("no-match");
        check(ui.Find(UiSelector::Text("VALID")).Exists() && ui.Find(UiSelector::Text("VALUE-2-bulk-219")).Exists(),
              "修改搜索不改变已选对象或来源身份");
        ui.Find(UiSelector::Text("Switch source")).Tap();
        ui.PumpAndSettle();
        check(ui.Find(UiSelector::Text("VALUE-2-")).Exists() && ui.Find(UiSelector::Text("SEARCH-")).Exists() &&
              ui.Find(UiSelector::Text("INVALID")).Exists(), "切换来源同步清空对象和搜索，不保留旧目录选择");
        ui.Find(UiSelector::Key("rule-target-object")).Tap();
        ui.PumpAndSettle();
        ui.Find(UiSelector::Text("Other")).Tap();
        check(ui.Find(UiSelector::Text("VALUE-2-Other")).Exists() && ui.Find(UiSelector::Text("VALID")).Exists(),
              "新来源目录可选，不使用上个来源的同名对象");
    }
    const auto catalog = PickerCatalog();
    check(!clashflux::ui::RuleTargetSelectionValid(catalog, 2, 1, "Same"), "过期来源目录不能通过目标校验");
    check(!clashflux::ui::RuleTargetSelectionValid(catalog, 1, 2, "Group two"), "对象校验包含类型，不只比较名称");
}

[[huxerui::composable]] huxerui::View LogMenuContent() {
    auto level = huxerui::UseState<std::size_t>(2);
    auto result = huxerui::UseState(std::string{"idle"});
    return huxerui::Column{
        clashflux::ui::LogActionsMenu(level.Get(), [level](std::size_t next) { level = next; },
            [result] { result = "copy"; }, [result] { result = "file"; }, [result] { result = "clear"; }),
        huxerui::Text("LEVEL-" + std::to_string(level.Get())),
        huxerui::Text("RESULT-" + result.Get()),
    }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Start));
}
huxerui::View LogMenuRoot() { return huxerui::FlatTheme{LogMenuContent()}; }
const huxerui::Application kLogMenuApplication{LogMenuRoot};

void testLogActions() {
    const std::vector<clashflux::ui::LogEntry> entries{{"[10:00] info 中文", 1}, {"[10:01] warning", 2}, {"[10:02] error", 3}};
    check(clashflux::ui::ExportLogText(entries, 0) == "[10:00] info 中文\n[10:01] warning\n[10:02] error\n",
        "全部级别导出保持 Unicode、时间戳和原始顺序");
    check(clashflux::ui::ExportLogText(entries, 2) == "[10:01] warning\n", "导出只包含当前级别可见日志");
    check(clashflux::ui::ExportLogText(entries, 4).empty(), "空过滤结果不泄漏其他级别日志");
    huxerui::testing::UiTest ui(kLogMenuApplication, {.viewport = {600.0F, 500.0F},
        .resources = {.locale = huxerui::Locale::FromLanguageTag("zh")}, .resource_provider = AppResources()});
    ui.PumpAndSettle();
    const auto open = [&] { ui.Find(UiSelector::Key("log-actions-menu")).Tap(); ui.PumpAndSettle(); };
    const auto hover = [&](const char* text) {
        const auto bounds = ui.Find(UiSelector::Text(text)).One().bounds;
        ui.SendPointer({.type = huxerui::PointerEventType::Move, .pointer_id = 23,
            .position = {bounds.x + bounds.width / 2.0F, bounds.y + bounds.height / 2.0F},
            .device_kind = huxerui::PointerDeviceKind::Mouse});
        ui.Pump(kSettle);
    };
    open();
    const auto panel = ui.Find(UiSelector::Key("action-menu-surface")).One().bounds;
    ui.SendPointer({.type = huxerui::PointerEventType::Move, .pointer_id = 23,
        .position = {panel.x + 0.25F, panel.y + 0.25F},
        .device_kind = huxerui::PointerDeviceKind::Mouse});
    ui.Pump(kSettle);
    check(!ui.Find(UiSelector::Text("警告")).Exists(), "面板圆角外悬停不命中内部菜单项");
    hover("级别");
    check(ui.Find(UiSelector::Text("警告")).Exists(), "悬停级别打开日志级别子菜单");
    using huxerui::testing::UiSemanticSelector;
    check(ui.FindSemantics(UiSemanticSelector::AllOf({UiSemanticSelector::Label("警告"),
        UiSemanticSelector::Checked(huxerui::SemanticCheckedState::Checked)})).Exists(), "级别子菜单标记当前选择");
    ui.Find(UiSelector::Text("错误")).Tap(); ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("LEVEL-3")).Exists(), "级别选择写回受控过滤状态");
    open(); hover("导出");
    check(ui.Find(UiSelector::Text("剪贴板")).Exists() && ui.Find(UiSelector::Text("文件")).Exists(), "悬停导出显示剪贴板与文件");
    ui.Find(UiSelector::Text("剪贴板")).Tap(); ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("RESULT-copy")).Exists(), "剪贴板菜单调用复制动作");
    open(); hover("导出"); ui.Find(UiSelector::Text("文件")).Tap(); ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("RESULT-file")).Exists(), "文件菜单调用系统导出动作");
    open(); ui.Find(UiSelector::Text("清空")).Tap(); ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("RESULT-clear")).Exists(), "清空菜单调用清理动作");
    open(); hover("级别"); hover("导出");
    check(ui.Find(UiSelector::Text("文件")).Exists() && !ui.Find(UiSelector::Text("错误")).Exists(),
        "返回父项悬停另一项会替换子菜单");
    ui.PressKey(huxerui::Key::Escape); ui.PumpAndSettle();
    check(!ui.Find(UiSelector::Text("文件")).Exists() && !ui.Find(UiSelector::Text("导出")).Exists(),
        "Escape 同时关闭父菜单与子菜单");
    open(); hover("级别");
    check(ui.FindSemantics(UiSemanticSelector::AllOf({UiSemanticSelector::Label("错误"),
        UiSemanticSelector::Checked(huxerui::SemanticCheckedState::Checked)})).Exists(), "重新打开子菜单显示更新后的级别");
    ui.TapAt({500.0F, 400.0F}); ui.PumpAndSettle();
    check(!ui.Find(UiSelector::Text("级别")).Exists() && !ui.Find(UiSelector::Text("错误")).Exists(),
        "点击菜单外部关闭所有面板");
}

[[huxerui::composable]] huxerui::View CommonMenuContent() {
    auto menu = clashflux::ui::UseActionMenu();
    auto result = huxerui::UseState(std::string{"idle"});
    const auto entries = [result] {
        std::vector<clashflux::ui::ActionMenuEntry> items;
        items.push_back(clashflux::ui::ActionMenuItem("Disabled", [result] { result = "disabled"; }).Enabled(false));
        items.push_back(clashflux::ui::ActionMenuItem("Checked", [result] { result = "checked"; }).Checked(true));
        items.push_back(clashflux::ui::ActionMenuSection{});
        for (int i = 0; i < 40; ++i) {
            items.push_back(clashflux::ui::ActionMenuItem("Action-" + std::to_string(i),
                [result, i] { result = std::to_string(i); }).Danger(i == 39));
        }
        return items;
    };
    return huxerui::Column{
        huxerui::Button("Open common").With(menu.Anchor()).OnClick([menu, entries] { menu.Show(entries()); }),
        huxerui::Button("Open at point").OnClick([menu, entries] { menu.ShowAt({280.0F, 90.0F}, entries()); }),
        huxerui::Text("COMMON-" + result.Get()),
    };
}
huxerui::View CommonMenuRoot() { return huxerui::FlatTheme{CommonMenuContent()}; }
const huxerui::Application kCommonMenuApplication{CommonMenuRoot};
huxerui::View ThemeSwitchRoot() {
    return huxerui::Scope([] {
        auto dark = huxerui::UseState(false);
        auto spec = huxerui::MaterialLightThemeSpec();
        spec.colors = dark.Get() ? clashflux::ui::FluxDarkColors() : clashflux::ui::FluxLightColors();
        return huxerui::Theme(huxerui::MaterialThemeDefinition(spec), huxerui::Column{
            huxerui::Button("Toggle theme").OnClick([dark] { dark = !dark.Get(); }),
            clashflux::ui::EmptyState("Theme body", huxerui::ImageResource{"app", "images/request"}),
        }.With(huxerui::Grow(1.0F)));
    });
}
const huxerui::Application kThemeSwitchApplication{ThemeSwitchRoot, {.show_debug_overlay = false}};
void testThemeSwitch() {
    huxerui::testing::UiTest ui(kThemeSwitchApplication, {
        .viewport = {400.0F, 700.0F}, .resource_provider = AppResources()});
    ui.PumpAndSettle();
    check(colorDistance(paintedTextColor(ui, "Theme body", "content"), clashflux::ui::FluxLightColors().on_surface_variant) < 0.001F,
          "通用组件使用浅色主题变量");
    ui.Find(UiSelector::Text("Toggle theme")).Tap(); ui.PumpAndSettle();
    check(colorDistance(paintedTextColor(ui, "Theme body", "content"), clashflux::ui::FluxDarkColors().on_surface_variant) < 0.001F,
          "一次点击切为深色，已挂载组件绘制色同步切换");
    ui.Find(UiSelector::Text("Toggle theme")).Tap(); ui.PumpAndSettle();
    check(colorDistance(paintedTextColor(ui, "Theme body", "content"), clashflux::ui::FluxLightColors().on_surface_variant) < 0.001F,
          "再次点击恢复浅色主题，无旧颜色残留");
}

huxerui::View EmptyStateRoot() {
    return huxerui::FlatTheme{clashflux::ui::EmptyState(
        "No subscriptions yet. Use the add button to import a subscription and start choosing your proxy groups.",
        huxerui::ImageResource{"app", "images/request"})};
}
const huxerui::Application kEmptyStateApplication{EmptyStateRoot, {.show_debug_overlay = false}};
void testEmptyState() {
    huxerui::testing::UiTest ui(kEmptyStateApplication, {
        .viewport = {1000.0F, 700.0F}, .resource_provider = AppResources()});
    for (auto size : {huxerui::Size{1000.0F, 700.0F}, huxerui::Size{320.0F, 480.0F}}) {
        ui.SetWindowMetrics({.viewport = size}); ui.PumpAndSettle();
        const auto content = ui.Find(UiSelector::Key("empty-state-content")).One().bounds;
        const auto icon = ui.Find(UiSelector::Key("empty-state-icon")).One().bounds;
        const auto message = ui.Find(UiSelector::Key("empty-state-message")).One().bounds;
        check(std::abs(content.x + content.width / 2.0F - size.width / 2.0F) < 0.5F &&
              std::abs(content.y + content.height / 2.0F - size.height / 2.0F) < 0.5F,
              "宽窄视口空状态均在可用内容区域水平、垂直居中");
        check(std::abs(icon.width - 48.0F) < 0.5F && std::abs(icon.height - 48.0F) < 0.5F &&
              std::abs(message.y - icon.y - icon.height - 12.0F) < 0.5F,
              "空状态图标尺寸和图文间距统一");
        check(message.width <= 360.5F && message.x >= 23.5F &&
              message.x + message.width <= size.width - 23.5F,
              "长提示限制宽度并在窄屏内换行，保留两侧留白");
    }
}

huxerui::View ResponsiveShellRoot() {
    return huxerui::FlatTheme{huxerui::Scope([] {
        return clashflux::ui::ResponsiveDesktopShell(
            huxerui::Scope([] {
                auto clicks = huxerui::UseState(0);
                return huxerui::Button("RETAINED-" + std::to_string(clicks.Get()))
                    .OnClick([clicks] { clicks = clicks.Get() + 1; });
            }),
            huxerui::Text("SIDEBAR").With(huxerui::Frame{.width = 80.0F}),
            huxerui::Text("CHROME").With(huxerui::Frame{.height = 40.0F}),
            huxerui::Text("BOTTOM").With(huxerui::Frame{.height = 64.0F}));
    })};
}
const huxerui::Application kResponsiveShellApplication{ResponsiveShellRoot, {.show_debug_overlay = false}};
void testResponsiveDesktopShell() {
    huxerui::testing::UiTest ui(kResponsiveShellApplication, {.viewport = {1000.0F, 700.0F}});
    ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("SIDEBAR")).Exists() && !ui.Find(UiSelector::Text("BOTTOM")).Exists(),
          "桌面宽屏使用侧栏");
    ui.Find(UiSelector::Text("RETAINED-0")).Tap(); ui.PumpAndSettle();
    for (float width : {599.0F, 320.0F}) {
        ui.SetWindowMetrics({.viewport = {width, 480.0F}}); ui.PumpAndSettle();
        check(!ui.Find(UiSelector::Text("SIDEBAR")).Exists() &&
              ui.Find(UiSelector::Text("BOTTOM")).Exists() && ui.Find(UiSelector::Text("CHROME")).Exists(),
              "桌面窄屏隐藏侧栏，显示底部导航并保留窗口控件行");
        const auto content = ui.Find(UiSelector::Key("responsive-pages")).One().bounds;
        check(std::abs(content.width - width) < 0.5F && std::abs(content.y - 40.0F) < 0.5F &&
              content.y + content.height <= 416.5F, "Compact 内容占满宽度并避开窗口栏和底部导航");
        check(ui.Find(UiSelector::Text("RETAINED-1")).Exists(), "缩窄窗口保留页面 State");
    }
    ui.SetWindowMetrics({.viewport = {600.0F, 700.0F}}); ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("SIDEBAR")).Exists() && !ui.Find(UiSelector::Text("BOTTOM")).Exists() &&
          ui.Find(UiSelector::Text("RETAINED-1")).Exists(), "600pt 恢复桌面布局且页面不重挂载");
}

void testGroupDrawers() {
    huxerui::testing::UiTest ui(kDrawerApplication, {.viewport = {1000.0F, 700.0F}, .resource_provider = AppResources()});
    ui.PumpAndSettle();
    ui.Find(UiSelector::Key("section-tab-picker")).Tap();
    ui.Pump();
    const float enteringX = ui.Find(UiSelector::Key("group-picker-item-group-0")).One().bounds.x;
    ui.PumpUntil([&] {
        return ui.Find(UiSelector::Key("group-picker-item-group-0")).One().bounds.x < enteringX;
    });
    const float movingX = ui.Find(UiSelector::Key("group-picker-item-group-0")).One().bounds.x;
    check(movingX > enteringX - 359.0F, "右侧抽屉存在进入中间位置，不直接跳到目标");
    ui.PumpAndSettle();
    const auto side = ui.Find(UiSelector::Key("group-picker-side")).One().bounds;
    check(std::abs(side.x - 640.0F) < 0.5F && std::abs(side.width - 360.0F) < 0.5F &&
          std::abs(side.y - 41.0F) < 0.5F && std::abs(side.height - 659.0F) < 0.5F,
          "宽屏抽屉贴右侧，仅占标题栏分割线以下的内容区域且宽度 360pt");
    check(!ui.Find(UiSelector::Key("group-picker-bottom")).Exists(), "宽屏不显示底部抽屉");
    ui.Find(UiSelector::Text("TITLE-0")).Tap(); ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("TITLE-1")).Exists() &&
          ui.Find(UiSelector::Key("group-picker-side")).Exists(),
          "右侧抽屉打开时标题栏仍可点击，且不会作为外部点击关闭抽屉");
    check(ui.Find(UiSelector::Text("!")).Exists(), "分组抽屉保留保真度角标");
    ui.Find(UiSelector::Key("group-picker-item-group-11")).Tap(); ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("SELECTED-11")).Exists() &&
          !ui.Find(UiSelector::Key("group-picker-side")).Exists(), "右侧抽屉选择后切换分组并关闭");
    checkVisible(ui, 11, "从抽屉选择仍同步居中标签");
    ui.Find(UiSelector::Key("section-tab-picker")).Tap(); ui.PumpAndSettle();
    ui.TapAt({100.0F, 450.0F}); ui.PumpAndSettle();
    check(!ui.Find(UiSelector::Key("group-picker-side")).Exists(), "点击透明的外部区域关闭右侧抽屉");
    ui.Find(UiSelector::Key("section-tab-picker")).Tap(); ui.PumpAndSettle();
    ui.PressKey(huxerui::Key::Escape); ui.PumpAndSettle();
    check(!ui.Find(UiSelector::Key("group-picker-side")).Exists(), "取消键关闭右侧抽屉");
    ui.SetWindowMetrics({.viewport = {400.0F, 700.0F}}); ui.PumpAndSettle();
    ui.Find(UiSelector::Key("section-tab-picker")).Tap(); ui.PumpAndSettle();
    const auto bottom = ui.Find(UiSelector::Key("group-picker-bottom")).One().bounds;
    check(bottom.y > 0.0F && bottom.x >= -0.5F && bottom.x + bottom.width <= 400.5F &&
          bottom.y + bottom.height <= 700.5F, "缩窄后使用贴底且位于视口内的抽屉");
    check(!ui.Find(UiSelector::Key("group-picker-side")).Exists(), "窄屏不显示右侧抽屉");
    ui.Find(UiSelector::Key("group-picker-item-group-0")).Tap(); ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("SELECTED-0")).Exists() &&
          !ui.Find(UiSelector::Key("group-picker-bottom")).Exists(), "底部抽屉选择后切换分组并关闭");
    ui.Find(UiSelector::Key("section-tab-picker")).Tap(); ui.PumpAndSettle();
    ui.PressKey(huxerui::Key::Escape); ui.PumpAndSettle();
    check(!ui.Find(UiSelector::Key("group-picker-bottom")).Exists(), "取消键关闭底部抽屉");
    ui.SetWindowMetrics({.viewport = {1000.0F, 320.0F}}); ui.PumpAndSettle();
    ui.Find(UiSelector::Key("section-tab-picker")).Tap(); ui.PumpAndSettle();
    ui.Find(UiSelector::Key("group-picker-scroll")).ScrollBy({0.0F, 2000.0F}); ui.PumpAndSettle();
    ui.Find(UiSelector::Key("group-picker-item-group-11")).Tap(); ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("SELECTED-11")).Exists(), "矮窗口分组抽屉可滚动到末项选择");
}

void testCommonMenu() {
    huxerui::testing::UiTest ui(kCommonMenuApplication, {.viewport = {600.0F, 500.0F}, .resource_provider = AppResources()});
    ui.PumpAndSettle();
    ui.Find(UiSelector::Text("Open common")).Tap(); ui.PumpAndSettle();
    const auto disabled = ui.Find(UiSelector::Text("Disabled")).One().bounds;
    ui.TapAt({disabled.x + disabled.width * 0.5F, disabled.y + disabled.height * 0.5F});
    ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("COMMON-idle")).Exists() &&
          ui.Find(UiSelector::Key("action-menu-surface")).Exists(), "通用菜单禁用项不执行也不关闭面板");
    check(ui.FindSemantics(huxerui::testing::UiSemanticSelector::AllOf({
        huxerui::testing::UiSemanticSelector::Label("Checked"),
        huxerui::testing::UiSemanticSelector::Checked(huxerui::SemanticCheckedState::Checked)})).Exists(),
        "通用菜单勾选语义保留");
    ui.Find(UiSelector::Text("Checked")).Tap(); ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("COMMON-checked")).Exists() &&
          !ui.Find(UiSelector::Key("action-menu-surface")).Exists(), "通用菜单选择后先关闭再执行操作");
    ui.Find(UiSelector::Text("Open at point")).Tap(); ui.PumpAndSettle();
    const auto surface = ui.Find(UiSelector::Key("action-menu-surface"));
    check(surface.One().bounds.height <= 484.0F, "长菜单保持在窗口内");
    surface.ScrollBy({0.0F, 5000.0F}); ui.PumpAndSettle();
    ui.Find(UiSelector::Text("Action-39")).Tap(); ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("COMMON-39")).Exists(), "按坐标打开的长菜单可滚动到末项并执行");
}

[[huxerui::composable]] huxerui::View ProfileFileFilterContent() {
    const auto filter = clashflux::ui::ProfileConfigFileFilter();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto result = huxerui::UseState(std::string{"idle"});
    return huxerui::Column{
        huxerui::Button("Choose profile file").OnClick([tasks, toast, filter, result] {
            tasks.Launch([toast, filter, result]() -> huxerui::Task<void> {
                // 消费生产筛选 DTO 的真实事件协程；不需要宿主文件窗口即可验证资源边界。
                const auto picked = co_await clashflux::ui::PickProfileConfigFile(nullptr, filter, toast);
                if (!picked) result = filter.name + ":" + filter.extensions[0] + "," + filter.extensions[1] + "," + filter.extensions[2];
            });
        }),
        huxerui::Text(result.Get()),
    };
}
huxerui::View ProfileFileFilterRoot() { return huxerui::FlatTheme{ProfileFileFilterContent()}; }
const huxerui::Application kProfileFileFilterApplication{ProfileFileFilterRoot};

void testProfileFileFilter() {
    for (const auto* language : {"zh", "en"}) {
        huxerui::testing::UiTest ui(kProfileFileFilterApplication, {.viewport = {600.0F, 500.0F},
            .resources = {.locale = huxerui::Locale::FromLanguageTag(language)}, .resource_provider = AppResources()});
        ui.PumpAndSettle();
        ui.Find(UiSelector::Text("Choose profile file")).Tap(); ui.Pump(kSettle);
        const std::string expected = std::string(language) == "zh" ? "sing-box / Clash 配置:json,yaml,yml" : "sing-box / Clash configuration:json,yaml,yml";
        check(ui.Find(UiSelector::Text(expected)).Exists(), "文件选择事件协程使用预解析的本地化筛选 DTO，不读取组合环境");
        ui.Find(UiSelector::Text("Choose profile file")).Tap(); ui.Pump(kSettle);
        check(ui.Find(UiSelector::Text(expected)).Exists(), "文件选择不可用时允许再次操作，不终止运行时");
    }
}
[[huxerui::composable]] huxerui::View ImportTaskOrigin(
    huxerui::State<bool> busy, huxerui::State<std::string> result,
    huxerui::State<int> attempts) {
    const auto launch = [busy, result, attempts](bool fail) {
        clashflux::ui::LaunchProfileImport(busy,
            [fail, attempts]() -> huxerui::Task<std::pair<std::int64_t, std::string>> {
                attempts = attempts.Get() + 1;
                co_await huxerui::Delay(std::chrono::milliseconds(200));
                if (fail) throw std::runtime_error("import fixture failure");
                co_return std::pair<std::int64_t, std::string>{42, ""};
            }, [result](auto outcome) {
                result = outcome.first ? "IMPORT-SUCCESS" : "IMPORT-ERROR:" + outcome.second;
            });
    };
    return huxerui::Row{
        huxerui::Button("Import fixture").OnClick([launch] { launch(false); }),
        huxerui::Button("Fail fixture").OnClick([launch] { launch(true); }),
    };
}
[[huxerui::composable]] huxerui::View ImportTaskContent() {
    auto shown = huxerui::UseState(true);
    auto busy = huxerui::UseState(false);
    auto result = huxerui::UseState(std::string{"IMPORT-IDLE"});
    auto attempts = huxerui::UseState(0);
    return huxerui::Column{
        huxerui::Button("Replace origin").OnClick([shown] { shown = !shown.Get(); }),
        shown.Get() ? ImportTaskOrigin(busy, result, attempts).Key("import-origin")
                    : huxerui::View{huxerui::Row{}}.Key("retired-origin"),
        huxerui::Text(busy.Get() ? "IMPORT-BUSY" : "IMPORT-READY"),
        huxerui::Text(result.Get()),
        huxerui::Text("IMPORT-ATTEMPTS:" + std::to_string(attempts.Get())),
    };
}
huxerui::View ImportTaskRoot() { return huxerui::FlatTheme{ImportTaskContent()}; }
const huxerui::Application kImportTaskApplication{ImportTaskRoot};
void testProfileImportCompletion() {
    huxerui::testing::UiTest ui(kImportTaskApplication, {.viewport = {600.0F, 400.0F}, .resource_provider = AppResources()});
    ui.PumpAndSettle();
    ui.Find(UiSelector::Text("Import fixture")).Tap(); ui.Pump();
    ui.Find(UiSelector::Text("Import fixture")).Tap(); ui.Pump();
    check(ui.Find(UiSelector::Text("IMPORT-ATTEMPTS:1")).Exists(), "busy import rejects duplicate clicks");
    ui.Find(UiSelector::Text("Replace origin")).Tap(); ui.Pump();
    check(!ui.Find(UiSelector::Text("Import fixture")).Exists(), "originating import UI is actually retired during await");
    ui.Pump(std::chrono::milliseconds(400)); ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("IMPORT-SUCCESS")).Exists() && ui.Find(UiSelector::Text("IMPORT-READY")).Exists(),
          "application import survives origin retirement and clears spinner before completion callback");
    ui.Find(UiSelector::Text("Replace origin")).Tap(); ui.PumpAndSettle();
    ui.Find(UiSelector::Text("Fail fixture")).Tap(); ui.Pump(std::chrono::milliseconds(400)); ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("IMPORT-ERROR:import fixture failure")).Exists() &&
          ui.Find(UiSelector::Text("IMPORT-READY")).Exists(), "import exception becomes feedback and clears spinner");
    ui.Find(UiSelector::Text("Import fixture")).Tap(); ui.Pump(std::chrono::milliseconds(400)); ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("IMPORT-SUCCESS")).Exists() && ui.Find(UiSelector::Text("IMPORT-ATTEMPTS:3")).Exists(),
          "failed import remains retryable without retaining busy or creating duplicate operations");
}
[[huxerui::composable]] huxerui::View ThemeColorSurfaceContent() {
    using namespace clashflux::ui;
    const auto& theme = huxerui::UseTheme();
    const float edge = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact ?
        kThemeColorCompactCardEdge : kThemeColorCardEdge;
    auto mode = huxerui::UseState(0);
    auto selected = huxerui::UseState(0);
    auto added = huxerui::UseState(false);
    std::vector<huxerui::View> cards;
    for (int index = 0; index < 2; ++index) {
        const auto accent = ResolveFluxAccent(index == 0 ? "blue" : "purple");
        const bool active = selected.Get() == index;
        cards.push_back(ThemeColorCardSurface(theme,
            IsDarkTheme(theme) ? accent.dark : accent.light,
            IsDarkTheme(theme) ? accent.onDark : accent.onLight,
            active ? std::optional<huxerui::ImageResource>{{"app", "images/check"}} : std::nullopt,
            active, accent.name, [selected, index] { selected = index; }, std::nullopt, edge)
            .Key("test-color-" + std::to_string(index)));
    }
    cards.push_back(ThemeColorCardSurface(theme, theme.colors.surface_container, theme.colors.on_surface,
        huxerui::ImageResource{"app", "images/add"}, false, "Add color", [added] { added = true; }, std::nullopt, edge)
        .Key("test-color-add"));
    std::vector<huxerui::View> modes;
    const std::array<huxerui::ImageResource, 3> icons{{
        {"app", "images/sun_moon"}, {"app", "images/moon"}, {"app", "images/sun"}}};
    const std::array<std::string, 3> labels{"Automatic", "Dark", "Light"};
    for (int index = 0; index < 3; ++index)
        modes.push_back(ThemeColorCardSurface(theme, theme.colors.surface_container, theme.colors.on_surface,
            icons[index], mode.Get() == index, labels[index], [mode, index] { mode = index; },
            huxerui::StringVariant{labels[index]}, edge).Key("test-mode-" + std::to_string(index)));
    return huxerui::Column {
      huxerui::Row(std::move(modes)).With(huxerui::Spacing(kSectionCardSpacing)),
      huxerui::Flow(std::move(cards)).With(huxerui::Spacing(clashflux::ui::kSectionCardSpacing)),
      huxerui::Text(added.Get() ? "color-added" : "color-idle"),
    }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

huxerui::View ThemeColorSurfaceLightRoot() {
    auto theme = huxerui::MaterialLightThemeSpec();
    theme.colors = clashflux::ui::FluxLightColors();
    return huxerui::MaterialTheme(theme, ThemeColorSurfaceContent());
}
huxerui::View ThemeColorSurfaceDarkRoot() {
    auto theme = huxerui::MaterialDarkThemeSpec();
    theme.colors = clashflux::ui::FluxDarkColors();
    theme.spacing.extra_large = 40.0F;
    theme.shapes.medium = 18.0F;
    return huxerui::MaterialTheme(theme, ThemeColorSurfaceContent());
}
const huxerui::Application kThemeColorSurfaceLight{ThemeColorSurfaceLightRoot, {.show_debug_overlay = false}};
const huxerui::Application kThemeColorSurfaceDark{ThemeColorSurfaceDarkRoot, {.show_debug_overlay = false}};

void testThemeColorSurfaces() {
    for (const auto* application : {&kThemeColorSurfaceLight, &kThemeColorSurfaceDark}) {
        for (float width : {320.0F, 800.0F}) {
            huxerui::testing::UiTest ui(*application, {
                .viewport = {width, 500.0F}, .resource_provider = AppResources()});
            ui.PumpAndSettle();
            const auto color = ui.Find(UiSelector::Key("test-color-0")).One();
            const auto other = ui.Find(UiSelector::Key("test-color-1")).One();
            const auto add = ui.Find(UiSelector::Key("test-color-add")).One();
            check(color.size.width == color.size.height && other.size == color.size && add.size == color.size,
                  "color and add cards remain equal squares in narrow and wide themes");
            for (const char* key : {"test-mode-0", "test-mode-1", "test-mode-2"}) {
                const auto mode = ui.Find(UiSelector::Key(key)).One();
                const auto first = ui.Find(UiSelector::Key("test-mode-0")).One();
                check(mode.size == color.size && mode.bounds.y == first.bounds.y && mode.in_viewport,
                      "all mode cards use the same square style and remain in one horizontal row");
            }
            const auto checkCenter = [&ui](const char* key) {
                const auto card = ui.Find(UiSelector::Key(key)).One().bounds;
                const auto symbol = ui.Find(UiSelector::Key(key)).Find(UiSelector::Key("theme-color-symbol")).One().bounds;
                check(std::abs(card.x + card.width / 2 - symbol.x - symbol.width / 2) < 0.1F &&
                          std::abs(card.y + card.height / 2 - symbol.y - symbol.height / 2) < 0.1F,
                      "selection check and add icon stay centered in their card");
            };
            checkCenter("test-color-0");
            checkCenter("test-color-add");
            ui.Find(UiSelector::Key("test-color-1")).Tap();
            ui.PumpAndSettle();
            check(!ui.Find(UiSelector::Key("test-color-0")).Find(UiSelector::Key("theme-color-symbol")).Exists(),
                  "previous selection mark clears after selecting another color");
            checkCenter("test-color-1");
            ui.Find(UiSelector::Key("test-color-add")).Tap();
            ui.PumpAndSettle();
            check(ui.Find(UiSelector::Text("color-added")).Exists(), "matching add card retains its click action");
        }
    }
}
} // namespace

void testCustomThemeColors() {
    using namespace clashflux::ui;
    check(NormalizeThemeColor("  aA44cC ") == "#AA44CC", "normalize custom color input");
    for (const auto invalid : {"", "#123", "#12345678", "#GG1234", "#12 3456"})
        check(!NormalizeThemeColor(invalid), "reject incomplete or invalid custom colors");
    const auto saved = ReadCustomThemeColors("#336699\ninvalid\n336699\n#aa44cc");
    check(saved == std::vector<std::string>{"#336699", "#AA44CC"}, "custom colors normalize and deduplicate");
    check(ReadCustomThemeColors(SaveCustomThemeColors(saved)) == saved, "custom colors round trip");
    check(ThemeColorFromHex("#336699") == huxerui::Color::Rgb(51, 102, 153), "hex channels decode correctly");
    check(ResolveFluxAccent("invalid").id == "blue", "invalid saved color uses default");
    for (const auto hex : {"#000000", "#FFFFFF", "#FFFF00", "#0000FF", "#FF00FF", "#336699"}) {
        const auto accent = ResolveFluxAccent(hex);
        check(accent.id == hex, "custom color retains its stable identity");
        for (bool dark : {false, true}) {
            const auto colors = dark ? FluxDarkColors(hex) : FluxLightColors(hex);
            const float a = ThemeColorLuminance(colors.primary), b = ThemeColorLuminance(colors.on_primary);
            check((std::max(a, b) + 0.05F) / (std::min(a, b) + 0.05F) >= 4.5F,
                  "custom button text retains readable contrast in both modes");
        }
    }
}

int main() {
    testCustomThemeColors();
    testThemeColorSurfaces();
    {
        huxerui::testing::UiTest ui(kMotionApplication, {.viewport = {800.0F, 700.0F}, .resource_provider = AppResources()});
        ui.PumpAndSettle();
        const auto active = labelColor(ui, 0);
        const auto normal = labelColor(ui, 1);
        check(colorDistance(active, normal) > 0.1F, "选中与普通文字色可区分");
        huxerui::PointerEvent event{.type = huxerui::PointerEventType::Down,
            .pointer_id = 71, .position = {600.0F, 210.0F},
            .device_kind = huxerui::PointerDeviceKind::Touch,
            .changed_button = huxerui::PointerButton::Primary, .pressed_buttons = huxerui::PointerButton::Primary};
        ui.SendPointer(event);
        event.type = huxerui::PointerEventType::Move;
        event.position.x = 400.0F;
        ui.SendPointer(event);
        ui.Pump();
        const auto faded = labelColor(ui, 0);
        const auto raised = labelColor(ui, 1);
        check(colorDistance(faded, active) > 0.02F && colorDistance(faded, normal) > 0.02F,
              "拖动时当前标签逐渐淡出，保留中间颜色");
        check(colorDistance(raised, normal) > 0.02F && colorDistance(raised, active) > 0.02F,
              "目标标签在提交选择前逐渐增强");
        check(ui.Find(UiSelector::Text("SELECTED-0")).Exists(), "渐变不提前改变选中语义");
        event.position.x = 300.0F;
        ui.SendPointer(event);
        ui.Pump();
        check(colorDistance(labelColor(ui, 0), normal) < colorDistance(faded, normal),
              "继续拖动使当前标签更接近普通色");
        check(colorDistance(labelColor(ui, 1), active) < colorDistance(raised, active),
              "继续拖动使目标标签更接近选中色");
        event.position.x = 500.0F;
        ui.SendPointer(event);
        ui.Pump();
        check(colorDistance(labelColor(ui, 0), active) < colorDistance(faded, active),
              "按住反向拖动时颜色沿原轨道恢复");
        event.type = huxerui::PointerEventType::Cancel;
        event.pressed_buttons = huxerui::PointerButton::None;
        ui.SendPointer(event);
        ui.PumpAndSettle();
        check(colorDistance(labelColor(ui, 0), active) < 0.001F &&
              colorDistance(labelColor(ui, 1), normal) < 0.001F, "取消回弹恢复两端文字颜色");
        ui.Find(UiSelector::Key("section-tab-group-1")).Tap();
        ui.Pump(kStep);
        check(colorDistance(labelColor(ui, 1), active) > 0.001F,
              "点击切换时目标文字不提前跳成完整选中色");
        ui.PumpAndSettle();
        check(colorDistance(labelColor(ui, 1), active) < 0.001F &&
              colorDistance(labelColor(ui, 0), normal) < 0.001F, "完成切换后两端颜色完全交换");
        ui.Find(UiSelector::Text("Last")).Tap();
        ui.Pump(kStep);
        ui.Find(UiSelector::Text("First")).Tap();
        ui.PumpAndSettle();
        check(colorDistance(labelColor(ui, 0), active) < 0.001F &&
              colorDistance(labelColor(ui, 1), normal) < 0.001F, "快速远距离反向改选后颜色恢复正确");
    }

    testRuleTargetPicker();
    testLogActions();
    testCommonMenu();
    testGroupDrawers();
    testResponsiveDesktopShell();
    testEmptyState();
    testThemeSwitch();
    testProfileFileFilter();
    testProfileImportCompletion();
    {
        huxerui::testing::UiTest ui(kApplication, {
            .viewport = {400.0F, 700.0F},
            .resource_provider = AppResources(),
        });
        ui.PumpAndSettle();
        check(std::abs(ui.Find(UiSelector::Key("page-content-0")).One().bounds.x - kPageInset) <= 0.5F,
              "首次显示不制造无方向的入场运动");
        const auto viewport = ui.Find(UiSelector::Key("section-tab-pages")).One().bounds;
        check(std::abs(viewport.x) < 0.5F && std::abs(viewport.width - 400.0F) < 0.5F,
              "分页视口占满容器宽度，左右内容边距不缩窄滑动区域");
        check(std::abs(ui.Find(UiSelector::Key("page-content-0")).One().bounds.width -
                       (viewport.width - 2.0F * kPageInset)) < 0.5F,
              "内容宽度由每页的左右边距决定");
        checkVisible(ui, 0, "初始标签可见");
        check(ui.Find(UiSelector::Key("section-tab-picker")).Exists(), "溢出时提供标签菜单");

        const auto initialIndicator = indicator(ui);
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

        // 受控选择跳到未显示的目标页后，标签条应揭示完整目标项。
        ui.Find(UiSelector::Text("Last")).Tap();
        checkEntry(ui, 11, 1);
        checkVisible(ui, 11, "远距离切换后末尾标签完整可见");

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
        ui.Pump(kSettle);
        check(!ui.Find(UiSelector::Key("section-tab-group-11")).One().in_viewport,
              "手动滚动允许浏览其他标签，不会立即被拉回选中项");
        ui.Find(UiSelector::Text("First")).Tap();
        checkEntry(ui, 0, -1);
        checkVisible(ui, 0, "从末尾切回首项时标签栏滚回开头");

        // 在过渡完成前反向切换，最终意图与方向仍正确。
        ui.Find(UiSelector::Text("Last")).Tap();
        ui.Pump(kStep);
        ui.Find(UiSelector::Text("First")).Tap();
        checkEntry(ui, 0, -1);
        checkVisible(ui, 0, "快速反向切换后首项可见");

        ui.Find(UiSelector::Text("Last")).Tap();
        checkEntry(ui, 11, 1);
        ui.SetWindowMetrics({.viewport = {260.0F, 700.0F}});
        for (int frame = 0; frame < 10; ++frame) ui.Pump(kStep);
        checkVisible(ui, 11, "缩窄窗口后重新保证选中标签可见");
        ui.Find(UiSelector::Text("Fewer")).Tap();
        ui.Pump(kSettle);
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
        const auto outgoing = content.One().bounds;
        const auto incoming = ui.Find(UiSelector::Key("page-content-1")).One().bounds;
        check(std::abs(incoming.x - (outgoing.x + outgoing.width) - 2.0F * kPageInset) < 0.5F,
              "拖动中两页边距合成一份卡片间距，形成连续但不粘连的卡片节奏");
        check(std::abs(incoming.x - outgoing.x - 400.0F) < 0.5F,
              "横滑步长保持完整容器宽度");
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
