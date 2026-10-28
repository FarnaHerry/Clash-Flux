// 使用生产标签栏与内容 Pager，在无窗口 Runtime 中验证跟手翻页、嵌套边界和实际几何。
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>

#include <huxerui/huxerui.h>
#include <huxerui/testing/ui_test.h>

#include "ui.h"
#include "box_cat.h"
#include "runtime_diagnostics.h"
#include "text_document.h"
#include "home_control_motion.h"
#include "home_header_control.h"
#include "home_grid.h"
#include "home_card.h"
#include "home_easter_eggs.h"
#include "home_mode_choices.h"
#include "interaction_theme.h"
#include "home_vpn_status.h"
#include "rule_target_picker.h"
#include "log_actions.h"
#include "filtered_indices.h"
#include "profile_file_picker.h"
#include "profile_import_task.h"
#include "profile_list_model.h"
#include "profile_reorder_geometry.h"
#include "section_tab_picker.h"
#include "responsive_shell.h"
#include "page_layout.h"
#include "about_update.h"
#include "title_bar_search.h"
#include "page_loading.h"
#include "empty_state.h"
#include "connection_row.h"
#include "theme_colors.h"
#include "theme_color_card.h"
#include "vpn_snapshot_model.h"
#include "core_operation.h"
#include "inbound_settings.h"
#include "pptp_connection_progress.h"
#include "subscription_rule_layout.h"
#include "profile_card_layout.h"
#include "profile_list_layout.h"
#include "proxy_display_name.h"

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

std::shared_ptr<clashflux::ui::ProfileGridGeometry> reorderGeometryFixture;

huxerui::View ProfileReorderGeometryRoot() {
    return huxerui::FlatTheme{huxerui::VirtualGrid(3, [](std::size_t index) {
        const auto id = static_cast<std::int64_t>(index + 1);
        return huxerui::Text("profile-" + std::to_string(id))
            .With(huxerui::Frame{.height = 180.0F},
                  clashflux::ui::ProfileSlotBounds{reorderGeometryFixture, id})
            .Key(id);
    }).Columns(huxerui::GridColumns::Fixed(3))
      .ColumnSpacing(12.0F).EstimatedRowExtent(180.0F)};
}
const huxerui::Application kProfileReorderGeometryApplication{ProfileReorderGeometryRoot};

huxerui::View ProfileReorderDragRoot() {
    return huxerui::FlatTheme{huxerui::Scope([] {
        auto order = huxerui::UseState(std::vector<std::int64_t>{1, 2, 3});
        auto active = huxerui::UseState<std::int64_t>(0);
        auto target = huxerui::UseState<std::int64_t>(0);
        const auto ids = order.Get();
        const std::int64_t activeId = active.Get();
        const std::int64_t targetId = target.Get();
        const auto drop = [order, active, target, ids](std::int64_t from,
                                                        huxerui::Point position) {
            const auto to = reorderGeometryFixture->Closest(ids, position);
            if (from != to && to != 0) {
                auto reordered = ids;
                const auto source = std::find(reordered.begin(), reordered.end(), from);
                const auto destination = std::find(reordered.begin(), reordered.end(), to);
                if (source != reordered.end() && destination != reordered.end()) {
                    const auto value = *source;
                    const auto fromIndex = source - reordered.begin();
                    const auto toIndex = destination - reordered.begin();
                    reordered.erase(reordered.begin() + fromIndex);
                    reordered.insert(reordered.begin() + toIndex, value);
                    order = std::move(reordered);
                }
            }
            active = 0;
            target = 0;
        };
        return huxerui::VirtualGrid(ids.size(), [=](std::size_t index) {
            const auto id = ids[index];
            huxerui::Point offset{};
            const auto from = std::find(ids.begin(), ids.end(), activeId);
            const auto to = std::find(ids.begin(), ids.end(), targetId);
            if (from != ids.end() && to != ids.end()) {
                const auto destination = clashflux::ui::ProfileReorderDestination(
                    index, static_cast<std::size_t>(from - ids.begin()),
                    static_cast<std::size_t>(to - ids.begin()));
                if (destination != index) {
                    const auto& sourceBounds = reorderGeometryFixture->slots.at(id);
                    const auto& targetBounds = reorderGeometryFixture->slots.at(ids[destination]);
                    offset = {targetBounds.x - sourceBounds.x,
                              targetBounds.y - sourceBounds.y};
                }
            }
            auto handle = huxerui::Text("profile-" + std::to_string(id))
                .With(huxerui::Frame{.height = 180.0F},
                      huxerui::DragSource(id, [id] {
                          return huxerui::Text("preview-" + std::to_string(id));
                      }))
                .On<huxerui::DragSourceEvents::Started>([active, id](const auto&) {
                    active = id;
                })
                .On<huxerui::DragSourceEvents::Changed>([target, ids, id](
                    const huxerui::DragEvent& event) {
                    if (const auto closest = reorderGeometryFixture->Closest(ids, event.window_position)) {
                        target = closest;
                    }
                })
                .On<huxerui::DragSourceEvents::Ended>([active, target](const auto&) {
                    active = 0;
                    target = 0;
                });
            return huxerui::Column{std::move(handle).With(huxerui::Offset(
                       huxerui::AnimateTo(offset, huxerui::TweenSpec{.duration = 0.18})))}
                .With(clashflux::ui::ProfileSlotBounds{reorderGeometryFixture, id},
                      huxerui::DropTarget::Accepts<std::int64_t>())
                .On<huxerui::DropEvents<std::int64_t>::Dropped>(
                    [drop](const std::int64_t& source, const huxerui::DropEvent& event) {
                        drop(source, event.window_position);
                    })
                .Key("reorder-slot-" + std::to_string(index));
        }).Columns(huxerui::GridColumns::Fixed(3))
          .ColumnSpacing(12.0F).EstimatedRowExtent(180.0F)
          .With(huxerui::DropTarget::Accepts<std::int64_t>())
          .On<huxerui::DropEvents<std::int64_t>::Dropped>(
              [drop](const std::int64_t& source, const huxerui::DropEvent& event) {
                  drop(source, event.window_position);
              });
    })};
}
const huxerui::Application kProfileReorderDragApplication{ProfileReorderDragRoot};

std::size_t connectionRowsBuilt = 0;
huxerui::View ConnectionListRoot() {
    return huxerui::FlatTheme{huxerui::Scope([] {
        const auto active = huxerui::UseState(false);
        const auto shortList = huxerui::UseState(false);
        const auto closed = huxerui::UseState(std::string{});
        const auto scroll = huxerui::UseScrollController();
        const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
        const std::size_t count = shortList.Get() ? 1 : 1000;
        huxerui::View list = huxerui::Row{};
        if (active.Get()) {
            list = huxerui::VirtualList(count, [compact, closed](std::size_t index) {
                ++connectionRowsBuilt;
                const std::string id = std::to_string(index);
                const clashflux::ui::ConnectionRow row{
                    .id = id, .host = index == 0 ? "未知目标" : "host-" + id,
                    .inboundType = index == 0 ? "tun" : "mixed",
                    .network = "tcp", .chains = "DIRECT",
                    .rule = index == 0 ? "rule_set=cf_test => route(DIRECT)" : "final",
                    .direct = index == 0};
                // 调用生产行组件，factory 本身没有活动组合上下文。
                return clashflux::ui::ConnectionRowContent(row, "1KB/s", "2KB/s",
                    "10KB", "20KB", compact,
                    huxerui::Button("CLOSE-" + id).OnClick([closed, id] { closed = id; }))
                    .Key("connection-fixture-" + id);
            }).EstimatedItemExtent(90.0F).Controller(scroll)
                .With(huxerui::Grow(1.0F)).Key("connection-fixture-list");
        }
        return huxerui::Column {
          huxerui::Button("ENTER-CONNECTIONS").OnClick([active] { active = !active.Get(); }),
          huxerui::Button("UPDATE-CONNECTIONS").OnClick([shortList] { shortList = !shortList.Get(); }),
          huxerui::Button("SCROLL-CONNECTIONS").OnClick([scroll] { scroll.ScrollToItem(900); }),
          huxerui::Text("CLOSED-" + closed.Get()),
          list,
        }.With(huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    })};
}
const huxerui::Application kConnectionListApplication{ConnectionListRoot};

int diagnosticsStarts = 0, diagnosticsCopies = 0, diagnosticsSaves = 0,
    diagnosticsCloses = 0;
[[huxerui::composable]] huxerui::View PptpProgressFixture() {
    auto opening = huxerui::UseState(true);
    return huxerui::Column{
        huxerui::Button("TOGGLE-PPTP-ACTION").OnClick([opening] { opening = !opening.Get(); }),
        huxerui::Text(clashflux::ui::PptpConnectionProgress(opening.Get())),
        huxerui::Text(clashflux::ui::LocalizedFormat("已关闭 {} 个连接", 1)),
    };
}
huxerui::View PptpProgressRoot() { return huxerui::FlatTheme{PptpProgressFixture()}; }
const huxerui::Application kPptpProgressApplication{PptpProgressRoot};
void testPptpProgressResources() {
    for (auto locale : {"zh", "en", "fr"}) {
        huxerui::testing::UiTest ui(kPptpProgressApplication, {.viewport = {390.0F, 700.0F},
            .resources = {.locale = huxerui::Locale::FromLanguageTag(locale)}, .resource_provider = AppResources()});
        ui.PumpAndSettle();
        const bool zh = std::string_view(locale) == "zh";
        check(ui.Find(UiSelector::Text(zh ? "已关闭 1 个连接" : "Closed 1 connections")).Exists(),
              "PPTP 断开完成反馈从真实资源包解析和插值");
        check(ui.Find(UiSelector::Text(zh ? "连接中…" : "Connecting…")).Exists(),
              "PPTP 连接进度从真实资源包解析");
        ui.Find(UiSelector::Text("TOGGLE-PPTP-ACTION")).Tap(); ui.PumpAndSettle();
        check(ui.Find(UiSelector::Text(zh ? "断开中…" : "Disconnecting…")).Exists(),
              "PPTP 断开进度从真实资源包解析，默认语言回退不抛异常");
        ui.Find(UiSelector::Text("TOGGLE-PPTP-ACTION")).Tap(); ui.PumpAndSettle();
        check(ui.Find(UiSelector::Text(zh ? "连接中…" : "Connecting…")).Exists(),
              "PPTP 断开后可重新进入连接进度");
    }
}
clashflux::ui::TextDocumentHandle documentFixture;
std::shared_ptr<const std::string> documentFixtureSource;
std::shared_ptr<huxerui::Clipboard> documentFixtureClipboard;
bool documentFixtureReadOnly = false;
bool documentFixtureFill = false;
int documentFixtureCompositions = 0;
[[huxerui::composable]] huxerui::View TextDocumentFixture() {
    ++documentFixtureCompositions;
    documentFixtureClipboard = huxerui::UseApplication().Clipboard();
    if (!documentFixture) documentFixture = std::make_shared<clashflux::ui::TextDocument>(documentFixtureSource);
    auto visible = huxerui::UseState(true);
    auto tick = huxerui::UseState(0);
    return huxerui::Column {
      huxerui::Row {
        huxerui::Button("Toggle document").OnClick([visible] { visible = !visible.Get(); }),
        huxerui::Button("Recompose parent").OnClick([tick] { tick += 1; }),
        huxerui::Text(std::to_string(tick.Get())),
      },
      visible.Get() ? clashflux::ui::DocumentTextView(documentFixture, documentFixtureReadOnly,
                                                    documentFixtureFill ? 0.0F : 300.0F)
                    : huxerui::View{huxerui::Text("Document hidden")},
    }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}
huxerui::View TextDocumentFixtureRoot() {
    return huxerui::MaterialTheme(TextDocumentFixture());
}
const huxerui::Application kTextDocumentApplication{TextDocumentFixtureRoot};
void waitForTextDocument(huxerui::testing::UiTest& ui,
                         const clashflux::ui::TextDocumentHandle& document) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (!document->Ready() && document->Error().empty() && std::chrono::steady_clock::now() < deadline) {
        ui.Pump();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    ui.Pump();
    check(document->Ready(), "document worker preparation completes without UI-thread parsing");
}
void testTextDocuments() {
    std::string text;
    for (int line = 0; line < 30000; ++line) text += "rule-" + std::to_string(line) + ": example.com\n";
    text += "中文 😀\n";
    const auto original = std::make_shared<const std::string>(std::move(text));
    documentFixtureReadOnly = false;
    documentFixtureSource = original;
    documentFixture.reset();
    {
        huxerui::testing::UiTest ui(kTextDocumentApplication, {.viewport = {640.0F, 500.0F},
            .resource_provider = AppResources()});
        ui.Pump();
        waitForTextDocument(ui, documentFixture);
        check(documentFixture->Controller().IsConnected(), "large text mounts the production CodeEditor");
        check(documentFixture->Snapshot() == original, "unchanged document snapshots share the original bytes");
        const auto editor = ui.Find(UiSelector::Key("document-editor")).Find(UiSelector::Type<huxerui::Canvas>());
        check(editor.One().bounds.height < 301.0F, "large document keeps a bounded editor viewport");
        editor.Tap();
        documentFixture->Controller().SetCursor(0, 0);
        const int compositions = documentFixtureCompositions;
        editor.EnterText("新增 😀 ");
        ui.Pump();
        check(documentFixtureCompositions == compositions, "typing does not recompose the owning page");
        const auto edited = documentFixture->Snapshot();
        check(edited != original && edited->starts_with("新增 😀 ") && edited->ends_with("中文 😀\n"),
              "explicit snapshot retains the entire large UTF-8 document after editing");
        check(documentFixture->Snapshot() == edited, "repeated save/copy snapshots reuse unchanged text");
        ui.Find(UiSelector::Text("Recompose parent")).Tap();
        ui.Pump();
        check(documentFixture->Snapshot() == edited, "unrelated parent recomposition preserves live text");
        documentFixture->Controller().SetCursor(0, 0);
        editor.Tap();
        documentFixture->Controller().SetCursor(0, 0);
        editor.EnterText("before-unmount ");
        if (ui.Find(UiSelector::Key("action-menu-surface")).Exists()) {
            ui.PressKey(huxerui::Key::Escape);
            ui.Pump();
        }
        ui.Find(UiSelector::Text("Toggle document")).Tap();
        ui.Pump();
        check(ui.Find(UiSelector::Text("Document hidden")).Exists(), "hide action removes the document viewport");
        check(!documentFixture->Controller().IsConnected(), "unmounted document disconnects the controller");
        check(documentFixture->Snapshot()->starts_with("before-unmount "),
              "retained document preserves unsaved edits after editor release");
        ui.Find(UiSelector::Text("Toggle document")).Tap();
        ui.Pump();
        check(documentFixture->Controller().Text().starts_with("before-unmount "),
              "remount restores unsaved text without truncating the document");
    }
    documentFixtureReadOnly = true;
    documentFixtureSource = original;
    documentFixture.reset();
    {
        huxerui::testing::UiTest ui(kTextDocumentApplication, {.viewport = {390.0F, 500.0F},
            .resource_provider = AppResources()});
        ui.Pump();
        waitForTextDocument(ui, documentFixture);
        const auto editor = ui.Find(UiSelector::Key("document-editor")).Find(UiSelector::Type<huxerui::Canvas>());
        editor.Tap();
        documentFixture->Controller().SelectAll();
        check(documentFixture->Controller().Copy(), "read-only editor supports selection and copy through the fork fix");
        check(documentFixtureClipboard->ReadText() == std::optional<std::string>{*original},
              "toolbar copy writes actual selected text before any native clipboard action");
        check(!documentFixture->Controller().Cut() && !documentFixture->Controller().Paste(),
              "read-only controller refuses cut and paste");
        check(documentFixture->Controller().Text() == *original, "copy does not change the read-only document");
        ui.Find(UiSelector::Text("Recompose parent")).Tap();
        ui.Pump();
        check(documentFixture->Snapshot() == original, "read-only preview shares its full source across recomposition");
        ui.Find(UiSelector::Key("document-search-toggle")).Tap();
        check(ui.Find(UiSelector::Key("document-search-input")).Exists(), "shared document search opens");
        ui.Find(UiSelector::Key("document-search-close")).Tap();
        check(!ui.Find(UiSelector::Key("document-search-input")).Exists(), "shared document search closes");
        ui.Find(UiSelector::Key("document-search-toggle")).Tap();
        check(ui.Find(UiSelector::Key("document-search-input")).Exists(), "search reopens on the first click after closing");
    }
    documentFixture.reset();
    documentFixtureFill = true;
    {
        huxerui::testing::UiTest ui(kTextDocumentApplication, {.viewport = {320.0F, 700.0F},
            .resource_provider = AppResources()});
        ui.Pump();
        waitForTextDocument(ui, documentFixture);
        const auto retained = documentFixture;
        for (const auto size : {huxerui::Size{320, 360}, huxerui::Size{960, 700}, huxerui::Size{320, 700}}) {
            ui.SetWindowMetrics({.viewport = size}); ui.PumpAndSettle();
            const auto bounds = ui.Find(UiSelector::Key("document-editor")).One().bounds;
            check(bounds.width <= size.width && bounds.height > size.height - 160.0F &&
                  std::abs(bounds.y + bounds.height - size.height) < 1.0F,
                  "full-page JSON preview fills the bounded remaining height across resize");
            check(documentFixture == retained && documentFixture->Snapshot() == original,
                  "resizing the read-only preview preserves its document and original content");
        }
    }
    documentFixtureFill = false;
    documentFixture.reset();
    documentFixtureReadOnly = false;
    documentFixtureSource = std::make_shared<const std::string>("original\r\n中文 😀\n");
    {
        huxerui::testing::UiTest ui(kTextDocumentApplication, {.viewport = {640.0F, 500.0F},
            .resource_provider = AppResources()});
        ui.Pump();
        waitForTextDocument(ui, documentFixture);
        const auto& controller = documentFixture->Controller();
        documentFixtureClipboard->WriteText("pasted 😀");
        controller.SelectAll();
        check(controller.Paste(), "toolbar paste works without an earlier native clipboard action");
        check(*documentFixture->Snapshot() == "pasted 😀", "paste emits change notification and updates the save snapshot");
        controller.SelectAll();
        check(controller.Cut(), "toolbar cut succeeds only after writing the selection to clipboard");
        check(documentFixtureClipboard->ReadText() == std::optional<std::string>{"pasted 😀"} &&
              documentFixture->Snapshot()->empty(), "cut preserves selected bytes on clipboard and saves the edited document");
    }
    documentFixture.reset();
    documentFixtureClipboard.reset();
}

bool diagnosticsFixtureDark = false;
bool diagnosticsFixtureCanSave = true;
bool diagnosticsFixtureSaving = false;
std::string diagnosticsFixtureReport = "redacted-report";
[[huxerui::composable]] huxerui::View DiagnosticsFixture() {
    auto target = huxerui::UseState(huxerui::TextEditingValue{});
    auto busy = huxerui::UseState(false);
    return clashflux::ui::RuntimeDiagnosticsContent(target, busy.Get(), true, std::make_shared<const std::string>(diagnosticsFixtureReport),
        [busy] { ++diagnosticsStarts; busy = true; }, [busy] { busy = false; },
        [] { ++diagnosticsCopies; }, [] { ++diagnosticsCloses; },[] { ++diagnosticsSaves; },
        diagnosticsFixtureCanSave, diagnosticsFixtureSaving);
}
huxerui::View DiagnosticsFixtureRoot() {
    auto spec = diagnosticsFixtureDark ? huxerui::MaterialDarkThemeSpec() : huxerui::MaterialLightThemeSpec();
    spec.colors = diagnosticsFixtureDark ? clashflux::ui::FluxDarkColors()
                                        : clashflux::ui::FluxLightColors();
    auto definition = huxerui::MaterialThemeDefinition(spec);
    definition.Set(clashflux::ui::ResolveFluxButtonStyle(spec));
    return huxerui::Theme(definition, DiagnosticsFixture());
}
const huxerui::Application kDiagnosticsApplication{DiagnosticsFixtureRoot};
void testDiagnosticsContent() {
    for (auto locale : {"zh", "en", "fr"}) { // fr exercises default resource fallback
        diagnosticsStarts = diagnosticsCopies = diagnosticsSaves = 0;
        huxerui::testing::UiTest ui(kDiagnosticsApplication, {.viewport = {390.0F, 800.0F},
            .resources = {.locale = huxerui::Locale::FromLanguageTag(locale)}, .resource_provider = AppResources()});
        ui.PumpAndSettle();
        const bool zh = std::string_view(locale) == "zh";
        check(ui.Find(UiSelector::Text(zh ? "运行诊断" : "Runtime diagnostics")).Exists(), "运行诊断标题及默认语言资源可实际挂载");
        ui.Find(UiSelector::Text(zh ? "复制脱敏报告" : "Copy redacted report")).Tap(); ui.PumpAndSettle();
        check(diagnosticsCopies == 1, "诊断报告复制回调可用");
        ui.Find(UiSelector::Text(zh ? "保存脱敏报告" : "Save redacted report")).Tap();ui.PumpAndSettle();
        check(diagnosticsSaves==1,"脱敏报告保存入口在中英文及默认回退资源中可用");
        ui.Find(UiSelector::Text(zh ? "开始诊断" : "Run diagnostics")).Tap(); ui.Pump(kStep);
        check(diagnosticsStarts == 1, "开始诊断回调可用");
        check(ui.Find(UiSelector::Text(zh ? "正在收集诊断证据…" : "Collecting diagnostic evidence…")).Exists(), "诊断过程中加载资源安全并展示状态");
        // 框架对禁用控件的 Tap 直接抛错，用 enabled 断言防重复入口。
        check(!ui.Find(UiSelector::Text(zh ? "开始诊断" : "Run diagnostics")).One().enabled,
              "运行中禁用重复诊断");
        check(diagnosticsStarts == 1, "运行中不重复启动诊断");
        check(!ui.Find(UiSelector::Text(zh ? "保存脱敏报告" : "Save redacted report")).One().enabled,
              "采集过程中禁止保存未完成报告");
        ui.Find(UiSelector::Text(zh ? "取消" : "Cancel")).Tap(); ui.PumpAndSettle();
        const auto reportReady = [&] {
            return ui.FindSemantics(huxerui::testing::UiSemanticSelector::Value("redacted-report")).Exists();
        };
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!reportReady() && std::chrono::steady_clock::now() < deadline) {
            ui.Pump();
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        check(reportReady() && ui.Find(UiSelector::Text(zh ? "开始诊断" : "Run diagnostics")).One().enabled,
              "取消动作后恢复报告与交互");
    }
}

void testConnectionList() {
    check(clashflux::ui::ConnectionRuleTypes("rule_set=cf_test => route(DIRECT)") == "RULE-SET" &&
          clashflux::ui::ConnectionRuleTypes("domain_suffix=.example port=443 => route(proxy)") ==
              "DOMAIN-SUFFIX + PORT" &&
          clashflux::ui::ConnectionRuleTypes("final => route(DIRECT)") == "FINAL",
          "连接卡片从命中规则提取类型，不展示规则值或路由动作");
    connectionRowsBuilt = 0;
    huxerui::testing::UiTest ui(kConnectionListApplication,
        {.viewport = {390.0F, 700.0F}, .resource_provider = AppResources()});
    ui.UpdateResourceConfiguration({.locale = huxerui::Locale::FromLanguageTag("zh")});
    ui.PumpAndSettle();
    check(connectionRowsBuilt == 0, "未进入连接页不构建连接行");
    ui.Find(UiSelector::Text("ENTER-CONNECTIONS")).Tap(); ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("DIRECT · RULE-SET")).Exists() &&
          ui.Find(UiSelector::Text("tun")).Exists() &&
          ui.Find(UiSelector::Text("↑ 1KB/s")).Exists() &&
          ui.Find(UiSelector::Text("↓ 2KB/s")).Exists() &&
          ui.Find(UiSelector::Text("未知目标")).Exists(), "窄屏生产连接行展示实时流量和缺省字段");
    check(connectionRowsBuilt > 0 && connectionRowsBuilt < 100, "一千条连接仅构造视口附近行");
    ui.Find(UiSelector::Text("CLOSE-0")).Tap(); ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("CLOSED-0")).Exists(), "关闭连接回调保留正确 ID");
    ui.Find(UiSelector::Text("SCROLL-CONNECTIONS")).Tap(); ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("host-900")).Exists(), "滚动后懒构建的窄屏连接行仍能解析资源");
    ui.Find(UiSelector::Text("UPDATE-CONNECTIONS")).Tap(); ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("未知目标")).Exists(), "连接快照缩短后列表正常显示");
    ui.SetWindowMetrics({.viewport = {1080.0F, 700.0F}}); ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("DIRECT · RULE-SET")).Exists(), "宽屏显示实际直连与规则类型");
    ui.SetWindowMetrics({.viewport = {320.0F, 700.0F}}); ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("DIRECT · RULE-SET")).Exists(), "宽屏切回窄屏可重建连接行");
    ui.Find(UiSelector::Text("ENTER-CONNECTIONS")).Tap(); ui.PumpAndSettle();
    ui.Find(UiSelector::Text("ENTER-CONNECTIONS")).Tap(); ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("DIRECT · RULE-SET")).Exists(), "离开后重进连接页安全挂载");
    ui.UpdateResourceConfiguration({.locale = huxerui::Locale::FromLanguageTag("en")});
    ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("DIRECT · RULE-SET")).Exists() &&
          ui.Find(UiSelector::Text("Unknown destination")).Exists(), "已挂载连接行响应语言切换");
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
            huxerui::Row{}.With(huxerui::Frame{
                .height = clashflux::ui::kDesktopChromeDividerThickness}),
            huxerui::ProvideEnvironment(
                clashflux::ui::SectionTabPickerInsets{clashflux::ui::kDesktopTitleBarHeight +
                    clashflux::ui::kDesktopChromeDividerThickness},
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

void testDesktopTouchpadSectionPager() {
    huxerui::testing::UiTest ui(kApplication, {
        .viewport = {400.0F, 700.0F}, .resource_provider = AppResources(),
    });
    ui.PumpAndSettle();
    const auto scroll = [&ui](huxerui::ScrollInputPhase phase, float delta) {
        return ui.SendScroll({{200.0F, 300.0F}, delta, 0.0F, {},
                              huxerui::ScrollInputKind::Touchpad, phase});
    };
    static_cast<void>(scroll(huxerui::ScrollInputPhase::Begin, 0.0F));
    check(scroll(huxerui::ScrollInputPhase::Update, 240.0F).x > 0.0F,
          "桌面触摸板横滑由生产分区 Pager 消费");
    static_cast<void>(scroll(huxerui::ScrollInputPhase::End, 0.0F));
    ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("SELECTED-1")).Exists(),
          "桌面触摸板释放后写回分区受控索引");
    static_cast<void>(ui.SendScroll({{200.0F, 300.0F}, 240.0F, 0.0F}));
    ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("SELECTED-1")).Exists(),
          "离散滚轮不触发分区翻页");
}

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
    const std::regex pattern{(channel.empty() ? "" : channel + R"(\s+\{\s+)") +
        R"(text\s+\{\s+bounds\s+\{[^}]*\}\s+content\s+\{\s+text=")" + text +
        R"("[\s\S]*?foreground\s+\{\s+red=([\d.-]+)\s+green=([\d.-]+)\s+blue=([\d.-]+)\s+alpha=([\d.-]+))"};
    std::smatch match;
    check(std::regex_search(snapshot, match, pattern), "标签实际绘制颜色可读取");
    if (match.empty()) return {};
    return {std::stof(match[1]), std::stof(match[2]), std::stof(match[3]), std::stof(match[4])};
}
huxerui::Color labelColor(huxerui::testing::UiTest& ui, int index) {
    return paintedTextColor(ui, label(index));
}

void testDiagnosticsThemeAndLayout() {
    using namespace clashflux::ui;
    const auto composite = [](huxerui::Color foreground, huxerui::Color background) {
        const float a = foreground.alpha;
        return huxerui::Color{foreground.red * a + background.red * (1 - a),
            foreground.green * a + background.green * (1 - a),
            foreground.blue * a + background.blue * (1 - a), 1.0F};
    };
    const auto contrast = [](huxerui::Color a, huxerui::Color b) {
        const float x = ThemeColorLuminance(a), y = ThemeColorLuminance(b);
        return (std::max(x, y) + 0.05F) / (std::min(x, y) + 0.05F);
    };
    for (bool dark : {false, true}) {
        diagnosticsFixtureDark = dark;
        for (const auto width : {320.0F, 390.0F, 960.0F}) {
            for (const auto locale : {"zh", "en"}) {
                const bool zh = std::string_view(locale) == "zh";
                diagnosticsFixtureReport.clear();
                huxerui::testing::UiTest ui(kDiagnosticsApplication,
                    {.viewport = {width, 800.0F},
                     .resources = {.locale = huxerui::Locale::FromLanguageTag(locale)},
                     .resource_provider = AppResources()});
                ui.PumpAndSettle();
                check(!ui.Find(UiSelector::Key("diagnostics-report")).Exists(),
                      "empty diagnostics does not reserve a blank report viewport");
                const auto copyLabel = zh ? "复制脱敏报告" : "Copy redacted report";
                const auto saveLabel = zh ? "保存脱敏报告" : "Save redacted report";
                const auto copy = ui.Find(UiSelector::Text(copyLabel)).One();
                const auto save = ui.Find(UiSelector::Text(saveLabel)).One();
                check(!copy.enabled && !save.enabled,
                      "empty report disables export actions without hiding their labels");
                check(copy.bounds.x >= 0 && save.bounds.x >= 0 &&
                          copy.bounds.x + copy.bounds.width <= width + 0.5F &&
                          save.bounds.x + save.bounds.width <= width + 0.5F &&
                          (copy.bounds.y + copy.bounds.height <= save.bounds.y + 0.5F ||
                           copy.bounds.x + copy.bounds.width <= save.bounds.x + 0.5F),
                      "Chinese and English export buttons wrap without clipping or overlap");
                const auto colors = dark ? FluxDarkColors() : FluxLightColors();
                auto spec = huxerui::MaterialLightThemeSpec(); spec.colors = colors;
                const auto style = ResolveFluxButtonStyle(spec);
                const auto disabledFill = std::get<huxerui::Color>(
                    std::get<huxerui::Brush>(style.disabled_background.Get()).Get());
                const auto buttonSurface = composite(disabledFill, colors.surface_container_highest);
                const auto disabledText = paintedTextColor(ui, copyLabel, "");
                check(contrast(composite(disabledText, buttonSurface), buttonSurface) >= 3.0F,
                      "actual disabled diagnostic labels remain readable in light and dark themes");
                ui.Find(UiSelector::Text(zh ? "开始诊断" : "Run diagnostics")).Tap();
                ui.Pump(kStep);
                check(ui.Find(UiSelector::Text(zh ? "取消" : "Cancel")).One().enabled,
                      "running diagnostics keeps cancellation enabled");
                check(!ui.Find(UiSelector::Text(copyLabel)).One().enabled,
                      "running diagnostics cannot export partial reports");
                ui.Find(UiSelector::Text(zh ? "取消" : "Cancel")).Tap(); ui.PumpAndSettle();
                check(ui.Find(UiSelector::Text(zh ? "开始诊断" : "Run diagnostics")).One().enabled,
                      "cancelled diagnostics can be started again");
            }
        }
    }
    diagnosticsFixtureDark = true;
    diagnosticsFixtureReport = std::string(4000, 'x');
    diagnosticsFixtureCanSave = false;
    {
        huxerui::testing::UiTest ui(kDiagnosticsApplication,
            {.viewport = {390.0F, 800.0F}, .resource_provider = AppResources()});
        ui.PumpAndSettle();
        check(ui.Find(UiSelector::Key("diagnostics-report")).One().bounds.height <= 200.5F,
              "long diagnostic report remains in a bounded scroll viewport");
        check(!ui.Find(UiSelector::Text("Save redacted report")).One().enabled &&
                  ui.Find(UiSelector::Text("Copy redacted report")).One().enabled,
              "platforms without save capability retain copying");
    }
    diagnosticsFixtureCanSave = true;
    {
        diagnosticsCloses = 0;
        huxerui::testing::UiTest ui(kDiagnosticsApplication,
            {.viewport = {320.0F, 400.0F}, .resource_provider = AppResources()});
        ui.PumpAndSettle();
        ui.Find(UiSelector::Key("runtime-diagnostics-content"))
            .ScrollUntil(UiSelector::Text("Close")).Tap();
        ui.PumpAndSettle();
        check(diagnosticsCloses == 1,
              "short diagnostic dialogs can scroll to and activate the close action");
    }
    diagnosticsFixtureSaving = true;
    {
        huxerui::testing::UiTest ui(kDiagnosticsApplication,
            {.viewport = {390.0F, 800.0F}, .resource_provider = AppResources()});
        ui.PumpAndSettle();
        check(!ui.Find(UiSelector::Text("Saving…")).One().enabled,
              "in-flight diagnostic export shows saving state and prevents duplicate saves");
    }
    diagnosticsFixtureDark = diagnosticsFixtureSaving = false;
    diagnosticsFixtureReport = "redacted-report";
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

huxerui::View UnifiedLogHistoryRoot() {
    return huxerui::FlatTheme{huxerui::Scope([] {
        auto entries = huxerui::UseStateList<clashflux::ui::LogEntry>();
        huxerui::Lifecycle([entries] {
            using clashflux::ui::AppendLogEntry;
            AppendLogEntry(entries, {"application-new", 1, 20}, 3);
            AppendLogEntry(entries, {"core-old", 1, 10}, 3);
            AppendLogEntry(entries, {"core-late", 2, 15}, 3);
            AppendLogEntry(entries, {"application-same-second", 3, 20}, 3);
            return [] {};
        }, 0);
        const std::vector<clashflux::ui::LogEntry> snapshot(entries.begin(), entries.end());
        return huxerui::Column {
            huxerui::Text(clashflux::ui::ExportLogText(snapshot, 0)),
            huxerui::Text(clashflux::ui::ExportLogText(snapshot, 2)),
            huxerui::Text(clashflux::ui::ExportLogText(snapshot, 0, "application")),
            huxerui::Button("CLEAR-UNIFIED").OnClick([entries] { entries.Clear(); }),
            huxerui::Text(entries.Empty() ? "UNIFIED-EMPTY" : "UNIFIED-READY"),
        };
    })};
}
const huxerui::Application kUnifiedLogHistoryApplication{UnifiedLogHistoryRoot};

void testFilteredRuleIndices() {
    clashflux::ui::FilteredIndicesCache cache;
    constexpr std::size_t count = 100000;
    std::size_t visits = 0;
    const auto match = [&visits](std::size_t index) { ++visits; return index % 10000 == 0; };
    const auto all = cache.Resolve(1, count, "", match);
    check(all.Size() == count && !all.matches && all.SourceIndex(count - 1) == count - 1 && visits == 0,
          "十万条规则空搜索直接映射原表，不扫描、不分配全量索引");
    const auto filtered = cache.Resolve(1, count, "match", match);
    for (int frame = 0; frame < 100; ++frame) {
        const auto next = cache.Resolve(1, count, "match", match);
        check(next.matches == filtered.matches, "无关重组共享同一筛选索引快照");
    }
    check(visits == count && filtered.Size() == 10 && filtered.SourceIndex(9) == 90000,
          "搜索只在内容或查询变化时扫描，保持来源索引顺序");
    const auto changed = cache.Resolve(2, count, "match", [](std::size_t index) { return index == 99999; });
    check(changed.Size() == 1 && changed.SourceIndex(0) == 99999,
          "同样行数下的编辑和重排仍按内容修订刷新筛选");
    const auto cleared = cache.Resolve(2, count, "", match);
    check(!cleared.matches && cleared.Size() == count, "清空搜索恢复原列表映射");
}

std::size_t ruleFixtureBuilt = 0;
huxerui::View LargeRuleListRoot() {
    return huxerui::FlatTheme{huxerui::Scope([] {
        const auto scroll = huxerui::UseScrollController();
        const auto section = huxerui::UseState<std::size_t>(0);
        const auto motion = clashflux::ui::UseSectionTabMotion();
        const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
        clashflux::ui::FilteredIndicesCache cache;
        const auto indices = cache.Resolve(1, 100000, "", [](std::size_t) { return true; });
        huxerui::View list = huxerui::VirtualList(indices.Size(), [indices, compact](std::size_t index) {
            const auto source = indices.SourceIndex(index);
            return huxerui::Scope([source, compact] {
                ++ruleFixtureBuilt;
                const auto text = [](std::string value) {
                    return huxerui::Text(value).Style(huxerui::TextStyle{huxerui::Font::Monospace(13.0F)});
                };
                huxerui::View fields = compact
                    ? huxerui::View{huxerui::Column{text("subscription"), text("DOMAIN-SUFFIX · DIRECT"),
                        text("rule-" + std::to_string(source) + ".example.com")}}
                    : huxerui::View{huxerui::Row{
                        text("subscription").With(huxerui::Frame{.width = 180.0F}),
                        text("DOMAIN-SUFFIX").With(huxerui::Frame{.width = 145.0F}),
                        text("rule-" + std::to_string(source) + ".example.com").With(huxerui::Grow(1.0F)),
                        text("DIRECT").With(huxerui::Frame{.width = 180.0F})}};
                // UnifiedListRow is itself composable: keep both Scope boundaries.
                return huxerui::Scope([fields, source] {
                    return huxerui::Column{
                        huxerui::View{fields}.With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(8.0F, 6.0F)),
                            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
                        huxerui::Divider(),
                    }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
                        .Key("rule-fixture-" + std::to_string(source));
                });
            });
        }).EstimatedItemExtent(compact ? 86.0F : 42.0F).Controller(scroll).With(huxerui::Grow(1.0F));
        std::vector<huxerui::View> pages{
            huxerui::Column{huxerui::Text("HEADERS"), huxerui::Divider(), list}
                .With(huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)).Key("subscription"),
            huxerui::Column{huxerui::Text("GLOBAL")}.With(huxerui::Grow(1.0F)).Key("global"),
        };
        huxerui::View listPage = huxerui::Column {
            huxerui::Button("JUMP-RULE").OnClick([scroll] { scroll.ScrollToItem(90000); }),
            clashflux::ui::SectionTabBar({{"subscription", "Subscription"}, {"global", "Global"}},
                section.Get() == 0 ? "subscription" : "global",
                [section](const std::string& key) { section = key == "global" ? 1 : 0; }, motion),
            clashflux::ui::SectionTabPages(pages, section.Get(), [section](std::size_t next) { section = next; }, motion),
        }.With(huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
        // Match RulesPage's effect sibling and composable scaffold boundary.
        const huxerui::View scaffold = huxerui::Scope([listPage] { return listPage; });
        listPage = huxerui::Column{huxerui::Scope([] { return huxerui::Row{}; }), scaffold}
            .With(huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
        return huxerui::IndexedPages(std::vector<huxerui::View>{listPage, huxerui::Row{}}, 0)
            .With(huxerui::Grow(1.0F));
    })};
}
const huxerui::Application kLargeRuleListApplication{LargeRuleListRoot};
void testLargeRuleList() {
    for (const auto viewport : {huxerui::Size{1000.0F, 700.0F}, huxerui::Size{390.0F, 700.0F}}) {
        ruleFixtureBuilt = 0;
        huxerui::testing::UiTest ui(kLargeRuleListApplication, {.viewport = viewport, .resource_provider = AppResources()});
        ui.PumpAndSettle();
        check(ruleFixtureBuilt < 200, "十万规则在分页、多列布局内仍只构建视口及缓存区行");
        const auto initialBuilt = ruleFixtureBuilt;
        for (int frame = 0; frame < 60; ++frame) ui.Pump(std::chrono::milliseconds(16));
        check(ruleFixtureBuilt == initialBuilt, "规则页连续帧不重复构建静止列表行");
        ui.Find(UiSelector::Text("JUMP-RULE")).Tap();
        ui.PumpAndSettle();
        check(ui.Find(UiSelector::Key("rule-fixture-90000")).Exists() && ruleFixtureBuilt < 400,
              "跳至第九万条仍保持有界虚拟化");
    }
}

void testLogActions() {
    {
        huxerui::testing::UiTest ui(kUnifiedLogHistoryApplication, {.viewport = {600.0F, 500.0F}});
        ui.PumpAndSettle();
        check(ui.Find(UiSelector::Text("core-late\napplication-new\napplication-same-second\n")).Exists(),
              "合并日志按时间排列，晚到行插入正确位置，同秒保留接收顺序且只留最新行");
        check(ui.Find(UiSelector::Text("core-late\n")).Exists() &&
              ui.Find(UiSelector::Text("application-new\napplication-same-second\n")).Exists(),
              "统一列表的级别与搜索筛选作用于两种来源");
        ui.Find(UiSelector::Text("CLEAR-UNIFIED")).Tap();
        ui.PumpAndSettle();
        check(ui.Find(UiSelector::Text("UNIFIED-EMPTY")).Exists(), "清空统一列表后无来源缓存残留");
    }
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
        auto spec = dark.Get() ? huxerui::MaterialDarkThemeSpec() : huxerui::MaterialLightThemeSpec();
        spec.colors = dark.Get() ? clashflux::ui::FluxDarkColors() : clashflux::ui::FluxLightColors();
        return huxerui::Theme(huxerui::MaterialThemeDefinition(spec), huxerui::Scope([dark] {
          const auto& theme = huxerui::UseTheme();
          return clashflux::ui::DesktopTitleBarBody(huxerui::Column{
            huxerui::Button("Toggle theme").OnClick([dark] { dark = !dark.Get(); }),
            clashflux::ui::EmptyState("Theme body", huxerui::ImageResource{"app", "images/request"}),
          }.With(huxerui::Grow(1.0F)), theme);
        }));
    });
}
const huxerui::Application kThemeSwitchApplication{ThemeSwitchRoot, {.show_debug_overlay = false}};
float titleShadowOpacity(huxerui::testing::UiTest& ui) {
    const auto snapshot = ui.CaptureSnapshot().ToString();
    const std::regex shadow{
        R"(shadow\s+\{\s+bounds\s+\{\s+x=0\.000000\s+y=-40\.000000\s+width=[\d.]+\s+height=40\.000000\s+\}\s+color\s+\{[^}]*alpha=([\d.]+))"};
    std::smatch match;
    check(std::regex_search(snapshot, match, shadow), "标题栏实际绘制原生投影");
    return match.empty() ? -1.0F : std::stof(match[1]);
}
void testThemeSwitch() {
    auto semanticDark = huxerui::MaterialDarkThemeSpec();
    semanticDark.colors.background = huxerui::Color::White();
    auto semanticLight = huxerui::MaterialLightThemeSpec();
    semanticLight.colors.background = huxerui::Color::Black();
    check(clashflux::ui::IsDarkTheme(semanticDark) && !clashflux::ui::IsDarkTheme(semanticLight),
          "主题语义来自框架 brightness，不受单个自定义底色亮度影响");
    huxerui::testing::UiTest ui(kThemeSwitchApplication, {
        .viewport = {400.0F, 700.0F}, .resource_provider = AppResources()});
    ui.PumpAndSettle();
    check(std::abs(titleShadowOpacity(ui) - 0.06F) < 0.001F,
          "浅色标题栏使用轻柔投影");
    const auto edge = ui.Find(UiSelector::Key("desktop-title-bar-shadow")).One().bounds;
    check(std::abs(edge.width - 400.0F) < 0.5F && std::abs(edge.height - 5.0F) < 0.5F,
          "投影覆盖内容宽度且不占用页面布局高度");
    check(colorDistance(paintedTextColor(ui, "Theme body", "content"), clashflux::ui::FluxLightColors().on_surface_variant) < 0.001F,
          "通用组件使用浅色主题变量");
    ui.Find(UiSelector::Text("Toggle theme")).Tap(); ui.PumpAndSettle();
    check(std::abs(titleShadowOpacity(ui) - 0.035F) < 0.001F,
          "深色主题立即降低投影强度，覆盖层不阻挡按钮点击");
    check(colorDistance(paintedTextColor(ui, "Theme body", "content"), clashflux::ui::FluxDarkColors().on_surface_variant) < 0.001F,
          "一次点击切为深色，已挂载组件绘制色同步切换");
    ui.Find(UiSelector::Text("Toggle theme")).Tap(); ui.PumpAndSettle();
    check(std::abs(titleShadowOpacity(ui) - 0.06F) < 0.001F,
          "恢复浅色后投影无旧主题残留");
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

struct HomeGridFixture {
    huxerui::State<bool> populated{false};
};
std::shared_ptr<HomeGridFixture> homeGridFixture;
bool homeGridDesktop = true;
bool homeGridControl = false;

void InstallHomeGridFixture(huxerui::ApplicationContext& context) {
    homeGridFixture = std::make_shared<HomeGridFixture>();
    context.Provide(homeGridFixture);
}

huxerui::View HomeGridRoot() {
    return huxerui::FlatTheme{huxerui::Scope([] {
        using namespace clashflux::ui;
        const auto model = huxerui::UseService<HomeGridFixture>();
        const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
        std::vector<huxerui::View> cards;
        for (const auto& spec : HomeCardCatalog(homeGridDesktop)) {
            huxerui::View body = huxerui::Text(model->populated.Get() ? "Live " + std::string(spec.id) : "—");
            cards.push_back(huxerui::Column {body}.With(huxerui::Padding(16.0F))
                .LayoutValue<HomeCardSpan>({spec.width, spec.height})
                .Key("home-card-" + std::string(spec.id)));
        }
        if (homeGridDesktop && homeGridControl && !compact) {
            cards.push_back(huxerui::Row {}.LayoutValue<HomeCardSpan>({1, 1})
                .LayoutValue<HomeControlSpan>(true).Key("home-card-control"));
        }
        return huxerui::ScrollView(HomeGrid(cards).LayoutValue<HomeGridCompact>(compact))
            .With(huxerui::Grow(1.0F)).Key("home-grid-viewport");
    })};
}
const huxerui::Application kHomeGridApplication{HomeGridRoot,
    {.show_debug_overlay = false, .application_hooks = {InstallHomeGridFixture}}};

void testFixedHomeGeometry() {
    using namespace clashflux::ui;
    for (const bool desktop : {false, true}) {
        homeGridDesktop = desktop;
        homeGridControl = desktop;
        huxerui::testing::UiTest ui(kHomeGridApplication, {.viewport = {960.0F, 900.0F}});
        const auto populated = homeGridFixture->populated;
        for (const float width : {320.0F, 960.0F, 1400.0F}) {
            populated = false;
            ui.SetWindowMetrics({.viewport = {width, 900.0F}}); ui.PumpAndSettle();
            const auto catalog = HomeCardCatalog(desktop);
            check(catalog.size() == (desktop ? 7U : 5U), "home uses the actual platform's fixed card count");
            std::vector<huxerui::Rect> before;
            for (const auto& spec : catalog) {
                const auto rect = ui.Find(UiSelector::Key("home-card-" + std::string(spec.id))).One().bounds;
                before.push_back(rect);
                check(std::abs(rect.height - (spec.height == 1 ? 88.0F : 188.0F)) < 0.01F,
                      "home retains the fixed 88pt/188pt card heights");
            }
            const auto traffic = before[0];
            const auto total = before[1];
            const auto mode = before[2];
            check(std::abs(total.width - mode.width) < 0.01F &&
                  std::abs(traffic.width - (total.width * 2.0F + 12.0F)) < 0.01F,
                  "traffic spans two real columns and keeps the 12pt gap");
            if (width < 600.0F) {
                check(std::abs(total.y - traffic.y - 200.0F) < 0.01F &&
                      std::abs(mode.x - total.x - total.width - 12.0F) < 0.01F,
                      "compact home places total/mode side by side below traffic");
            } else {
                check(std::abs(total.y - traffic.y) < 0.01F &&
                      std::abs(total.x - traffic.x - traffic.width - 12.0F) < 0.01F,
                      "wide home retains the fixed traffic/total first row");
            }
            std::optional<huxerui::Rect> control;
            if (desktop && width >= 600.0F) {
                control = ui.Find(UiSelector::Key("home-card-control")).One().bounds;
                check(std::abs(control->height - 88.0F) < 0.01F &&
                      std::abs(control->width - (width >= 1200.0F ? traffic.width : total.width)) < 0.01F,
                      "running control placeholder preserves its responsive one/two-column span");
            }
            const auto viewport = ui.Find(UiSelector::Key("home-grid-viewport")).One().bounds;
            populated = true; ui.PumpAndSettle();
            for (std::size_t index = 0; index < catalog.size(); ++index) {
                check(ui.Find(UiSelector::Key("home-card-" + std::string(catalog[index].id))).One().bounds == before[index],
                      "hydration changes card interiors without changing their actual positions or dimensions");
            }
            check(ui.Find(UiSelector::Key("home-grid-viewport")).One().bounds == viewport,
                  "home hydration preserves the scroll viewport");
            if (control) check(ui.Find(UiSelector::Key("home-card-control")).One().bounds == *control,
                               "home hydration preserves the known running control slot");
        }
    }
}

struct LoadingSkeletonFixture {
    huxerui::State<bool> ready{false};
    huxerui::State<bool> active{true};
    huxerui::State<bool> reduced{false};
    huxerui::State<bool> dark{false};
    huxerui::State<int> kind{0};
    int compositions = 0;
};
std::shared_ptr<LoadingSkeletonFixture> loadingSkeletonFixture;
bool skeletonInitiallyReady = false;

void InstallLoadingSkeletonFixture(huxerui::ApplicationContext& context) {
    loadingSkeletonFixture = std::make_shared<LoadingSkeletonFixture>();
    loadingSkeletonFixture->ready = skeletonInitiallyReady;
    context.Provide(loadingSkeletonFixture);
}

huxerui::View LoadingSkeletonRoot() {
    return huxerui::Scope([] {
        const auto model = huxerui::UseService<LoadingSkeletonFixture>();
        auto theme = model->dark.Get() ? clashflux::ui::FluxDarkColors() : clashflux::ui::FluxLightColors();
        auto spec = huxerui::FlatLightThemeSpec();
        spec.colors = theme;
        spec.motion.reduced_motion = model->reduced.Get();
        return huxerui::FlatTheme(spec, huxerui::Scope([model] {
            ++model->compositions;
            if (model->ready.Get()) return huxerui::View{huxerui::Text("SKELETON-READY")};
            if (!model->active.Get()) return huxerui::View{huxerui::Text("SKELETON-INACTIVE")};
            const auto& current = huxerui::UseTheme();
            return model->kind.Get() == 2 ? clashflux::ui::ProxyLoadingSkeleton(current)
                : clashflux::ui::PageLoadingSkeleton(current, model->kind.Get() == 1);
        }));
    });
}
const huxerui::Application kLoadingSkeletonApplication{LoadingSkeletonRoot,
    {.show_debug_overlay = false, .application_hooks = {InstallLoadingSkeletonFixture}}};

void testSkeletonShimmer() {
    skeletonInitiallyReady = false;
    huxerui::testing::UiTest ui(kLoadingSkeletonApplication, {.viewport = {960.0F, 600.0F}});
    ui.UpdateWindowLifecycleState(huxerui::WindowLifecycleState::Active);
    const auto model = loadingSkeletonFixture;
    for (bool dark : {false, true}) {
        model->dark = dark;
        for (float width : {320.0F, 960.0F}) {
            ui.SetWindowMetrics({.viewport = {width, 600.0F}});
            for (int kind : {0, 1, 2}) {
                model->kind = kind;
                for (int frame = 0; frame < 4; ++frame) ui.Pump(std::chrono::milliseconds(50));
                const char* key = kind == 2 ? "proxy-loading-skeleton" : "page-loading-skeleton";
                const auto bounds = ui.Find(UiSelector::Key(key)).One().bounds;
                const auto before = ui.CaptureSnapshot();
                const int compositions = model->compositions;
                ui.Pump(std::chrono::milliseconds(200));
                check(before != ui.CaptureSnapshot(), "loading placeholders paint a moving gradient in both themes");
                check(model->compositions == compositions && ui.Find(UiSelector::Key(key)).One().bounds == bounds &&
                      bounds.width <= width + 0.5F,
                      "shimmer keeps narrow/wide layout and composition stable for grid, list and compact proxy cards");
            }
        }
    }
    ui.UpdateWindowLifecycleState(huxerui::WindowLifecycleState::Background);
    ui.PumpAndSettle();
    const auto background = ui.CaptureSnapshot();
    ui.Pump(std::chrono::seconds(2));
    check(background == ui.CaptureSnapshot(), "background skeleton stops painting and schedules no recurring work");
    ui.UpdateWindowLifecycleState(huxerui::WindowLifecycleState::Active);
    ui.Pump(std::chrono::milliseconds(100));
    const auto foreground = ui.CaptureSnapshot();
    ui.Pump(std::chrono::milliseconds(200));
    check(foreground != ui.CaptureSnapshot(), "foreground skeleton resumes its shared phase");
    model->reduced = true;
    ui.PumpAndSettle();
    const auto reduced = ui.CaptureSnapshot();
    ui.Pump(std::chrono::seconds(2));
    check(reduced == ui.CaptureSnapshot(), "reduced motion keeps a static skeleton without recurring frames");
    model->reduced = false;
    model->active = false;
    ui.PumpAndSettle();
    check(!ui.Find(UiSelector::Key("proxy-loading-skeleton")).Exists(), "leaving the page retires its shimmer");
    model->active = true;
    ui.Pump(std::chrono::milliseconds(50));
    model->ready = true;
    ui.Pump();
    check(ui.Find(UiSelector::Text("SKELETON-READY")).Exists() &&
          !ui.Find(UiSelector::Key("proxy-loading-skeleton")).Exists(),
          "real data replaces an unfinished shimmer immediately");
    ui.PumpAndSettle();

    skeletonInitiallyReady = true;
    huxerui::testing::UiTest cached(kLoadingSkeletonApplication, {.viewport = {320.0F, 600.0F}});
    cached.UpdateWindowLifecycleState(huxerui::WindowLifecycleState::Active);
    cached.PumpAndSettle();
    check(cached.Find(UiSelector::Text("SKELETON-READY")).Exists() &&
          !cached.Find(UiSelector::Key("page-loading-skeleton")).Exists(),
          "cached first-frame content never mounts a loading animation");
    skeletonInitiallyReady = false;
}

huxerui::View DesktopPageSwitcherRoot() {
    return huxerui::FlatTheme{huxerui::Scope([] {
        auto selected = huxerui::UseState<std::size_t>(0);
        auto ready = huxerui::UseState(false);
        const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
        std::vector<huxerui::View> pages;
        for (int index = 0; index < 3; ++index) {
            pages.push_back(huxerui::Scope([index, ready, selected]() -> huxerui::View {
                auto count = huxerui::UseState(0);
                const auto& theme = huxerui::UseTheme();
                if (index == 1 && !ready.Get())
                    return clashflux::ui::PageLoadingSkeleton(theme, false, selected.Get() == 1);
                return huxerui::Column {
                  huxerui::Text("DESTINATION-" + std::to_string(index)),
                  huxerui::Text("MOTION-" + std::to_string(index) +
                      (theme.motion.reduced_motion ? "-reduced" : "-normal")),
                  huxerui::Button("COUNT-" + std::to_string(index) + "-" + std::to_string(count.Get()))
                      .OnClick([count] { count = count.Get() + 1; }),
                }.With(huxerui::Grow(1.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
            }).Key("destination-" + std::to_string(index)));
        }
        return huxerui::Column {
          huxerui::Row {
            huxerui::Button("First").OnClick([selected] { selected = 0; }),
            huxerui::Button("Middle").OnClick([selected] { selected = 1; }),
            huxerui::Button("Last").OnClick([selected] { selected = 2; }),
            huxerui::Button("Ready").OnClick([ready] { ready = true; }),
          },
          clashflux::ui::DesktopPageSwitcher(pages, selected, compact, huxerui::UseTheme()),
        }.With(huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    })};
}
const huxerui::Application kDesktopPageSwitcherApplication{DesktopPageSwitcherRoot,
    {.show_debug_overlay = false}};

void testDesktopPageSwitcher() {
    huxerui::testing::UiTest ui(kDesktopPageSwitcherApplication, {.viewport = {1000.0F, 700.0F}});
    ui.PumpAndSettle();
    ui.Find(UiSelector::Text("COUNT-0-0")).Tap(); ui.PumpAndSettle();
    for (const char* destination : {"Last", "First", "Last"}) {
        ui.Find(UiSelector::Text(destination)).Tap();
        ui.Pump();
        const auto viewport = ui.Find(UiSelector::Key("desktop-primary-pages")).One().bounds;
        const auto page = ui.Find(UiSelector::Key(destination == std::string("Last")
            ? "destination-2" : "destination-0")).One();
        check(page.in_viewport && std::abs(page.bounds.x - viewport.x) < 0.5F &&
              std::abs(page.bounds.width - viewport.width) < 0.5F,
              "wide desktop switches directly to the selected destination on the first frame");
    }
    ui.Drag({200.0F, 320.0F}, {800.0F, 320.0F}); ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("DESTINATION-2")).One().in_viewport,
          "wide desktop horizontal gestures do not change the primary destination");
    for (float width : {400.0F, 1000.0F, 599.0F, 600.0F}) {
        ui.SetWindowMetrics({.viewport = {width, 700.0F}}); ui.PumpAndSettle();
        ui.Find(UiSelector::Text("First")).Tap(); ui.PumpAndSettle();
        check(ui.Find(UiSelector::Text("COUNT-0-1")).Exists(),
              "resizing across the desktop breakpoint retains each page's local state");
        check(ui.Find(UiSelector::Text("MOTION-0-normal")).Exists(),
              "wide-page switching leaves the page's real motion preference unchanged");
        if (width == 400.0F) {
            ui.Find(UiSelector::Text("Last")).Tap(); ui.Pump(kStep);
            check(ui.Find(UiSelector::Key("destination-2")).One().bounds.x > 1.0F,
                  "desktop Compact retains its animated page selection");
            ui.SetWindowMetrics({.viewport = {1000.0F, 700.0F}}); ui.Pump();
            check(std::abs(ui.Find(UiSelector::Key("destination-2")).One().bounds.x) < 0.5F,
                  "expanding during a Compact transition immediately settles on the target page");
        }
    }
    ui.Find(UiSelector::Text("Middle")).Tap(); ui.Pump();
    const auto skeleton = ui.Find(UiSelector::Key("page-loading-skeleton")).One();
    const auto viewport = ui.Find(UiSelector::Key("desktop-primary-pages")).One().bounds;
    check(skeleton.in_viewport && std::abs(skeleton.bounds.x - viewport.x) < 0.5F &&
          std::abs(skeleton.bounds.width - viewport.width) < 0.5F,
          "an unready destination immediately presents a bounded loading skeleton");
    ui.Pump(std::chrono::seconds(2));
    check(ui.Find(UiSelector::Key("page-loading-skeleton")).One().in_viewport,
          "loading follows readiness instead of a fixed display timer");
    ui.Find(UiSelector::Text("First")).Tap(); ui.Pump();
    ui.Find(UiSelector::Text("Ready")).Tap(); ui.Pump();
    ui.Find(UiSelector::Text("Middle")).Tap(); ui.Pump();
    check(!ui.Find(UiSelector::Key("page-loading-skeleton")).Exists() &&
          ui.Find(UiSelector::Text("DESTINATION-1")).One().in_viewport,
          "data arriving offscreen is shown directly without replaying a skeleton");
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
            huxerui::Column {
                huxerui::Text("BOTTOM").With(huxerui::Frame{.height = 64.0F}),
            }.With(huxerui::SafeAreaPadding{.top = false}));
    })};
}
const huxerui::Application kResponsiveShellApplication{ResponsiveShellRoot,
    {.window = {.content_mode = huxerui::WindowContentMode::EdgeToEdge}, .show_debug_overlay = false}};
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
        const auto bottom = ui.Find(UiSelector::Text("BOTTOM")).One().bounds;
        check(std::abs(content.width - width) < 0.5F && std::abs(content.y - 40.0F) < 0.5F &&
              std::abs(content.y + content.height - 480.0F) < 0.5F,
              "Compact 内容占满窗口栏下方，延伸到悬浮导航后面");
        check(bottom.y > content.y && bottom.y + bottom.height < 480.0F &&
              bottom.x > 0.0F && bottom.x + bottom.width < width,
              "窄屏导航悬浮在内容上方并保留四周间隙");
        const auto fade = ui.Find(UiSelector::Key("compact-navigation-fade")).One().bounds;
        check(std::abs(fade.y - bottom.y - bottom.height * 0.5F) < 0.5F &&
              std::abs(fade.y + fade.height - 480.0F) < 0.5F &&
              std::abs(fade.width - width) < 0.5F,
              "渐隐只从胶囊导航行中线向下覆盖到视口底边");
        check(ui.Find(UiSelector::Text("RETAINED-1")).Exists(), "缩窄窗口保留页面 State");
    }
    for (float inset : {24.0F, 48.0F}) {
        ui.SetWindowMetrics({.viewport = {320.0F, 480.0F}, .safe_area = {.bottom = inset}});
        ui.PumpAndSettle();
        const auto bottom = ui.Find(UiSelector::Text("BOTTOM")).One().bounds;
        const auto fade = ui.Find(UiSelector::Key("compact-navigation-fade")).One().bounds;
        check(std::abs(fade.y - bottom.y - bottom.height * 0.5F) < 0.5F &&
              std::abs(fade.y + fade.height - 480.0F) < 0.5F,
              "底部安全区变化后渐隐起点仍对齐胶囊导航行中线");
    }
    ui.SetWindowMetrics({.viewport = {600.0F, 700.0F}}); ui.PumpAndSettle();
    check(!ui.Find(UiSelector::Key("compact-navigation-fade")).Exists(),
          "宽屏移除胶囊及其渐隐层");
    check(ui.Find(UiSelector::Text("SIDEBAR")).Exists() && !ui.Find(UiSelector::Text("BOTTOM")).Exists() &&
          ui.Find(UiSelector::Text("RETAINED-1")).Exists(), "600pt 恢复桌面布局且页面不重挂载");
}

huxerui::View ProfileFormNavigationRoot() {
    return huxerui::FlatTheme{huxerui::Scope([] {
        const auto model = huxerui::UseState(
            std::make_shared<clashflux::ui::DesktopSettingsNavigationModel>()).Get();
        auto form = huxerui::UseState(0);
        auto active = huxerui::UseState(true);
        auto mounted = huxerui::UseState(true);
        const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
        huxerui::View effects;
        if (mounted.Get()) {
            effects = huxerui::Scope([model, form, active, compact] {
                clashflux::ui::UseCompactProfileFormNavigation(
                    model, active.Get() && compact && form.Get() != 0);
                return huxerui::Row{};
            }).Key("profile-form-effects");
        }
        huxerui::View navigation;
        if (!(active.Get() && model->compactProfileFormOpen.Get())) {
            navigation = huxerui::Text("PROFILE-DOCK")
                .With(huxerui::Frame{.height = 64.0F});
        }
        return clashflux::ui::ResponsiveDesktopShell(huxerui::Column{
            effects,
            huxerui::Button("EDIT").OnClick([form] { form = 1; }),
            huxerui::Button("CREATE").OnClick([form] { form = 2; }),
            huxerui::Button("FILE").OnClick([form] { form = 3; }),
            huxerui::Button("BACK").OnClick([form] { form = 0; }),
            huxerui::Button("SWITCH").OnClick([active] { active = !active.Get(); }),
            huxerui::Button("MOUNT").OnClick([mounted] { mounted = !mounted.Get(); }),
            huxerui::Text("FORM-" + std::to_string(form.Get())),
        }.With(huxerui::Grow(1.0F)),
            huxerui::Text("SIDEBAR").With(huxerui::Frame{.width = 80.0F}),
            huxerui::Text("CHROME").With(huxerui::Frame{.height = 40.0F}), navigation);
    })};
}
const huxerui::Application kProfileFormNavigationApplication{ProfileFormNavigationRoot,
    {.show_debug_overlay = false}};

void testProfileFormNavigation() {
    huxerui::testing::UiTest ui(kProfileFormNavigationApplication,
        {.viewport = {400.0F, 700.0F}});
    ui.PumpAndSettle();
    const auto tap = [&ui](const char* text) {
        ui.Find(UiSelector::Text(text)).Tap();
        ui.PumpAndSettle();
    };
    const auto dock = [&ui] { return ui.Find(UiSelector::Key("compact-navigation")).Exists(); };
    check(dock(), "subscription list shows the compact dock");
    for (const char* entry : {"EDIT", "CREATE", "FILE"}) {
        tap(entry);
        for (float width : {320.0F, 599.0F, 960.0F, 400.0F}) {
            ui.SetWindowMetrics({.viewport = {width, 700.0F}});
            ui.PumpAndSettle();
            check(!dock() && !ui.Find(UiSelector::Key("compact-navigation-scrim")).Exists(),
                  "profile forms hide both dock and scrim across breakpoints");
        }
        tap("SWITCH");
        check(dock(), "switching primary pages restores the dock despite retained form state");
        tap("SWITCH");
        check(!dock(), "returning to the retained profile form hides the dock again");
        tap("BACK");
        check(dock(), "closing a profile form restores the dock");
    }
    tap("EDIT");
    tap("MOUNT");
    check(dock(), "unmounting profile forms clears their dock visibility state");
    tap("MOUNT");
    check(!dock(), "remounting a retained form restores its dock visibility state");
    tap("BACK");
    check(dock(), "closing a local form restores its root-page dock independently of pushed routes");
}

huxerui::View CompactProfileCardsRoot() {
    return huxerui::FlatTheme{huxerui::Scope([] {
        using namespace clashflux::ui;
        const auto& theme = huxerui::UseTheme();
        const SelectableTileColors colors{theme.colors.surface_container,
            theme.colors.on_surface, theme.colors.on_surface_variant, theme.colors.on_surface_variant};
        auto menuCount = huxerui::UseState(0);
        const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
        return huxerui::Column {
          huxerui::Text("MENU-" + std::to_string(menuCount.Get())),
          huxerui::VirtualGrid(100, [colors, compact, menuCount](std::size_t index) {
              const auto suffix = std::to_string(index);
              const std::string traffic = index % 4 == 1 || index % 4 == 3 ? "" : "12 / 100 GB";
              const std::string expiry = index % 4 >= 2 ? "" : "Expires 2026-11-30";
              auto body = CompactProfileCardContent("A very long subscription name that needs ellipsis",
                  "By rules", colors,
                  huxerui::Text("=").With(huxerui::Frame{.width = 20.0F}),
                  huxerui::Row {huxerui::Text("...")}
                      .With(huxerui::Frame{.width = 40.0F, .height = 40.0F},
                            huxerui::Semantics{.role = huxerui::SemanticRole::Button})
                      .OnClick([menuCount] { menuCount = menuCount.Get() + 1; }).Key("profile-more"),
                  CompactProfileTrafficContent(traffic, expiry,
                      ProfileTrafficProgress(traffic.empty() ? 0 : 12, traffic.empty() ? 0 : 100), colors.muted));
              return huxerui::Column {body}.With(
                  huxerui::Frame{.height = ProfileCardHeight(compact)},
                  huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 8.0F)),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
                  .Key("compact-profile-" + suffix);
          }).Columns(huxerui::GridColumns::Fixed(1))
              .EstimatedRowExtent(ProfileCardHeight(compact))
              .RowSpacing(compact ? kCompactProfileCardSpacing : kSectionCardSpacing)
              .With(huxerui::Grow(1.0F)).Key("compact-profile-grid"),
        }.With(huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    })};
}
const huxerui::Application kCompactProfileCardsApplication{CompactProfileCardsRoot,
    {.show_debug_overlay = false}};

void testCompactProfileCards() {
    using namespace clashflux::ui;
    huxerui::testing::UiTest ui(kCompactProfileCardsApplication, {.viewport = {320.0F, 700.0F}});
    ui.PumpAndSettle();
    for (float width : {320.0F, 599.0F, 960.0F, 400.0F, 320.0F}) {
        ui.SetWindowMetrics({.viewport = {width, 700.0F}});
        ui.PumpAndSettle();
        std::optional<float> infoY, barY;
        for (int index = 0; index < 4; ++index) {
            const auto card = ui.Find(UiSelector::Key("compact-profile-" + std::to_string(index)));
            const auto bounds = card.One().bounds;
            check(std::abs(bounds.height - ProfileCardHeight(width < 600.0F)) < 0.5F,
                  "every quota/expiry combination shares the responsive card height");
            const auto name = card.Find(UiSelector::Key("profile-compact-name")).One().bounds;
            const auto status = card.Find(UiSelector::Key("profile-compact-status")).One().bounds;
            const auto more = card.Find(UiSelector::Key("profile-more")).One().bounds;
            const auto info = card.Find(UiSelector::Key("profile-traffic-label")).One().bounds;
            const auto bar = card.Find(UiSelector::Key("profile-traffic-bar")).One().bounds;
            check(name.x + name.width <= status.x && status.x + status.width <= more.x &&
                  more.x + more.width <= bounds.x + bounds.width,
                  "long names leave status and the more button visible at narrow widths");
            check(bar.y + bar.height <= bounds.y + bounds.height &&
                  std::abs(bar.height - 3.0F) < 0.5F,
                  "the traffic bar stays inside the compact card");
            if (infoY) check(std::abs(info.y - bounds.y - *infoY) < 0.5F &&
                            std::abs(bar.y - bounds.y - *barY) < 0.5F,
                            "empty metadata preserves identical text and progress positions");
            infoY = info.y - bounds.y;
            barY = bar.y - bounds.y;
        }
    }
    ui.Find(UiSelector::Key("compact-profile-0")).Find(UiSelector::Key("profile-more")).Tap();
    ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("MENU-1")).Exists(), "the compact card menu remains clickable");
    const auto grid = ui.Find(UiSelector::Key("compact-profile-grid"));
    grid.ScrollBy({0.0F, 3000.0F});
    ui.PumpAndSettle();
    check(!ui.Find(UiSelector::Key("compact-profile-0")).Exists() ||
          !ui.Find(UiSelector::Key("compact-profile-0")).One().in_viewport,
          "compact cards retain virtual scrolling with the smaller row estimate");
    grid.ScrollBy({0.0F, -3000.0F});
    ui.PumpAndSettle();
    check(ui.Find(UiSelector::Key("compact-profile-0")).One().in_viewport,
          "scrolling back remounts the compact card at its original geometry");
    check(ProfileTrafficProgress(0, 0) == 1.0F && ProfileTrafficProgress(12, 100) == 0.12F,
          "unknown traffic keeps a full placeholder bar while known quota keeps its real fraction");
}

void testProfileListSections() {
    using namespace clashflux::ui;
    struct Profile { std::string type, name; };
    const std::vector<Profile> rows{{"local", "match file"}, {"remote", "match URL"},
        {"custom", "other"}, {"remote", "other"}, {"local", "other"}, {"pptp", "other"}};
    const auto all = [](const Profile&) { return true; };
    const auto mobile = BuildProfileListSections(rows, false, all);
    check(mobile.size() == 1 && !mobile[0].type &&
          mobile[0].indices == std::vector<std::size_t>{0, 1, 2, 3, 4, 5},
          "phone list combines every source type in the authoritative order");
    const auto desktop = BuildProfileListSections(rows, true, all);
    check(desktop.size() == 4 && desktop[0].type == "remote" && desktop[1].type == "local" &&
          desktop[2].type == "pptp" && desktop[3].type == "custom" &&
          desktop[0].indices == std::vector<std::size_t>{1, 3},
          "desktop including narrow windows retains its existing type sections");
    const auto filtered = BuildProfileListSections(rows, false, [](const Profile& row) {
        return row.name.starts_with("match");
    });
    check(filtered.size() == 1 && filtered[0].indices == std::vector<std::size_t>{0, 1},
          "phone search spans remote and local configurations without regrouping");
    check(BuildProfileListSections(rows, false, [](const Profile&) { return false; }).empty() &&
          BuildProfileListSections(std::vector<Profile>{}, false, all).empty(),
          "empty phone searches and inventories produce a genuine empty state");
    check(ProxyObjectDisplayName("Group · original", "Subscription", false) == "Group · original" &&
          ProxyObjectDisplayName("Group · original", "Subscription", true) == "Group · original · Subscription" &&
          ProxyObjectDisplayName("DIRECT", "", true) == "DIRECT",
          "phone proxy names use original catalog names while desktop retains source qualification");
}

huxerui::View UnifiedProfileListRoot() {
    return huxerui::FlatTheme{huxerui::Scope([] {
        using namespace clashflux::ui;
        const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
        return UnifiedProfileList(huxerui::VirtualGrid(100, [compact](std::size_t index) {
            return huxerui::Text((index % 2 ? "remote-" : "local-") + std::to_string(index))
                .With(huxerui::Frame{.height = ProfileCardHeight(compact)}).Key("unified-row-" + std::to_string(index));
        }).Columns(huxerui::GridColumns::Fixed(1))
          .EstimatedRowExtent(ProfileCardHeight(compact))
          .RowSpacing(kCompactProfileCardSpacing).With(huxerui::Grow(1.0F)).Key("unified-grid"));
    })};
}
const huxerui::Application kUnifiedProfileListApplication{UnifiedProfileListRoot,
    {.show_debug_overlay = false}};

void testUnifiedProfileList() {
    huxerui::testing::UiTest ui(kUnifiedProfileListApplication, {.viewport = {320.0F, 700.0F}});
    ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("local-0")).One().in_viewport &&
          ui.Find(UiSelector::Text("remote-1")).One().in_viewport &&
          !ui.Find(UiSelector::Key("section-tab-pages")).Exists(),
          "phone remote and local entries share a visible list without a section Pager");
    ui.Drag({40.0F, 200.0F}, {280.0F, 200.0F});
    ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("local-0")).One().in_viewport,
          "horizontal gestures cannot switch hidden subscription categories");
    ui.Find(UiSelector::Key("unified-grid")).ScrollBy({0.0F, 1000.0F});
    ui.PumpAndSettle();
    for (float width : {400.0F, 960.0F, 320.0F}) {
        ui.SetWindowMetrics({.viewport = {width, 700.0F}});
        ui.PumpAndSettle();
        const auto grid = ui.Find(UiSelector::Key("unified-grid")).One().bounds;
        check(std::abs(grid.x - 4.0F) < 0.5F && std::abs(grid.width - width + 8.0F) < 0.5F,
              "the phone list preserves its existing side insets at every viewport width");
        const auto first = ui.Find(UiSelector::Key("unified-row-0"));
        check(!first.Exists() || !first.One().in_viewport,
              "resizing the unified phone list retains a scrolled position");
        check(!ui.Find(UiSelector::Key("section-tab-pages")).Exists(),
              "wide phones do not reintroduce desktop category paging");
    }
}

huxerui::View HeaderGeometryRoot() {
    return huxerui::FlatTheme{huxerui::Scope([] {
        const auto& theme = huxerui::UseTheme();
        const auto search = [](const char* surfaceKey) {
            return huxerui::Scope([surfaceKey] {
                return clashflux::ui::PageSearchFieldLayout(huxerui::Row{
                    huxerui::Text("SEARCH"), huxerui::Spacer{}, huxerui::Button("ADD"),
                }.With(huxerui::Frame{.height = 40.0F}, huxerui::Grow(1.0F))
                    .Key(surfaceKey),
                    huxerui::UseViewportClass() == huxerui::ViewportClass::Compact);
            }).With(huxerui::Grow(1.0F));
        };
        return huxerui::Column{
            clashflux::ui::PageHeaderLayout(theme, search("search-single-line"), {}, false, true)
                .Key("single-search-header"),
            clashflux::ui::PageHeaderLayout(theme, search("search-with-batch"),
                huxerui::Text("BATCH"), true, true).Key("batch-search-header"),
            huxerui::Row {
              huxerui::Text("TITLE").With(huxerui::Frame{.width = 72.0F}),
              search("search-between-actions"),
              huxerui::Button("ACTION").With(huxerui::Frame{.width = 80.0F})
                  .Key("header-trailing-action"),
            }.With(huxerui::Spacing(12.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
                .Key("desktop-search-header"),
            clashflux::ui::CenteredPageHeader(theme,
                huxerui::Text("CREATE").Key("create-centered-title"),
                huxerui::Button("BACK").With(huxerui::Frame{.width = 32.0F, .height = 32.0F}),
                huxerui::Button("SAVE").With(huxerui::Frame{.width = 40.0F, .height = 40.0F}))
                .Key("create-header"),
            clashflux::ui::CenteredPageHeader(theme,
                huxerui::Text("IMPORTING").Key("busy-centered-title"),
                huxerui::Button("BACK").With(huxerui::Frame{.width = 32.0F, .height = 32.0F}),
                huxerui::Row{}.With(huxerui::Frame{.width = 20.0F, .height = 20.0F}))
                .Key("busy-header"),
        }.With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(8.0F, 0.0F)),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    })};
}
const huxerui::Application kHeaderGeometryApplication{HeaderGeometryRoot, {.show_debug_overlay = false}};
void testHeaderGeometry() {
    huxerui::testing::UiTest ui(kHeaderGeometryApplication, {.viewport = {320.0F, 740.0F}});
    for (float width : {320.0F, 420.0F, 600.0F, 960.0F, 1600.0F, 420.0F}) {
        ui.SetWindowMetrics({.viewport = {width, 740.0F}}); ui.PumpAndSettle();
        for (const char* name : {"search-single-line", "search-with-batch"}) {
            const auto bounds = ui.Find(UiSelector::Key(name)).One().bounds;
            check(std::abs(bounds.height - (width < 600.0F ? 40.0F
                : clashflux::ui::kDesktopTitleBarSearchHeight)) < 0.5F,
                  "搜索栏高度固定，不能消耗整页的剩余高度");
            check(std::abs(bounds.x + bounds.width * 0.5F - width * 0.5F) < 0.5F &&
                  std::abs(bounds.width - (width < 600.0F ? width - 16.0F
                      : clashflux::ui::kDesktopTitleBarSearchWidth)) < 0.5F,
                  "搜索表面居中，桌面封顶 320pt、窄屏恢复可用宽度（含批量动作）");
        }
        const auto searchBounds = ui.Find(UiSelector::Key("search-between-actions")).One().bounds;
        const auto actionBounds = ui.Find(UiSelector::Key("header-trailing-action")).One().bounds;
        if (width >= 600.0F) {
            const auto header = ui.Find(UiSelector::Key("desktop-search-header")).One().bounds;
            check(std::abs(searchBounds.y - header.y - clashflux::ui::kDesktopTitleBarSearchInset) < 0.5F &&
                  std::abs(header.y + header.height - searchBounds.y - searchBounds.height -
                           clashflux::ui::kDesktopTitleBarSearchInset) < 0.5F,
                  "桌面搜索胶囊上下各留 3pt，不顶满自定义标题栏");
        }
        check(searchBounds.width > 0.0F && searchBounds.x + searchBounds.width + 11.5F <= actionBounds.x &&
              std::abs(actionBounds.x + actionBounds.width - (width - 8.0F)) < 0.5F,
              "标题与动作先保留宽度，搜索随剩余空间收缩且不挤压操作按钮");
        check(std::abs(searchBounds.x + searchBounds.width * 0.5F -
                       (8.0F + 72.0F + actionBounds.x) * 0.5F) < 0.5F,
              "搜索在标题与动作之间的可用区域居中");
        if (width >= 960.0F) {
            check(searchBounds.width <= clashflux::ui::kDesktopTitleBarSearchWidth + 0.5F &&
                  actionBounds.x - searchBounds.x - searchBounds.width > 100.0F,
                  "宽标题栏在搜索表面与动作间保留拖动空白");
        }
        for (const char* name : {"create-centered-title", "busy-centered-title"}) {
            const auto bounds = ui.Find(UiSelector::Key(name)).One().bounds;
            check(std::abs(bounds.x + bounds.width * 0.5F - width * 0.5F) < 0.5F,
                  "新建页面标题居于窗口中心，左右按钮大小不同及加载中仍保持居中");
        }
    }
}

struct SearchOutlineFixture {
    huxerui::State<bool> focused{false};
    huxerui::State<bool> reduced{false};
    huxerui::State<bool> dark{false};
    int compositions = 0;
};
std::shared_ptr<SearchOutlineFixture> searchOutlineFixture;
void InstallSearchOutlineFixture(huxerui::ApplicationContext& context) {
    searchOutlineFixture = std::make_shared<SearchOutlineFixture>();
    context.Provide(searchOutlineFixture);
}
[[huxerui::composable]] huxerui::View SearchOutlineContent() {
    const auto model = huxerui::UseService<SearchOutlineFixture>();
    const auto& theme = huxerui::UseTheme();
    ++model->compositions;
    const bool animated = !theme.motion.reduced_motion &&
        huxerui::UseWindow().LifecycleState() != huxerui::WindowLifecycleState::Background;
    return huxerui::Column {
        clashflux::ui::WithSearchFocusOutline(huxerui::Row {huxerui::Text("SEARCH")}
            .With(huxerui::Frame{.width = 320.0F, .height = 34.0F},
                  huxerui::Border(theme.colors.outline, 1.0F), huxerui::CornerRadius(17.0F))
            .Key("search-outline"), model->focused.Get(), theme.colors.primary, animated),
        huxerui::Text("BELOW-SEARCH").Key("below-search"),
    };
}
huxerui::View SearchOutlineRoot() {
    return huxerui::Scope([] {
        const auto model = huxerui::UseService<SearchOutlineFixture>();
        auto theme = model->dark.Get() ? huxerui::MaterialDarkThemeSpec() : huxerui::MaterialLightThemeSpec();
        theme.colors = model->dark.Get() ? clashflux::ui::FluxDarkColors() : clashflux::ui::FluxLightColors();
        theme.motion.reduced_motion = model->reduced.Get();
        return huxerui::MaterialTheme(theme, SearchOutlineContent());
    });
}
const huxerui::Application kSearchOutlineApplication{SearchOutlineRoot,
    {.show_debug_overlay = false, .application_hooks = {InstallSearchOutlineFixture}}};

void testSearchFocusOutline() {
    huxerui::testing::UiTest ui(kSearchOutlineApplication, {.viewport = {600.0F, 200.0F}});
    ui.UpdateWindowLifecycleState(huxerui::WindowLifecycleState::Active);
    const auto model = searchOutlineFixture;
    for (bool dark : {false, true}) {
        model->dark = dark;
        model->focused = false;
        ui.PumpAndSettle();
        const auto bounds = ui.Find(UiSelector::Key("search-outline")).One().bounds;
        const auto below = ui.Find(UiSelector::Key("below-search")).One().bounds;
        const auto idle = ui.CaptureSnapshot();
        model->focused = true;
        ui.Pump();
        const int compositions = model->compositions;
        const auto start = ui.CaptureSnapshot();
        ui.Pump(std::chrono::milliseconds(80));
        const auto partial = ui.CaptureSnapshot();
        ui.Pump(std::chrono::milliseconds(80));
        check(partial != start && partial != ui.CaptureSnapshot(),
              "focus outline reveals progressively instead of lighting the entire capsule immediately");
        check(model->compositions == compositions &&
              ui.Find(UiSelector::Key("search-outline")).One().bounds == bounds &&
              ui.Find(UiSelector::Key("below-search")).One().bounds == below,
              "focus reveal updates retained paint without recomposition or layout movement");
        ui.PumpAndSettle();
        const auto finished = ui.CaptureSnapshot();
        ui.Pump(std::chrono::seconds(2));
        check(finished == ui.CaptureSnapshot() && finished != idle,
              "completed focus reveal stays highlighted and stops requesting frames");
        model->focused = false;
        ui.PumpAndSettle();
        check(ui.CaptureSnapshot() == idle, "blur restores the normal border immediately");
    }
    for (bool background : {false, true}) {
        model->focused = true;
        ui.Pump();
        ui.Pump(std::chrono::milliseconds(50));
        model->focused = false;
        ui.Pump();
        model->focused = true;
        if (background) ui.UpdateWindowLifecycleState(huxerui::WindowLifecycleState::Background);
        else model->reduced = true;
        ui.PumpAndSettle();
        const auto stopped = ui.CaptureSnapshot();
        ui.Pump(std::chrono::seconds(2));
        check(stopped == ui.CaptureSnapshot(), "reduced motion/background settles interrupted focus without recurring frames");
        ui.SetWindowMetrics({.viewport = {390.0F, 200.0F}});
        ui.PumpAndSettle();
        check(ui.Find(UiSelector::Key("search-outline")).One().bounds.width == 320.0F,
              "focus paint preserves capsule geometry after resize");
        model->focused = false;
        ui.PumpAndSettle();
        model->reduced = false;
    }
}

// Exercise the mobile policy even on the desktop test host, including wide
// mobile viewports. The page content must keep its space below a 48pt search.
huxerui::View MobileSearchRoot() {
    return huxerui::FlatTheme{huxerui::Scope([] {
        const auto& theme = huxerui::UseTheme();
        const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
        auto query = huxerui::UseState(huxerui::TextEditingValue{});
        auto additions = huxerui::UseState(0);
        auto search = huxerui::Row {
          huxerui::Text("SEARCH"),
          huxerui::TextField(query.Get())
              .OnChanged([query](const huxerui::TextEditingValue& value) { query = value; })
              .With(huxerui::Grow(1.0F)).Key("mobile-search-input"),
          huxerui::Button("CLEAR").OnClick([query] { query = huxerui::TextEditingValue{}; }),
          huxerui::Button("ADD").OnClick([additions] { ++additions; }),
        }.With(huxerui::Frame{.height = 48.0F}, huxerui::Grow(1.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
            .Key("mobile-search-surface");
        return huxerui::Column {
          clashflux::ui::PageHeaderLayout(theme,
              clashflux::ui::PageSearchFieldLayout(search, compact, false), {}, false, true),
          huxerui::Column {
            huxerui::Text("QUERY:" + query.Get().text),
            huxerui::Text("ADDITIONS:" + std::to_string(additions.Get())),
          }.With(huxerui::Grow(1.0F)).Key("mobile-search-content"),
        }.With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(8.0F, 0.0F)),
               huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    })};
}
const huxerui::Application kMobileSearchApplication{MobileSearchRoot, {.show_debug_overlay = false}};
void testMobileSearchGeometry() {
    huxerui::testing::UiTest ui(kMobileSearchApplication, {.viewport = {390.0F, 740.0F}});
    ui.PumpAndSettle();
    ui.Find(UiSelector::Key("mobile-search-input")).Tap();
    ui.Find(UiSelector::Key("mobile-search-input")).EnterText("demo");
    ui.Pump(kStep); // A focused editor intentionally keeps its caret clock alive.
    int additions = 0;
    for (float width : {320.0F, 390.0F, 420.0F, 600.0F, 960.0F, 390.0F}) {
        ui.SetWindowMetrics({.viewport = {width, 740.0F}}); ui.Pump(kStep);
        const auto search = ui.Find(UiSelector::Key("mobile-search-surface")).One().bounds;
        const auto content = ui.Find(UiSelector::Key("mobile-search-content")).One().bounds;
        check(std::abs(search.x - 8.0F) < 0.5F && std::abs(search.y) < 0.5F &&
              std::abs(search.width - width + 16.0F) < 0.5F && std::abs(search.height - 48.0F) < 0.5F,
              "移动搜索保持原有满宽 48pt，不受桌面限宽居中影响（含宽移动视口）");
        check(content.y >= 47.5F && content.y < 49.0F && content.height > 680.0F,
              "移动搜索栏不吞掉下方列表区域");
        check(ui.Find(UiSelector::Text("QUERY:demo")).Exists(), "移动搜索内容在宽窄切换后保留");
        ui.Find(UiSelector::Text("ADD")).Tap(); ui.Pump(kStep);
        check(ui.Find(UiSelector::Text("ADDITIONS:" + std::to_string(++additions))).Exists(),
              "移动搜索内添加按钮在缩放后仍可点击");
    }
    ui.Find(UiSelector::Text("CLEAR")).Tap(); ui.Pump(kStep);
    check(ui.Find(UiSelector::Text("QUERY:")).Exists(), "移动搜索可正常清空");
}

// 回归复现：带 UsePopup 锚点的动作视图在响应式断点切换（标题栏结构性替换）
// 时，不得因锚点仍挂在旧节点上而抛 presentation anchor logic_error。
huxerui::View ResponsiveAnchorRoot() {
    return huxerui::FlatTheme{huxerui::Scope([] {
        auto popup = huxerui::UsePopup();
        const bool compact =
            huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
        huxerui::View anchored = huxerui::Row {
            huxerui::Button("LOG-ACTIONS").With(popup.Anchor()),
        };
        huxerui::View bar;
        if (compact) {
            bar = huxerui::Row {
                huxerui::Text("COMPACT-TITLE"),
                std::move(anchored),
            };
        } else {
            bar = huxerui::Column {
                huxerui::WindowTitleBar{huxerui::Row {
                    huxerui::Text("DESKTOP-TITLE"),
                    huxerui::View{std::move(anchored)}.With(huxerui::Grow(1.0F)),
                }}.With(huxerui::Frame{.height = 40.0F}),
                huxerui::Text("BODY"),
            }.With(huxerui::Grow(1.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
        }
        return huxerui::Column {std::move(bar)}.With(huxerui::Grow(1.0F));
    })};
}
const huxerui::Application kResponsiveAnchorApplication{
    ResponsiveAnchorRoot, {.show_debug_overlay = false}};
void testResponsiveAnchorMount() {
    huxerui::testing::UiTest ui(kResponsiveAnchorApplication, {.viewport = {1400.0F, 800.0F}});
    ui.PumpAndSettle();
    for (int cycle = 0; cycle < 3; ++cycle) {
        ui.SetWindowMetrics({.viewport = {500.0F, 700.0F}});
        ui.PumpAndSettle();
        ui.SetWindowMetrics({.viewport = {1400.0F, 800.0F}});
        ui.PumpAndSettle();
    }
    check(ui.Find(UiSelector::Text("LOG-ACTIONS")).Exists(),
          "响应式断点来回切换后锚点动作仍正常挂载，不抛 presentation anchor 错误");
}

huxerui::View CompactPageBottomRoot() {
    return huxerui::FlatTheme{huxerui::Scope([] {
        const auto& theme = huxerui::UseTheme();
        return huxerui::Column{
            huxerui::Spacer{}.With(huxerui::Grow(1.0F)),
            huxerui::Text("PAGE-END").Key("compact-page-end"),
        }.With(huxerui::Grow(1.0F),
               huxerui::Padding(clashflux::ui::PageContentInsets(theme, true, 8.0F)),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    })};
}
const huxerui::Application kCompactPageBottomApplication{
    CompactPageBottomRoot, {.show_debug_overlay = false}};
void testCompactPageBottom() {
    huxerui::testing::UiTest ui(kCompactPageBottomApplication,
                                {.viewport = {320.0F, 480.0F}});    for (float height : {480.0F, 640.0F}) {
        ui.SetWindowMetrics({.viewport = {320.0F, height}});
        ui.PumpAndSettle();
        const auto bounds = ui.Find(UiSelector::Key("compact-page-end")).One().bounds;
        check(std::abs(bounds.y + bounds.height - height) < 0.5F,
              "Compact 页面内容容器延伸到视口底边，不留骨架底部间距");
    }
}

huxerui::View PinnedPageActionRoot() {
    return huxerui::FlatTheme{huxerui::Scope([] {
        auto longForm = huxerui::UseState(false);
        return clashflux::ui::CompactPageActionContent(
            huxerui::ScrollView(huxerui::Column {
              huxerui::Button("LONG-FORM").OnClick([longForm] { longForm = true; }),
              huxerui::Row {}.With(huxerui::Frame{.height = longForm.Get() ? 1400.0F : 40.0F}),
              huxerui::Text("FORM-END").Key("pinned-form-end"),
            }).Key("pinned-form-scroll"),
            huxerui::Button("SAVE").With(huxerui::Frame{.height = clashflux::ui::kCompactPrimaryActionHeight})
                .Key("pinned-save"));
    })};
}
const huxerui::Application kPinnedPageActionApplication{PinnedPageActionRoot,
    {.window = {.content_mode = huxerui::WindowContentMode::EdgeToEdge}, .show_debug_overlay = false}};

void testPinnedPageAction() {
    for (const auto size : {huxerui::Size{320.0F, 480.0F}, huxerui::Size{400.0F, 700.0F},
                            huxerui::Size{420.0F, 300.0F}}) {
        for (float inset : {0.0F, 24.0F, 48.0F}) {
            huxerui::testing::UiTest ui(kPinnedPageActionApplication,
                {.viewport = size, .safe_area = {.bottom = inset}});
            ui.PumpAndSettle();
            const auto save = ui.Find(UiSelector::Key("pinned-save"));
            const auto scroll = ui.Find(UiSelector::Key("pinned-form-scroll"));
            const auto bounds = save.One().bounds;
            check(std::abs(bounds.height - clashflux::ui::kCompactPrimaryActionHeight) < 0.5F &&
                  std::abs(bounds.y + bounds.height - (size.height - inset - 8.0F)) < 0.5F,
                  "短表单的保存按钮固定在底部安全区上方，不额外预留胶囊导航空白");
            check(std::abs(scroll.One().bounds.height -
                           (size.height - inset - clashflux::ui::kCompactPrimaryActionHeight - 16.0F)) < 0.5F,
                  "表单独占按钮上方的剩余空间，不与底部占位平分高度");
            ui.Find(UiSelector::Text("LONG-FORM")).Tap();
            ui.PumpAndSettle();
            check(save.One().bounds == bounds, "长表单不会将保存按钮挤离底部");
            scroll.ScrollBy({0.0F, 2000.0F});
            ui.PumpAndSettle();
            const auto end = ui.Find(UiSelector::Key("pinned-form-end")).One();
            check(save.One().bounds == bounds && end.in_viewport &&
                  end.bounds.y + end.bounds.height <= bounds.y,
                  "滚动至表单末项时保存按钮保持固定，末项仍可完整显示");
            ui.SetWindowMetrics({.viewport = {size.width, size.height + 100.0F},
                                 .safe_area = {.bottom = inset}});
            ui.PumpAndSettle();
            const auto resized = save.One().bounds;
            check(std::abs(resized.y + resized.height - (size.height + 100.0F - inset - 8.0F)) < 0.5F,
                  "窗口高度变化后保存按钮仍锚定可用区域底部");
        }
    }
}

huxerui::View GroupPickerSelectionContent() {
    return huxerui::Scope([] {
        auto selected = huxerui::UseState(std::string{"alpha"});
        auto dismissed = huxerui::UseState(0);
        return huxerui::Column {
          huxerui::Text("DISMISSED-" + std::to_string(dismissed.Get())),
          clashflux::ui::GroupPickerContent({{"alpha", "Alpha"}, {"beta", "Beta", "!"}},
              selected.Get(), [selected](const std::string& key) { selected = key; },
              [dismissed] { dismissed = dismissed.Get() + 1; }).With(huxerui::Grow(1.0F)),
        }.With(huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    });
}
huxerui::View GroupPickerSelectionLightRoot() { return huxerui::FlatTheme{GroupPickerSelectionContent()}; }
huxerui::View GroupPickerSelectionDarkRoot() { return huxerui::FlatDarkTheme{GroupPickerSelectionContent()}; }
const huxerui::Application kGroupPickerSelectionLightApplication{GroupPickerSelectionLightRoot};
const huxerui::Application kGroupPickerSelectionDarkApplication{GroupPickerSelectionDarkRoot};
void testGroupPickerSelection() {
    using huxerui::testing::UiSemanticSelector;
    for (const auto* app : {&kGroupPickerSelectionLightApplication, &kGroupPickerSelectionDarkApplication}) {
        for (float width : {320.0F, 960.0F}) {
            huxerui::testing::UiTest ui(*app, {.viewport = {width, 500.0F}, .resource_provider = AppResources()});
            ui.PumpAndSettle();
            const auto choice = [&ui](const char* label) {
                return ui.FindSemantics(UiSemanticSelector::AllOf(
                    UiSemanticSelector::Label(label), UiSemanticSelector::Role(huxerui::SemanticRole::Button)));
            };
            check(choice("Alpha").One().selected == true && choice("Beta").One().selected == false,
                  "group choices use the shared tile's accessible selected state");
            check(ui.Find(UiSelector::Key("group-picker-item-alpha")).One().bounds.width < 200.0F,
                  "shared selected tiles retain the picker's compact flowing layout");
            const auto before = ui.CaptureSnapshot();
            ui.Find(UiSelector::Key("group-picker-item-beta")).Tap();
            ui.PumpAndSettle();
            check(choice("Beta").One().selected == true && choice("Alpha").One().selected == false &&
                  ui.Find(UiSelector::Text("DISMISSED-1")).Exists() && ui.CaptureSnapshot() != before,
                  "choosing a group updates tile selection and dismisses exactly once in both themes");
        }
    }
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
          std::abs(side.y - 40.5F) < 0.5F && std::abs(side.height - 659.5F) < 0.5F,
          "宽屏抽屉贴右侧，仅占标题栏下沿以下的内容区域且宽度 360pt");
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
    auto toast = clashflux::ui::UseFluxToast();
    auto result = huxerui::UseState(std::string{"idle"});
    return huxerui::Column{
        huxerui::Button("Choose profile file").OnClick([tasks, toast, filter, result] {
            tasks.Launch([toast, filter, result]() -> huxerui::Task<void> {
                // 消费生产筛选 DTO 的真实事件协程；不需要宿主文件窗口即可验证资源边界。
                const auto picked = co_await clashflux::ui::PickProfileConfigFile(nullptr, filter, toast);
                if (!picked) result = filter.name + ":" + filter.extensions[0] + "," + filter.extensions[1] + "," + filter.extensions[2] + "," + filter.extensions[3];
            });
        }),
        huxerui::Text(result.Get()),
    };
}
huxerui::View ProfileFileFilterRoot() { return huxerui::FlatTheme{clashflux::ui::FluxToastStack(ProfileFileFilterContent())}; }
const huxerui::Application kProfileFileFilterApplication{ProfileFileFilterRoot, {.window_hooks = {clashflux::ui::InstallToastLayers}}};

void testProfileFileFilter() {
    for (const auto* language : {"zh", "en"}) {
        huxerui::testing::UiTest ui(kProfileFileFilterApplication, {.viewport = {600.0F, 500.0F},
            .resources = {.locale = huxerui::Locale::FromLanguageTag(language)}, .resource_provider = AppResources()});
        ui.PumpAndSettle();
        ui.Find(UiSelector::Text("Choose profile file")).Tap(); ui.Pump(kSettle);
        const std::string expected = std::string(language) == "zh" ? "sing-box / Clash / OpenVPN 配置:json,yaml,yml,ovpn" : "sing-box / Clash / OpenVPN configuration:json,yaml,yml,ovpn";
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

struct RefreshFixture : clashflux::ui::ProfileRefreshModel {
    huxerui::State<std::string> a{"RULES-A-OLD"}, b{"RULES-B-OLD"}, error{std::string{}};
    huxerui::State<std::int64_t> visible{1};
    huxerui::State<std::uint64_t> aVersion{0}, bVersion{0};
    int attempts = 0;
    bool clearedBeforeFeedback = true;
};
std::shared_ptr<RefreshFixture> refreshFixture;
void InstallRefreshFixture(huxerui::ApplicationContext& context) {
    refreshFixture = std::make_shared<RefreshFixture>();
    context.Provide(refreshFixture);
}
[[huxerui::composable]] huxerui::View RefreshOrigin() {
    const auto model = huxerui::UseService<RefreshFixture>();
    const auto tasks = huxerui::UseApplicationTaskScope();
    const auto start = [model, tasks](std::int64_t id, bool fail) {
        clashflux::ui::LaunchProfileRefresh<std::string>(tasks, model, id,
            [model, id, fail]() -> huxerui::Task<std::string> {
                ++model->attempts;
                co_await huxerui::Delay(std::chrono::milliseconds(id == 1 ? 300 : 600));
                if (fail) throw std::runtime_error("refresh failed");
                co_return id == 1 ? "RULES-A-NEW" : "RULES-B-NEW";
            }, [model, id](std::optional<std::string> result, std::exception_ptr error) {
                model->clearedBeforeFeedback &= !model->Refreshing(id);
                if (error) model->error = "REFRESH-FAILED";
                else (id == 1 ? model->a : model->b) = std::move(*result);
                (id == 1 ? model->aVersion : model->bVersion) = model->RefreshVersion(id);
            });
    };
    return huxerui::Row {
      huxerui::Button("Refresh A").OnClick([start] { start(1, false); }),
      huxerui::Button("Fail B").OnClick([start] { start(2, true); }),
      huxerui::Button("Retry B").OnClick([start] { start(2, false); }),
    };
}
huxerui::View RefreshRoot() {
    return huxerui::FlatTheme {huxerui::Scope([] {
        const auto model = huxerui::UseService<RefreshFixture>();
        auto shown = huxerui::UseState(true);
        const auto& theme = huxerui::UseTheme();
        const auto id = model->visible.Get();
        return huxerui::Column {
          huxerui::Button("Retire refresh origin").OnClick([shown] { shown = !shown.Get(); }),
          huxerui::Button("View A").OnClick([model] { model->visible = 1; }),
          huxerui::Button("View B").OnClick([model] { model->visible = 2; }),
          shown.Get() ? RefreshOrigin().Key("refresh-origin") : huxerui::View{huxerui::Row {}}.Key("refresh-retired"),
          model->RulesLoading(id, id == 1 ? model->aVersion.Get() : model->bVersion.Get())
                               ? clashflux::ui::PageLoadingSkeleton(theme, true)
                                    .Key(id == 1 ? "rules-a-loading" : "rules-b-loading")
                               : huxerui::View{huxerui::Text(id == 1 ? model->a.Get() : model->b.Get())},
          huxerui::Text("GLOBAL-RULES-READY"), huxerui::Text(model->error.Get()),
        };
    })};
}
const huxerui::Application kRefreshApplication{RefreshRoot,
    {.show_debug_overlay = false, .application_hooks = {InstallRefreshFixture}}};
void testScopedProfileRefresh() {
    huxerui::testing::UiTest ui(kRefreshApplication, {.viewport = {700.0F, 1000.0F}});
    ui.Pump();
    const auto model = refreshFixture;
    ui.Find(UiSelector::Text("Refresh A")).Tap(); ui.Pump();
    ui.Find(UiSelector::Text("Refresh A")).Tap(); ui.Pump();
    check(model->attempts == 1 && ui.Find(UiSelector::Key("rules-a-loading")).Exists(), "refresh A is single-flight and shows only A's skeleton");
    ui.Find(UiSelector::Text("View B")).Tap(); ui.Pump();
    check(ui.Find(UiSelector::Text("RULES-B-OLD")).Exists() && !ui.Find(UiSelector::Key("page-loading-skeleton")).Exists() &&
          ui.Find(UiSelector::Text("GLOBAL-RULES-READY")).Exists(), "switching to cached B during A refresh shows B and global rules without skeletons");
    ui.Find(UiSelector::Text("View A")).Tap(); ui.Pump();
    check(ui.Find(UiSelector::Key("rules-a-loading")).Exists(), "returning to pending A restores only A's skeleton");
    ui.Find(UiSelector::Text("Fail B")).Tap(); ui.Pump();
    ui.Find(UiSelector::Text("Retire refresh origin")).Tap(); ui.Pump();
    check(!ui.Find(UiSelector::Text("Refresh A")).Exists(), "refresh origin actually retires while tasks are awaiting");
    ui.Pump(std::chrono::milliseconds(350)); ui.Pump();
    check(ui.Find(UiSelector::Text("RULES-A-NEW")).Exists() && model->Refreshing(2), "A completes after retirement without clearing B's pending operation");
    ui.Find(UiSelector::Text("View B")).Tap(); ui.Pump();
    ui.Pump(std::chrono::milliseconds(350)); ui.Pump();
    check(ui.Find(UiSelector::Text("RULES-B-OLD")).Exists() && ui.Find(UiSelector::Text("REFRESH-FAILED")).Exists(), "failed B refresh restores its old rules and clears its skeleton");
    ui.Find(UiSelector::Text("Retire refresh origin")).Tap(); ui.Pump();
    ui.Find(UiSelector::Text("Retry B")).Tap(); ui.Pump(std::chrono::milliseconds(700)); ui.Pump();
    check(ui.Find(UiSelector::Text("RULES-B-NEW")).Exists() && model->clearedBeforeFeedback, "failed refresh is retryable and clears ID-specific busy before feedback");
    const auto old = *model->BeginRefresh(1); model->FinishRefresh(1, old);
    const auto latest = *model->BeginRefresh(1);
    check(!model->FinishRefresh(1, old) && model->Refreshing(1) && model->RefreshVersion(2) != latest,
          "stale completion cannot clear a newer operation or change another profile's version");
    model->FinishRefresh(1, latest);
    check(model->RulesLoading(1, old) && !model->RulesLoading(2, model->bVersion.Get()),
          "completed A refresh invalidates only A's old rules until its new snapshot arrives");
    check(!model->RulesLoading(1, latest), "the matching rules snapshot ends only its own loading state");
}

struct SubscriptionRuleLayoutFixture {
    huxerui::State<std::size_t> source{0}, view{0};
    huxerui::State<bool> busy{false}, dark{false};
    int actions = 0;
};
std::shared_ptr<SubscriptionRuleLayoutFixture> subscriptionRuleLayoutFixture;

huxerui::View SubscriptionRuleLayoutRoot() {
    return huxerui::Scope([] {
        const auto model = huxerui::UseService<SubscriptionRuleLayoutFixture>();
        auto theme = model->dark.Get() ? huxerui::MaterialDarkThemeSpec() : huxerui::MaterialLightThemeSpec();
        theme.colors = model->dark.Get() ? clashflux::ui::FluxDarkColors() : clashflux::ui::FluxLightColors();
        return huxerui::MaterialTheme(theme, huxerui::Scope([model] {
            std::vector<std::string> views;
            for (const auto* label : {"修改后的规则", "订阅原规则", "个人修改记录"})
                views.push_back(huxerui::UseString(clashflux::ui::Localized(label)));
            auto scroll = huxerui::UseScrollController();
            return huxerui::Column {
              clashflux::ui::SubscriptionRulesToolbar({"A", "B", "一个很长的订阅名称 Long subscription name"},
                  model->source.Get(), views, model->view.Get(), model->busy.Get(),
                  [model](std::size_t index) { model->source = index; },
                  [model](std::size_t index) { model->view = index; },
                  huxerui::IconButton(huxerui::ImageResource{"app", "images/more_vertical"}, "Preview")
                      .OnClick([model] { ++model->actions; }).Key("rule-toolbar-more"))
                  .Key("rule-toolbar"),
              huxerui::VirtualList(1000, [model](std::size_t index) {
                  return clashflux::ui::SubscriptionRuleContent("域名后缀",
                      "very-long-api.example.com/path/to/resource", "香港自动选择很长的出口名称",
                      "个人新增 · 订阅规则之前",
                      huxerui::IconButton(huxerui::ImageResource{"app", "images/more_vertical"}, "Details")
                          .OnClick([model] { ++model->actions; }).Key("rule-row-more"))
                      .With(huxerui::Padding(8.0F)).Key("rule-layout-" + std::to_string(index));
              }).Controller(scroll).EstimatedItemExtent(clashflux::ui::kSubscriptionRuleEstimatedExtent)
                  .With(huxerui::Grow(1.0F)).Key("rule-layout-list"),
            }.With(huxerui::Padding(4.0F), huxerui::Spacing(4.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
        }));
    });
}
const huxerui::Application kSubscriptionRuleLayoutApplication{SubscriptionRuleLayoutRoot,
    {.show_debug_overlay = false, .application_hooks = {[](huxerui::ApplicationContext& context) {
        subscriptionRuleLayoutFixture = std::make_shared<SubscriptionRuleLayoutFixture>();
        context.Provide(subscriptionRuleLayoutFixture);
    }}}};

void testSubscriptionRuleLayout() {
    for (const auto* locale : {"zh", "en"}) {
        huxerui::testing::UiTest ui(kSubscriptionRuleLayoutApplication,
            {.viewport = {960.0F, 600.0F},
             .resources = {.locale = huxerui::Locale::FromLanguageTag(locale)}, .resource_provider = AppResources()});
        const auto model = subscriptionRuleLayoutFixture;
        for (bool dark : {false, true}) {
            model->dark = dark;
            for (float width : {320.0F, 420.0F, 599.0F, 600.0F, 960.0F, 1600.0F, 320.0F}) {
                ui.SetWindowMetrics({.viewport = {width, 600.0F}}); ui.PumpAndSettle();
                const auto source = ui.Find(UiSelector::Key("subscription-rule-source")).One().bounds;
                const auto view = ui.Find(UiSelector::Key("subscription-rule-view")).One().bounds;
                const auto more = ui.Find(UiSelector::Key("rule-toolbar-more")).One().bounds;
                check(source.width >= 30.0F && source.x + source.width <= view.x &&
                      view.x + view.width <= more.x && more.x + more.width <= width - 3.5F &&
                      std::abs(source.y - view.y) < 1.0F && more.width >= 40.0F,
                      "subscription source, view and action stay on one accessible row at every width");
                check(width < 600.0F || source.width < 250.0F,
                      "wide rule source selector retains a compact natural width");
                const auto row = ui.Find(UiSelector::Key("rule-layout-0"));
                const auto kind = row.Find(UiSelector::Key("rule-kind")).One().bounds;
                const auto pattern = row.Find(UiSelector::Key("rule-pattern")).One().bounds;
                const auto target = row.Find(UiSelector::Key("rule-target")).One().bounds;
                const auto status = row.Find(UiSelector::Key("rule-status")).One().bounds;
                const auto action = row.Find(UiSelector::Key("rule-row-more")).One().bounds;
                check(pattern.width > 40.0F && target.width > 20.0F && kind.x + kind.width <= pattern.x &&
                      pattern.x + pattern.width <= target.x && target.x + target.width <= action.x &&
                      std::abs(kind.y - pattern.y) < 1.0F && status.y >= pattern.y + pattern.height &&
                      std::abs(status.x - pattern.x) < 1.0F && action.x + action.width <= width - 3.5F,
                      "rule match and target share the first line; status is aligned below without collisions");
                check(std::abs(row.One().bounds.height -
                      (16.0F + std::max(action.height, pattern.height + status.height + 4.0F))) < 1.0F,
                      "long rule fields keep a compact two-line height across widths");
                const auto snapshot = ui.CaptureSnapshot().ToString();
                check(width > 420.0F || snapshot.find("…") != std::string::npos,
                      "narrow rule labels paint an ellipsis instead of wrapping or spilling");
            }
        }
        ui.Find(UiSelector::Key("subscription-rule-source")).Tap(); ui.PumpAndSettle();
        ui.PressKey(huxerui::Key::ArrowDown); ui.PressKey(huxerui::Key::Enter); ui.PumpAndSettle();
        check(model->source.Get() == 1, "source selector still changes the controlled source");
        ui.Find(UiSelector::Key("subscription-rule-view")).Tap(); ui.PumpAndSettle();
        ui.PressKey(huxerui::Key::ArrowDown); ui.PressKey(huxerui::Key::Enter); ui.PumpAndSettle();
        check(model->view.Get() == 1, "visible view selector still changes the controlled mode");
        model->source = 2; model->view = 2; ui.PumpAndSettle();
        const auto source = ui.Find(UiSelector::Key("subscription-rule-source")).One().bounds;
        const auto view = ui.Find(UiSelector::Key("subscription-rule-view")).One().bounds;
        check(source.width >= 30.0F && source.x + source.width <= view.x,
              "long source names yield space to the complete view control");
        ui.Find(UiSelector::Key("rule-toolbar-more")).Tap();
        ui.Find(UiSelector::Key("rule-layout-0")).Find(UiSelector::Key("rule-row-more")).Tap();
        check(model->actions == 2, "toolbar preview and rule detail actions remain reachable at 320pt");
        model->busy = true; ui.PumpAndSettle();
        check(!ui.Find(UiSelector::Key("subscription-rule-source")).One().enabled &&
              ui.Find(UiSelector::Key("subscription-rule-view")).One().enabled,
              "save busy only disables the source; view and existing rules remain browsable");
        ui.Find(UiSelector::Key("rule-layout-list")).ScrollBy({0.0F, 1800.0F}); ui.PumpAndSettle();
        const auto before = ui.Find(UiSelector::Key("rule-layout-list")).Find(UiSelector::Key("rule-layout-0"));
        check(!before.Exists() || !before.One().in_viewport, "long subscription lists scroll with virtual rows");
        ui.SetWindowMetrics({.viewport = {1600.0F, 600.0F}}); ui.PumpAndSettle();
        ui.SetWindowMetrics({.viewport = {320.0F, 600.0F}}); ui.PumpAndSettle();
        check(model->source.Get() == 2 && model->view.Get() == 2 &&
              (!before.Exists() || !before.One().in_viewport),
              "resize preserves source, view and actual virtual-list scroll position");
    }
}

struct RuleSourceFixtureRow {
    std::int64_t id;
    std::string type;
    bool selected = false;
    bool operator==(const RuleSourceFixtureRow&) const = default;
};
void testRuleSourceEligibility() {
    std::shared_ptr<clashflux::ui::ProfileListModel<RuleSourceFixtureRow>> owned;
    huxerui::Application application{[] { return huxerui::View{huxerui::Row {}}; },
        {.application_hooks = {[&owned](huxerui::ApplicationContext& context) {
            owned = std::make_shared<clashflux::ui::ProfileListModel<RuleSourceFixtureRow>>();
            context.Provide(owned);
        }}}};
    huxerui::testing::UiTest ui(application, {.viewport = {700.0F, 1000.0F}});
    auto& model = *owned;
    model.list = std::vector<RuleSourceFixtureRow>{{1, "pptp", true}, {2, "remote"}, {3, "local"}, {4, "remote"}};
    // Worker classification includes YAML/JSON and excludes the OpenVPN file.
    model.ruleSources = std::vector<std::int64_t>{1, 2, 3};
    check(model.RuleSources().empty() && model.ResolveRuleSource(1) == 0,
          "rules wait for classification instead of offering unclassified profiles");
    model.ruleSourcesReady = true;
    const auto eligible = model.RuleSources();
    check(eligible.size() == 2 && eligible[0].id == 2 && eligible[1].id == 3 &&
          !model.IsRuleSource(1) && !model.IsRuleSource(4),
          "PPTP and OpenVPN are omitted while YAML and native JSON keep their stable IDs and order");
    check(model.ResolveRuleSource(1) == 2 && model.ResolveRuleSource(4) == 2,
          "unsupported selections fall back to an editable profile instead of creating placeholder entries");
    model.list = std::vector<RuleSourceFixtureRow>{{1, "pptp", true}, {2, "remote"}, {3, "local", true}, {4, "remote"}};
    check(model.ResolveRuleSource(0) == 3 && model.ResolveRuleSource(2) == 2,
          "the eligible main profile is the default and an eligible explicit selection is retained");
    model.list = std::vector<RuleSourceFixtureRow>{{1, "pptp", true}, {3, "local", true}, {4, "remote"}};
    check(!model.IsRuleSource(2) && model.ResolveRuleSource(2) == 3, "a removed source cannot remain selectable in a stale classification");
    model.list = std::vector<RuleSourceFixtureRow>{{1, "pptp", true}, {4, "remote"}};
    check(model.RuleSources().empty() && model.ResolveRuleSource(3) == 0,
          "an unsupported-only inventory has a genuine empty state and no fabricated selection");
}

using TestProfilesModel = clashflux::ui::ProfileListModel<std::string>;
struct SelectionFixture : clashflux::ui::ProfileListModel<std::int64_t> {
    huxerui::State<std::int64_t> desired{1}, applied{1};
    int publications = 0, cancellations = 0, executions = 0, active = 0, maximum = 0;
};
std::shared_ptr<SelectionFixture> selectionFixture;
void InstallSelectionFixture(huxerui::ApplicationContext& context) {
    selectionFixture = std::make_shared<SelectionFixture>();
    context.Provide(selectionFixture);
}
[[huxerui::composable]] huxerui::View SelectionOrigin() {
    const auto model = huxerui::UseService<SelectionFixture>();
    const auto tasks = huxerui::UseApplicationTaskScope();
    const auto select = [model, tasks](std::int64_t id, int latency, bool fail) {
        model->desired = id;
        clashflux::ui::LaunchProfileSelection<std::int64_t>(tasks, model, id,
            [model, id, latency, fail](std::stop_token cancel) -> huxerui::Task<std::int64_t> {
                ++model->executions;
                model->maximum = std::max(model->maximum, ++model->active);
                co_await huxerui::Delay(std::chrono::milliseconds(latency));
                --model->active;
                if (cancel.stop_requested()) ++model->cancellations;
                if (fail) throw std::runtime_error("superseded selection");
                co_return id;
            }, [model](std::optional<std::int64_t> id, std::exception_ptr failure, std::uint64_t) {
                ++model->publications;
                if (!failure && id) model->applied = *id;
                else model->desired = model->applied.Get();
            });
    };
    return huxerui::Row {
        huxerui::Button("Slow selection").OnClick([select] { select(2, 300, false); }),
        huxerui::Button("Quick selection").OnClick([select] { select(3, 50, false); }),
        huxerui::Button("Back to active").OnClick([select] { select(1, 10, false); }),
        huxerui::Button("Fail selection").OnClick([select] { select(4, 100, true); }),
    };
}
huxerui::View SelectionRoot() {
    return huxerui::FlatTheme {huxerui::Scope([] {
        auto retired = huxerui::UseState(false);
        return huxerui::Column {
            retired.Get() ? huxerui::View{huxerui::Text("SELECTION-RETIRED")} : SelectionOrigin(),
            huxerui::Button("Retire selection origin").OnClick([retired] { retired = true; }),
        };
    })};
}
const huxerui::Application kSelectionApplication{SelectionRoot,
    {.show_debug_overlay = false, .application_hooks = {InstallSelectionFixture}}};
void testLatestProfileSelection() {
    huxerui::testing::UiTest ui(kSelectionApplication, {.viewport = {900.0F, 400.0F}});
    ui.PumpAndSettle();
    const auto model = selectionFixture;
    const auto tap = [&](const char* label) { ui.Find(UiSelector::Text(label)).Tap(); ui.Pump(); };
    tap("Slow selection");
    const auto duplicateSerial = model->selectionSerial;
    tap("Slow selection"); tap("Slow selection");
    check(model->selectionSerial == duplicateSerial && model->executions == 1,
          "repeated clicks on the pending card are idempotent and submit no additional preparation");
    const auto cancelled = model->selectionCancel.get_token();
    tap("Back to active");
    check(cancelled.stop_requested() && model->desired.Get() == 1,
          "switching back while preparation is pending records the new intent and cancels the old request");
    ui.Pump(std::chrono::milliseconds(20));
    check(model->selectionPending.Get() && model->applied.Get() == 1,
          "retargeting keeps the active source unchanged while canceled preparation drains");
    ui.Pump(std::chrono::milliseconds(400));
    ui.PumpAndSettle();
    check(model->publications == 1 && model->cancellations == 1 && model->applied.Get() == 1,
          "a superseded completion cannot change selection or publish stale state");
    tap("Slow selection"); tap("Quick selection"); tap("Slow selection");
    ui.Pump(std::chrono::milliseconds(60));
    check(model->selectionPending.Get() && model->publications == 1,
          "equal-target ABA and intermediate completion cannot clear the newest pending request");
    tap("Retire selection origin");
    ui.SetWindowMetrics({.viewport = {390.0F, 600.0F}});
    ui.Pump(std::chrono::milliseconds(400)); ui.PumpAndSettle();
    check(model->applied.Get() == 2 && !model->selectionPending.Get() && model->publications == 2,
          "only the last selection publishes after caller retirement and resize");
    check(model->maximum == 1 && model->executions == 4,
          "one preparation runs at a time and intermediate queued targets are replaced without execution");
    huxerui::testing::UiTest retry(kSelectionApplication, {.viewport = {900.0F, 400.0F}});
    retry.PumpAndSettle();
    const auto failedModel = selectionFixture;
    retry.Find(UiSelector::Text("Fail selection")).Tap(); retry.Pump(std::chrono::milliseconds(200));
    retry.PumpAndSettle();
    check(!failedModel->selectionPending.Get() && failedModel->desired.Get() == failedModel->applied.Get(),
          "latest failure releases pending and restores the authoritative selection");
    retry.Find(UiSelector::Text("Quick selection")).Tap(); retry.Pump(std::chrono::milliseconds(100));
    retry.PumpAndSettle();
    check(failedModel->applied.Get() == 3 && !failedModel->selectionPending.Get(), "failed selection remains retryable");
}

struct ProfileListFixture {
    std::uint64_t revision = 1;
    std::vector<std::string> rows{"remote-1", "remote-2", "local-3"};
    std::chrono::milliseconds latency{10};
};
void InstallProfileListFixture(huxerui::ApplicationContext& context) {
    context.Provide(std::make_shared<TestProfilesModel>());
    context.Provide(std::make_shared<ProfileListFixture>());
}
huxerui::Task<clashflux::ui::ProfileListSnapshot<std::string>> ReadProfileListFixture(
    std::shared_ptr<ProfileListFixture> fixture) {
    clashflux::ui::ProfileListSnapshot<std::string> snapshot{.profiles = fixture->rows, .ready = true};
    co_await huxerui::Delay(fixture->latency);
    co_return snapshot;
}
[[huxerui::composable]] huxerui::View ProfileListFixtureContent() {
    const auto model = huxerui::UseService<TestProfilesModel>();
    const auto fixture = huxerui::UseService<ProfileListFixture>();
    const auto tasks = huxerui::UseApplicationTaskScope();
    auto cards = huxerui::UseStateList<std::string>();
    huxerui::Lifecycle([model, fixture, tasks] {
        clashflux::ui::DriveProfileList(tasks, model,
            [fixture] { return fixture->revision; },
            [fixture] { return ReadProfileListFixture(fixture); });
        return [] {};
    }, 0);
    // Production AppRoot's model -> retained StateList -> virtual cards chain.
    huxerui::Lifecycle([model, cards] {
        if (!model->selectionPending.Get() && !model->reorderPending.Get())
            clashflux::ui::ReplaceStateList(cards, model->list.Get());
        return [] {};
    }, model->list, model->selectionPending, model->reorderPending);
    return huxerui::Column{
        huxerui::Button("Remove remote").OnClick([model, fixture] {
            std::erase(fixture->rows, "remote-2");
            ++fixture->revision;
            model->Publish(fixture->rows);
        }),
        huxerui::Button("Add local").OnClick([model, fixture] {
            fixture->rows.push_back("local-4");
            ++fixture->revision;
            model->Publish(fixture->rows);
        }),
        huxerui::Button("Optimistic selection").OnClick([model, cards] {
            model->selectionPending = true;
            cards.Set(0, "optimistic-selection");
        }),
        huxerui::Button("Hold selection").OnClick([model] { model->selectionPending = true; }),
        huxerui::Button("Release selection").OnClick([model] { model->selectionPending = false; }),
        huxerui::Button("Hold reorder").OnClick([model] { model->reorderPending = true; }),
        huxerui::Button("Release reorder").OnClick([model] { model->reorderPending = false; }),
        huxerui::Button("External remove").OnClick([fixture] {
            std::erase(fixture->rows, "remote-2");
            ++fixture->revision;
        }),
        huxerui::Button("Slow read").OnClick([model, fixture] {
            fixture->latency = std::chrono::milliseconds{600};
            model->RequestSync();
        }),
        huxerui::VirtualGrid(cards.Size(), [cards](std::size_t index) {
            return huxerui::Text(cards[index]).Key(cards[index]);
        }).Columns(huxerui::GridColumns::Fixed(1)).EstimatedRowExtent(30.0F)
            .With(huxerui::Grow(1.0F)),
    }.With(huxerui::Grow(1.0F));
}
huxerui::View ProfileListFixtureRoot() { return huxerui::FlatTheme{ProfileListFixtureContent()}; }
const huxerui::Application kProfileListFixtureApplication{ProfileListFixtureRoot,
    {.application_hooks = {InstallProfileListFixture}}};
void PumpProfileList(huxerui::testing::UiTest& ui, std::chrono::milliseconds elapsed) {
    // Pump advances the clock once per frame; use bounded steps so a read's
    // completion and the following poll both execute inside the requested span.
    for (auto time = std::chrono::milliseconds{0}; time < elapsed;
         time += std::chrono::milliseconds{50})
        ui.Pump(std::chrono::milliseconds{50});
    ui.Pump();
}

// Drag-drop write-through: production ProfileListModel, mirror chain and pump.
struct ReorderPublishRow {
    std::int64_t id;
    std::string name;
    bool operator==(const ReorderPublishRow&) const = default;
};
using ReorderPublishModel = clashflux::ui::ProfileListModel<ReorderPublishRow>;
struct ReorderPublishStore {
    std::uint64_t revision = 1;
    std::vector<ReorderPublishRow> rows{{1, "remote-1"}, {2, "remote-2"}, {3, "local-3"}};
};
std::shared_ptr<ReorderPublishModel> reorderPublishModel;
std::shared_ptr<ReorderPublishStore> reorderPublishStore;
std::optional<huxerui::StateList<ReorderPublishRow>> reorderPublishCards;
void InstallReorderPublishFixture(huxerui::ApplicationContext& context) {
    context.Provide(std::make_shared<ReorderPublishModel>());
    context.Provide(std::make_shared<ReorderPublishStore>());
}
[[huxerui::composable]] huxerui::View ProfileReorderPublishContent() {
    const auto model = huxerui::UseService<ReorderPublishModel>();
    const auto store = huxerui::UseService<ReorderPublishStore>();
    const auto tasks = huxerui::UseApplicationTaskScope();
    auto cards = huxerui::UseStateList<ReorderPublishRow>();
    reorderPublishModel = model;
    reorderPublishStore = store;
    reorderPublishCards = cards;
    huxerui::Lifecycle([model, store, tasks] {
        clashflux::ui::DriveProfileList(tasks, model,
            [store] { return store->revision; },
            [store]() -> huxerui::Task<clashflux::ui::ProfileListSnapshot<ReorderPublishRow>> {
                clashflux::ui::ProfileListSnapshot<ReorderPublishRow> snapshot{
                    .profiles = store->rows, .ready = true};
                co_await huxerui::Delay(std::chrono::milliseconds{10});
                co_return snapshot;
            });
        return [] {};
    }, 0);
    huxerui::Lifecycle([model, cards] {
        if (!model->selectionPending.Get() && !model->reorderPending.Get())
            clashflux::ui::ReplaceStateList(cards, model->list.Get());
        return [] {};
    }, model->list, model->selectionPending, model->reorderPending);
    return huxerui::Text("reorder-publish");
}
huxerui::View ProfileReorderPublishRoot() { return huxerui::FlatTheme{ProfileReorderPublishContent()}; }
const huxerui::Application kProfileReorderPublishApplication{ProfileReorderPublishRoot,
    {.application_hooks = {InstallReorderPublishFixture}}};
void testProfileReorderPublish() {
    huxerui::testing::UiTest ui(kProfileReorderPublishApplication,
        huxerui::testing::UiTestOptions{.viewport = {700.0F, 850.0F}});
    PumpProfileList(ui, kSettle); ui.Pump();
    const auto model = reorderPublishModel;
    const auto store = reorderPublishStore;
    const auto modelIds = [model] {
        std::vector<std::int64_t> ids;
        for (const auto& row : model->list.Get()) ids.push_back(row.id);
        return ids;
    };
    const auto mirrorIds = [] {
        std::vector<std::int64_t> ids;
        const auto& cards = *reorderPublishCards;
        for (std::size_t i = 0; i < cards.Size(); ++i) ids.push_back(cards[i].id);
        return ids;
    };
    check(modelIds() == std::vector<std::int64_t>{1, 2, 3} &&
          mirrorIds() == std::vector<std::int64_t>{1, 2, 3},
          "reorder fixture hydrates the model and its mirror");
    // Production drop: reorderPending holds the mirror, setOrder updates the
    // store cache, then the new order is written through the authoritative model.
    model->reorderPending = true;
    store->rows = {store->rows[2], store->rows[0], store->rows[1]};
    check(model->PublishOrder({3, 1, 2}), "drag drop writes the new order through the model");
    model->reorderPending = false;
    ui.Pump();
    check(mirrorIds() == std::vector<std::int64_t>{3, 1, 2},
          "drop release mirrors the target order immediately without snapping back");
    PumpProfileList(ui, std::chrono::milliseconds{1500});
    check(mirrorIds() == std::vector<std::int64_t>{3, 1, 2} &&
          modelIds() == std::vector<std::int64_t>{3, 1, 2},
          "later list reads keep the published drag order");
    check(!model->PublishOrder({1, 2, 99}) &&
          modelIds() == std::vector<std::int64_t>{3, 1, 2},
          "incomplete drag order is rejected without touching the model");
}
void testProfileListPublication() {
    const auto makeOptions = [] {
        return huxerui::testing::UiTestOptions{.viewport = {700.0F, 850.0F}};
    };
    {
        huxerui::testing::UiTest ui(kProfileListFixtureApplication, makeOptions());
        PumpProfileList(ui, kSettle); ui.Pump();
        check(ui.Find(UiSelector::Text("remote-2")).Exists(), "profile list hydrates multiple remote/local rows");
        ui.Find(UiSelector::Text("Slow read")).Tap();
        PumpProfileList(ui, std::chrono::milliseconds{1000});
        ui.Find(UiSelector::Text("Remove remote")).Tap(); ui.Pump();
        check(!ui.Find(UiSelector::Text("remote-2")).Exists() &&
              ui.Find(UiSelector::Text("remote-1")).Exists(),
              "delete completion removes only its card without waiting for the list pump");
        ui.Find(UiSelector::Text("Add local")).Tap(); ui.Pump();
        check(ui.Find(UiSelector::Text("local-4")).Exists(), "import completion immediately publishes its new card");
        PumpProfileList(ui, std::chrono::milliseconds{700}); ui.Pump();
        check(!ui.Find(UiSelector::Text("remote-2")).Exists() && ui.Find(UiSelector::Text("local-4")).Exists(),
              "older in-flight profile snapshot cannot resurrect a deleted row or hide an import");
    }
    {
        huxerui::testing::UiTest ui(kProfileListFixtureApplication, makeOptions());
        PumpProfileList(ui, kSettle);
        ui.Find(UiSelector::Text("Optimistic selection")).Tap(); ui.Pump();
        check(ui.Find(UiSelector::Text("optimistic-selection")).Exists(),
              "pending selection preserves its optimistic card state");
        ui.Find(UiSelector::Text("Release selection")).Tap(); ui.Pump();
        check(ui.Find(UiSelector::Text("remote-1")).Exists() &&
              !ui.Find(UiSelector::Text("optimistic-selection")).Exists(),
              "failed selection restores authoritative cards even when the model list compares equal");
    }
    for (const bool reorder : {false, true}) {
        huxerui::testing::UiTest ui(kProfileListFixtureApplication, makeOptions());
        PumpProfileList(ui, kSettle); ui.Pump();
        ui.Find(UiSelector::Text(reorder ? "Hold reorder" : "Hold selection")).Tap();
        ui.Find(UiSelector::Text("External remove")).Tap();
        PumpProfileList(ui, std::chrono::milliseconds{1500}); ui.Pump();
        check(ui.Find(UiSelector::Text("remote-2")).Exists(), "pending selection/reorder defers poll publication");
        ui.Find(UiSelector::Text(reorder ? "Release reorder" : "Release selection")).Tap();
        PumpProfileList(ui, std::chrono::milliseconds{1500}); ui.Pump();
        check(!ui.Find(UiSelector::Text("remote-2")).Exists(),
              "deferred profile revision is retried after release without another database write");
    }
    {
        huxerui::testing::UiTest ui(kProfileListFixtureApplication, makeOptions());
        PumpProfileList(ui, kSettle); ui.Pump();
        ui.Find(UiSelector::Text("Hold reorder")).Tap();
        ui.Find(UiSelector::Text("Remove remote")).Tap(); ui.Pump();
        ui.Find(UiSelector::Text("Release reorder")).Tap(); ui.Pump();
        check(!ui.Find(UiSelector::Text("remote-2")).Exists(),
              "reorder release refreshes a skipped mirror even when the authoritative list is unchanged");
    }
}

// Use the production publication model and the subscription page's
// Lifecycle -> StateList -> VirtualGrid chain, without dialing a real VPN.
using TestVpnModel = clashflux::ui::VpnSnapshotModel<std::vector<std::string>>;
void InstallTestVpnModel(huxerui::ApplicationContext& context) {
    auto model = std::make_shared<TestVpnModel>();
    model->Publish({"VPN-1:idle", "VPN-2:idle"});
    context.Provide(model);
}

[[huxerui::composable]] huxerui::View VpnPublicationContent() {
    auto model = huxerui::UseService<TestVpnModel>();
    auto tasks = huxerui::UseTaskScope();
    auto cards = huxerui::UseStateList<std::string>();
    auto pollResult = huxerui::UseState(std::string{"POLL-idle"});
    huxerui::Lifecycle([model, cards] {
        clashflux::ui::ReplaceStateList(cards, model->view.Get());
        return [] {};
    }, model->view);
    return huxerui::Column{
        huxerui::Button("Start delayed VPN poll").OnClick([model, tasks, pollResult] {
            const auto before = model->syncTick.Get();
            auto next = model->view.Get();
            next[0] = "VPN-1:external";
            pollResult = "POLL-pending";
            tasks.Launch([model, pollResult, before, next]() -> huxerui::Task<void> {
                co_await huxerui::Delay(std::chrono::milliseconds(400));
                pollResult = model->PublishPolled(before, next)
                    ? "POLL-accepted" : "POLL-rejected";
            });
        }),
        huxerui::Button("First VPN connected").OnClick([model] {
            model->Publish({"VPN-1:connected", "VPN-2:connecting"});
        }),
        huxerui::Button("Second VPN failed").OnClick([model] {
            model->Publish({"VPN-1:connected", "VPN-2:failed"});
        }),
        huxerui::Button("First VPN disconnected").OnClick([model] {
            model->Publish({"VPN-1:idle", "VPN-2:failed"});
        }),
        huxerui::Button("Publish identical VPN result").OnClick([model] {
            model->Publish(model->view.Get());
        }),
        huxerui::Text(pollResult.Get()),
        huxerui::VirtualGrid(cards.Size(), [cards](std::size_t index) {
            return huxerui::Text(cards[index]).Key(static_cast<std::int64_t>(index));
        }).Columns(huxerui::GridColumns::Fixed(1)).EstimatedRowExtent(40.0F)
            .With(huxerui::Grow(1.0F)),
    }.With(huxerui::Grow(1.0F));
}
huxerui::View VpnPublicationRoot() { return huxerui::FlatTheme{VpnPublicationContent()}; }
const huxerui::Application kVpnPublicationApplication{VpnPublicationRoot,
    {.show_debug_overlay = false, .application_hooks = {InstallTestVpnModel}}};

void testVpnPublication() {
    huxerui::testing::UiTest ui(kVpnPublicationApplication, {.viewport = {700.0F, 700.0F}});
    ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("VPN-1:idle")).Exists(), "VPN cards receive the initial snapshot");
    ui.Find(UiSelector::Text("Start delayed VPN poll")).Tap(); ui.Pump();
    ui.Find(UiSelector::Text("First VPN connected")).Tap(); ui.Pump();
    check(ui.Find(UiSelector::Text("VPN-1:connected")).Exists() &&
          ui.Find(UiSelector::Text("VPN-2:connecting")).Exists(),
          "first VPN card updates in the completion frame while the next connection is pending");
    ui.Pump(std::chrono::milliseconds(500)); ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("POLL-rejected")).Exists() &&
          ui.Find(UiSelector::Text("VPN-1:connected")).Exists(),
          "late poll cannot replace an action's connected snapshot");
    ui.Find(UiSelector::Text("Second VPN failed")).Tap(); ui.Pump();
    check(ui.Find(UiSelector::Text("VPN-2:failed")).Exists(), "VPN failure updates without waiting for the one-second pump");
    ui.Find(UiSelector::Text("Start delayed VPN poll")).Tap(); ui.Pump();
    ui.Find(UiSelector::Text("First VPN disconnected")).Tap(); ui.Pump();
    check(ui.Find(UiSelector::Text("VPN-1:idle")).Exists(), "VPN disconnect immediately clears the connected card");
    ui.Pump(std::chrono::milliseconds(500)); ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("POLL-rejected")).Exists() &&
          ui.Find(UiSelector::Text("VPN-1:idle")).Exists(), "late connected poll cannot resurrect a disconnected card");
    ui.Find(UiSelector::Text("Start delayed VPN poll")).Tap(); ui.Pump();
    ui.Find(UiSelector::Text("Publish identical VPN result")).Tap(); ui.Pump();
    ui.Pump(std::chrono::milliseconds(500)); ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("POLL-rejected")).Exists(), "equal action snapshots still invalidate in-flight polls (ABA)");
    ui.Find(UiSelector::Text("Start delayed VPN poll")).Tap(); ui.Pump();
    ui.Pump(std::chrono::milliseconds(500)); ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("POLL-accepted")).Exists() &&
          ui.Find(UiSelector::Text("VPN-1:external")).Exists(), "unchanged polling continues to publish external VPN changes");
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
    check(ReplaceCustomThemeColor(saved, "#336699", "#AA44CC") ==
              std::vector<std::string>{"#AA44CC"},
          "editing a custom color replaces its entry and deduplicates collisions");
    check(RemoveCustomThemeColor(saved, "#AA44CC") == std::vector<std::string>{"#336699"},
          "deleting a custom color only removes the requested swatch");
    const auto hiddenPresets = ReadHiddenThemeColors("blue\nunknown\nblue\norange");
    check(hiddenPresets == std::vector<std::string>{"blue", "orange"},
          "hidden preset colors accept only known IDs and deduplicate them");
    check(HidePresetThemeColor(hiddenPresets, "teal") ==
              std::vector<std::string>{"blue", "orange", "teal"},
          "hiding a preset preserves existing hidden colors and appends its ID");
    check(SaveHiddenThemeColors(hiddenPresets) == "blue\norange",
          "hidden preset colors round trip using newline-separated stable IDs");
    check(ThemeColorFromHex("#336699") == huxerui::Color::Rgb(51, 102, 153), "hex channels decode correctly");
    const auto red = ThemeColorToHsb(huxerui::Color::Rgb(255, 0, 0));
    const auto green = ThemeColorToHsb(huxerui::Color::Rgb(0, 255, 0));
    const auto blue = ThemeColorToHsb(huxerui::Color::Rgb(0, 0, 255));
    check(std::abs(red.hue) < 0.01F && std::abs(red.saturation - 1.0F) < 0.01F &&
              std::abs(red.brightness - 1.0F) < 0.01F,
          "HSB conversion maps red to zero hue with full saturation and brightness");
    check(std::abs(green.hue - 120.0F) < 0.01F &&
              std::abs(blue.hue - 240.0F) < 0.01F,
          "HSB conversion maps the green and blue hue sectors");
    const HsbColor sample{217.0F, 0.73F, 0.82F};
    const auto roundTrip = ThemeColorToHsb(ThemeColorFromHsb(sample));
    check(std::abs(roundTrip.hue - sample.hue) < 0.01F &&
              std::abs(roundTrip.saturation - sample.saturation) < 0.01F &&
              std::abs(roundTrip.brightness - sample.brightness) < 0.01F,
          "HSB-to-RGB conversion preserves a representative color");
    check(ThemeColorToHex(ThemeColorFromHsb({kHsbTurnDegrees, 1.0F, 1.0F})) == "#FF0000",
          "the hue slider's 360-degree endpoint wraps to red");
    const auto hueAdjusted = UpdateHsbChannel(sample, HsbChannel::Hue, 120.0F);
    check(std::abs(hueAdjusted.hue - 120.0F) < 0.01F &&
              std::abs(hueAdjusted.saturation - sample.saturation) < 0.01F &&
              std::abs(hueAdjusted.brightness - sample.brightness) < 0.01F,
          "hue slider changes hue while preserving saturation and brightness");
    const auto saturationAdjusted = UpdateHsbChannel(sample, HsbChannel::Saturation, 45.0F);
    check(std::abs(saturationAdjusted.hue - sample.hue) < 0.01F &&
              std::abs(saturationAdjusted.saturation - 0.45F) < 0.01F &&
              std::abs(saturationAdjusted.brightness - sample.brightness) < 0.01F,
          "saturation slider changes saturation under the current hue and brightness");
    const auto brightnessAdjusted = UpdateHsbChannel(sample, HsbChannel::Brightness, 61.0F);
    check(std::abs(brightnessAdjusted.hue - sample.hue) < 0.01F &&
              std::abs(brightnessAdjusted.saturation - sample.saturation) < 0.01F &&
              std::abs(brightnessAdjusted.brightness - 0.61F) < 0.01F,
          "brightness slider changes brightness while preserving hue and saturation");
    const auto hueGradient = HsbChannelGradient(sample, HsbChannel::Hue);
    check(hueGradient.stops.size() == kHsbHueSectorCount + 1U &&
              hueGradient.stops.front().color == huxerui::Color::Rgb(255, 0, 0) &&
              hueGradient.stops.back().color == huxerui::Color::Rgb(255, 0, 0) &&
              hueGradient.stops[1].color == huxerui::Color::Rgb(255, 255, 0) &&
              hueGradient.stops[3].color == huxerui::Color::Rgb(0, 255, 255),
          "hue slider displays the full rainbow spectrum");
    const auto saturationGradient = HsbChannelGradient(sample, HsbChannel::Saturation);
    check(saturationGradient.stops.size() == 2U &&
              saturationGradient.stops.front().color ==
                  ThemeColorFromHsb({sample.hue, kHsbUnitMinimum, sample.brightness}) &&
              saturationGradient.stops.back().color ==
                  ThemeColorFromHsb({sample.hue, kHsbUnitMaximum, sample.brightness}),
          "saturation slider ranges from gray to the current hue at current brightness");
    const auto brightnessGradient = HsbChannelGradient(sample, HsbChannel::Brightness);
    check(brightnessGradient.stops.size() == 2U &&
              brightnessGradient.stops.front().color == huxerui::Color::Black() &&
              brightnessGradient.stops.back().color ==
                  ThemeColorFromHsb({sample.hue, sample.saturation, kHsbUnitMaximum}),
          "brightness slider ranges from black to the current hue and saturation");
    auto baseSliderStyle = huxerui::SliderStyle::Default();
    baseSliderStyle.width = 211.0F;
    baseSliderStyle.inactive_track = huxerui::Color::Rgb(42, 54, 66);
    baseSliderStyle.active_track = huxerui::Color::Rgb(0, 0, 255);
    baseSliderStyle.disabled_thumb.alpha = 0.38F;
    baseSliderStyle.focus_ring = huxerui::FocusRing{huxerui::Color::Rgb(0, 0, 255), 2.0F, 3.0F};
    const auto sliderTarget = huxerui::Color::Rgb(240, 72, 24);
    const auto gradientSlider = HsbGradientSliderStyle(baseSliderStyle, sliderTarget);
    const auto transparent = huxerui::Color::Transparent();
    check(gradientSlider.active_track == transparent && gradientSlider.inactive_track == transparent &&
              gradientSlider.active_tick == transparent && gradientSlider.inactive_tick == transparent &&
              gradientSlider.stop_indicator == transparent,
          "HSB sliders expose their independent gradient rails without theme-colored overlays");
    check(gradientSlider.thumb == sliderTarget && gradientSlider.width == baseSliderStyle.width &&
              gradientSlider.focus_ring->color == sliderTarget &&
              std::abs(gradientSlider.disabled_thumb.alpha - 0.38F) < 0.001F,
          "HSB slider keeps the selected-color thumb and themed geometry");
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

namespace {
struct ControlFixture {
    clashflux::ui::CoreOperations operations;
    huxerui::State<std::string> result{std::string{"CONTROL-IDLE"}};
    huxerui::State<std::string> poll{std::string{}};
    huxerui::State<int> executions{0};
    huxerui::State<int> resumedCommands{0};
    huxerui::State<bool> tunIntent{false};
};

struct BoxCatFixture {
    huxerui::State<clashflux::ui::BoxCatPhase> phase{clashflux::ui::BoxCatPhase::Sleeping};
    huxerui::State<bool> motion{true};
    huxerui::State<bool> cardShown{false};
    huxerui::State<clashflux::ui::HomeControlMotion> presentation{
        clashflux::ui::HomeControlMotion{}};
    int readyCount = 0;
    std::shared_ptr<clashflux::ui::BoxCatProbe> probe =
        std::make_shared<clashflux::ui::BoxCatProbe>();
};
std::shared_ptr<BoxCatFixture> boxCatFixture;
clashflux::ui::BoxCatPhase boxCatInitialPhase = clashflux::ui::BoxCatPhase::Sleeping;

void InstallBoxCatFixture(huxerui::ApplicationContext& context) {
    boxCatFixture = std::make_shared<BoxCatFixture>();
    boxCatFixture->phase = boxCatInitialPhase;
    context.Provide(boxCatFixture);
}

[[huxerui::composable]] huxerui::View BoxCatFixtureContent(
    bool statusIcon = false, bool morphToCard = false) {
    const auto model = huxerui::UseService<BoxCatFixture>();
    const auto theme = huxerui::UseTheme();
    const auto transition = huxerui::UseSharedTransition();
    const auto tasks = huxerui::UseTaskScope();
    const auto presentation = model->presentation.Get();
    const bool moving = presentation.action != clashflux::ui::CoreAction::Idle;
    const bool returning = presentation.action == clashflux::ui::CoreAction::Stop && !presentation.collapsing;
    const auto setPhase = [model, transition, tasks, morphToCard,
                          reducedMotion = theme.motion.reduced_motion](clashflux::ui::BoxCatPhase phase) {
        return [model, transition, tasks, morphToCard, reducedMotion, phase] {
            if (morphToCard && model->cardShown.Get() &&
                (phase == clashflux::ui::BoxCatPhase::Stopping ||
                 phase == clashflux::ui::BoxCatPhase::Sleeping)) {
                clashflux::ui::AnimateHomeControlCard(false, model->cardShown,
                    model->presentation, transition, tasks, reducedMotion);
                model->phase = phase;
                return;
            }
            model->phase = phase;
            if (phase != clashflux::ui::BoxCatPhase::Running) model->cardShown = false;
        };
    };
    const auto ready = [model, transition, tasks, morphToCard,
                       reducedMotion = theme.motion.reduced_motion] {
        if (model->phase.Get() != clashflux::ui::BoxCatPhase::Running) return;
        ++model->readyCount;
        if (morphToCard) {
            clashflux::ui::AnimateHomeControlCard(true, model->cardShown,
                model->presentation, transition, tasks, reducedMotion);
        }
    };
    huxerui::View cat = statusIcon || model->cardShown.Get() ? huxerui::View{huxerui::Row {
        clashflux::ui::BoxCatStatusIcon(model->phase.Get(), model->motion.Get() && !moving, model->probe)
            .Key("production-status-badge"),
        huxerui::Column {
            huxerui::Text("Running"), huxerui::Text("00:00:15"),
        },
    }.With(huxerui::Frame{.width = 280.0F, .height = 88.0F},
           huxerui::Background(theme.colors.surface_container),
           huxerui::CornerRadius(14.0F),
           huxerui::Spacing(6.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
           huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
           huxerui::Scale(huxerui::AnimateTo(presentation.collapsing
               ? clashflux::ui::kHomeControlCollapsedScale : 1.0F,
               clashflux::ui::kHomeControlCollapseMotion)))}
        : clashflux::ui::BoxCatButtonContent(model->phase.Get(), theme.colors.on_surface,
            model->motion.Get() && !moving, model->probe, ready,
            returning ? std::optional{clashflux::ui::BoxCatPhase::Running} : std::nullopt);
    if (morphToCard && presentation.collapsed) {
        cat = huxerui::Stack {
            clashflux::ui::BoxCatButtonContent(clashflux::ui::BoxCatPhase::Running,
                theme.colors.on_surface, false, model->probe)
                .With(huxerui::SharedBounds("home-core-control")
                    .BoundsTransform(clashflux::ui::HomeControlReturnToDock{})),
        }.With(huxerui::Frame{.width = 280.0F, .height = 88.0F},
               huxerui::Align(huxerui::HorizontalAlignment::Center,
                              huxerui::VerticalAlignment::Center));
    } else if (morphToCard) {
        auto bounds = huxerui::SharedBounds("home-core-control");
        if (returning) bounds = std::move(bounds).BoundsTransform(clashflux::ui::HomeControlReturnToDock{});
        else bounds = std::move(bounds).BoundsTransform(clashflux::ui::HomeControlMoveExpand{});
        cat = std::move(cat).With(std::move(bounds));
    }
    return huxerui::Column {
        std::move(cat).Key("production-box-cat"),
        huxerui::Button("Cat sleep").OnClick(setPhase(clashflux::ui::BoxCatPhase::Sleeping)),
        huxerui::Button("Cat start").OnClick(setPhase(clashflux::ui::BoxCatPhase::Starting)),
        huxerui::Button("Cat run").OnClick(setPhase(clashflux::ui::BoxCatPhase::Running)),
        huxerui::Button("Cat stop").OnClick(setPhase(clashflux::ui::BoxCatPhase::Stopping)),
        huxerui::Button("Cat fail").OnClick(setPhase(clashflux::ui::BoxCatPhase::Failed)),
        huxerui::Button("Cat motion").OnClick([model] { model->motion = !model->motion.Get(); }),
    }.With(transition.Scope());
}

huxerui::View BoxCatFixtureRoot() {
    return huxerui::FlatTheme{huxerui::FlatLightThemeSpec(), BoxCatFixtureContent()};
}
huxerui::View DarkBoxCatFixtureRoot() {
    return huxerui::FlatTheme{huxerui::FlatDarkThemeSpec(), BoxCatFixtureContent()};
}
huxerui::View BoxCatStatusFixtureRoot() {
    return huxerui::FlatTheme{huxerui::FlatDarkThemeSpec(), BoxCatFixtureContent(true)};
}
huxerui::View BoxCatMorphFixtureRoot() {
    return huxerui::FlatTheme{huxerui::FlatDarkThemeSpec(), BoxCatFixtureContent(false, true)};
}
huxerui::View ReducedBoxCatMorphFixtureRoot() {
    auto theme = huxerui::FlatDarkThemeSpec();
    theme.motion.reduced_motion = true;
    return huxerui::FlatTheme{theme, BoxCatFixtureContent(false, true)};
}
huxerui::View ReducedBoxCatFixtureRoot() {
    huxerui::ThemeSpec theme = huxerui::FlatLightThemeSpec();
    theme.motion.reduced_motion = true;
    return huxerui::FlatTheme(theme, BoxCatFixtureContent());
}
const huxerui::Application kBoxCatApplication{BoxCatFixtureRoot,
    {.show_debug_overlay = false, .application_hooks = {InstallBoxCatFixture}}};
const huxerui::Application kReducedBoxCatApplication{ReducedBoxCatFixtureRoot,
    {.show_debug_overlay = false, .application_hooks = {InstallBoxCatFixture}}};
const huxerui::Application kDarkBoxCatApplication{DarkBoxCatFixtureRoot,
    {.show_debug_overlay = false, .application_hooks = {InstallBoxCatFixture}}};
const huxerui::Application kBoxCatStatusApplication{BoxCatStatusFixtureRoot,
    {.show_debug_overlay = false, .application_hooks = {InstallBoxCatFixture}}};
const huxerui::Application kBoxCatMorphApplication{BoxCatMorphFixtureRoot,
    {.show_debug_overlay = false, .application_hooks = {InstallBoxCatFixture}}};
const huxerui::Application kReducedBoxCatMorphApplication{ReducedBoxCatMorphFixtureRoot,
    {.show_debug_overlay = false, .application_hooks = {InstallBoxCatFixture}}};

void checkBoxCatButton(huxerui::testing::UiTest& ui, const huxerui::ThemeSpec& theme) {
    const auto button = ui.Find(UiSelector::Key("production-box-cat")).One().bounds;
    const auto artwork = ui.Find(UiSelector::Key("box-cat-artwork")).One().bounds;
    check(button.width == 56.0F && button.height == 56.0F &&
          artwork.width == 48.0F && artwork.height == 48.0F &&
          std::abs(artwork.x + 24.0F - button.x - 28.0F) < 0.01F &&
          std::abs(artwork.y + 24.0F - button.y - 28.0F) < 0.01F,
          "production 48pt cat is centered inside the 56pt button");
    // The retained animation emits transform commands before its text, unlike
    // a Text node whose foreground sequence starts directly with DrawText.
    const auto symbols = paintedTextColor(ui, "z", "");
    check(colorDistance(symbols, theme.colors.on_surface) < 0.001F && symbols.alpha > 0,
          "transparent cat sleep symbols inherit the theme's neutral foreground");
    const auto snapshot = ui.CaptureSnapshot().ToString();
    static const std::regex filledButton{
        R"(rect\s+\{\s+bounds\s+\{\s+x=[\d.-]+\s+y=[\d.-]+\s+width=56\.000000\s+height=56\.000000)"};
    check(!std::regex_search(snapshot, filledButton),
          "the full-size cat hit target does not paint a button background");
    const auto before = ui.CaptureSnapshot();
    ui.Pump(std::chrono::milliseconds(100));
    check(ui.CaptureSnapshot() != before,
          "sleep symbols change actual paint at button size, not just the clock probe");
}

void testBoxCat() {
    using clashflux::ui::BoxCatPhase;
    boxCatInitialPhase = BoxCatPhase::Sleeping;
    huxerui::testing::UiTest ui(kBoxCatApplication, {
        .viewport = {500.0F, 700.0F}, .resource_provider = AppResources()});
    ui.UpdateWindowLifecycleState(huxerui::WindowLifecycleState::Active);
    const auto model = boxCatFixture;
    const auto tap = [&ui](const char* label) {
        ui.Find(UiSelector::Text(label)).Tap();
        ui.Pump();
    };
    const auto advance = [&ui](int milliseconds) {
        for (int elapsed = 0; elapsed < milliseconds; elapsed += 50)
            ui.Pump(std::chrono::milliseconds(std::min(50, milliseconds - elapsed)));
    };
    advance(250);
    check(model->probe->sleepLooping && model->probe->sleepTicks > 0 &&
          model->probe->sawSleep && model->probe->progress == 0.0F,
          "production cat starts in the settled sleeping pose with bounded zzz frames");
    checkBoxCatButton(ui, huxerui::FlatLightThemeSpec());
    ui.SetWindowMetrics({.viewport = {390.0F, 700.0F}});
    ui.Pump();
    checkBoxCatButton(ui, huxerui::FlatLightThemeSpec());

    tap("Cat start");
    advance(550);
    check(model->probe->loadingLooping && model->probe->handVisible &&
          model->probe->loadingTicks > 0 && model->probe->sawPetting &&
          std::abs(model->probe->progress - 0.30F) < 0.02F,
          "production cat keeps petting while the real start operation is pending");
    const auto waitingTicks = model->probe->loadingTicks;
    advance(250);
    check(model->probe->loadingTicks > waitingTicks &&
          std::abs(model->probe->progress - 0.30F) < 0.02F,
          "pending cat animation does not invent startup completion");

    tap("Cat motion");
    const auto pausedTicks = model->probe->loadingTicks;
    const float pausedProgress = model->probe->progress;
    advance(250);
    check(model->probe->loadingTicks == pausedTicks &&
          model->probe->progress == pausedProgress && !model->probe->loadingLooping,
          "shared-bounds suspension freezes production cat paint clocks");
    tap("Cat motion");

    tap("Cat run");
    advance(1800);
    check(model->probe->progress == 1.0F && model->probe->looping &&
          model->probe->ticks > 0 && model->probe->sawEars && model->probe->sawTail &&
          model->probe->sawHandRetiredAfterRise && !model->probe->handVisible,
          "confirmed running state completes the rise and starts ear/tail loops");

    ui.UpdateWindowLifecycleState(huxerui::WindowLifecycleState::Background);
    ui.Pump();
    const auto backgroundTicks = model->probe->ticks;
    advance(250);
    check(model->probe->ticks == backgroundTicks && !model->probe->looping,
          "background window suspends production cat loops");
    ui.UpdateWindowLifecycleState(huxerui::WindowLifecycleState::Active);
    advance(250);
    check(model->probe->ticks > backgroundTicks && model->probe->looping,
          "foreground resume continues the current running pose without a new transition");

    tap("Cat stop");
    advance(1500);
    check(model->probe->progress == 0.0F && model->probe->sleepLooping &&
          !model->probe->handVisible && !model->probe->looping,
          "stopping returns the cat to sleep without reversing the petting hand");

    tap("Cat fail");
    advance(1400);
    check(model->probe->failurePlaying && model->probe->sawWink,
          "failure presents the production peek and wink timeline");
    advance(1400);
    check(model->probe->failurePlays == 1 && !model->probe->failurePlaying &&
          model->probe->progress == 0.0F && model->probe->sleepLooping,
          "failure finishes once and returns to the sleeping pose");
    advance(300);
    check(model->probe->failurePlays == 1, "persistent failure state does not replay the wink");
    tap("Cat start");
    advance(450);
    tap("Cat fail");
    advance(2800);
    check(model->probe->failurePlays == 2 && model->probe->sleepLooping,
          "a new start/failure cycle permits exactly one new wink");

    boxCatInitialPhase = BoxCatPhase::Running;
    huxerui::testing::UiTest settled(kBoxCatApplication, {
        .viewport = {500.0F, 700.0F}, .resource_provider = AppResources()});
    settled.UpdateWindowLifecycleState(huxerui::WindowLifecycleState::Active);
    settled.Pump(std::chrono::milliseconds(250));
    check(boxCatFixture->probe->progress == 1.0F && boxCatFixture->probe->looping &&
          !boxCatFixture->probe->transitioning && boxCatFixture->probe->failurePlays == 0,
          "mounting an already running control does not replay the startup animation");

    huxerui::testing::UiTest status(kBoxCatStatusApplication, {
        .viewport = {500.0F, 700.0F}, .resource_provider = AppResources()});
    status.UpdateWindowLifecycleState(huxerui::WindowLifecycleState::Active);
    const auto statusModel = boxCatFixture;
    status.Pump(std::chrono::milliseconds(250));
    const auto badge = status.Find(UiSelector::Key("production-status-badge")).One().bounds;
    const auto artwork = status.Find(UiSelector::Key("box-cat-artwork")).One().bounds;
    check(badge.width == 36.0F && badge.height == 36.0F &&
          artwork.width == 36.0F && artwork.height == 36.0F &&
          std::abs(artwork.x - badge.x) < 0.01F && std::abs(artwork.y - badge.y) < 0.01F,
          "running card gives its paint-only cat a nonzero centered 36pt layout");
    const auto statusSnapshot = status.CaptureSnapshot().ToString();
    static const std::regex filledBadge{
        R"(rect\s+\{\s+bounds\s+\{\s+x=[\d.-]+\s+y=[\d.-]+\s+width=36\.000000\s+height=36\.000000)"};
    check(!std::regex_search(statusSnapshot, filledBadge),
          "the running cat has no colored circular backing");
    const auto statusPaint = status.CaptureSnapshot();
    const auto statusTicks = statusModel->probe->ticks;
    for (int i = 0; i < 20; ++i) status.Pump(std::chrono::milliseconds(50));
    check(statusModel->probe->looping && statusModel->probe->ticks > statusTicks &&
          statusModel->probe->sawEars && statusModel->probe->sawTail &&
          status.CaptureSnapshot() != statusPaint,
          "running card paints its moving ears and tail after mounting");
    status.Find(UiSelector::Text("Cat motion")).Tap(); status.Pump();
    const auto suspendedTicks = statusModel->probe->ticks;
    status.Pump(std::chrono::milliseconds(250));
    check(statusModel->probe->ticks == suspendedTicks && !statusModel->probe->looping &&
          status.Find(UiSelector::Key("box-cat-artwork")).One().bounds.width == 36.0F,
          "shared-bounds suspension keeps the running card artwork sized and visible");
    status.Find(UiSelector::Text("Cat motion")).Tap(); status.Pump(std::chrono::milliseconds(100));
    check(statusModel->probe->ticks > suspendedTicks && statusModel->probe->looping,
          "running card resumes idle animation after shared-bounds suspension");

    boxCatInitialPhase = BoxCatPhase::Sleeping;
    huxerui::testing::UiTest reduced(kReducedBoxCatApplication, {
        .viewport = {500.0F, 700.0F}, .resource_provider = AppResources()});
    reduced.UpdateWindowLifecycleState(huxerui::WindowLifecycleState::Active);
    const auto reducedModel = boxCatFixture;
    reduced.Pump(std::chrono::milliseconds(200));
    check(!reducedModel->probe->sleepLooping && reducedModel->probe->sleepTicks == 0,
          "reduced motion disables sleeping frame requests");
    reduced.Find(UiSelector::Text("Cat start")).Tap(); reduced.Pump(std::chrono::milliseconds(150));
    check(reducedModel->probe->progress == 0.30F && reducedModel->probe->handVisible &&
          !reducedModel->probe->loadingLooping && reducedModel->probe->loadingTicks == 0,
          "reduced motion keeps only the static petting hand");
    reduced.Find(UiSelector::Text("Cat run")).Tap(); reduced.Pump(std::chrono::milliseconds(150));
    check(reducedModel->probe->progress == 1.0F && !reducedModel->probe->looping &&
          reducedModel->probe->ticks == 0,
          "reduced motion resolves running directly without ear/tail loops");
    reduced.Find(UiSelector::Text("Cat fail")).Tap(); reduced.Pump(std::chrono::milliseconds(150));
    check(reducedModel->probe->progress == 0.0F && !reducedModel->probe->failurePlaying &&
          reducedModel->probe->failurePlays == 0,
          "reduced motion resolves failure directly to the stable sleeping pose");
    boxCatInitialPhase = BoxCatPhase::Sleeping;
    huxerui::testing::UiTest dark(kDarkBoxCatApplication, {
        .viewport = {500.0F, 700.0F}, .resource_provider = AppResources()});
    dark.UpdateWindowLifecycleState(huxerui::WindowLifecycleState::Active);
    dark.Pump(std::chrono::milliseconds(250));
    checkBoxCatButton(dark, huxerui::FlatDarkThemeSpec());
}

// Read the actual retained render tree, including shared-overlay transforms.
// Semantic layout bounds alone describe the destination, not the moving card.
struct CatPaintNode {
    std::string name, value;
    std::vector<CatPaintNode> children;
    const CatPaintNode* Find(std::string_view key) const {
        for (const auto& child : children) if (child.name == key) return &child;
        return nullptr;
    }
    float Number(std::string_view key, float fallback = 0) const {
        const auto* child = Find(key);
        return child ? std::stof(child->value) : fallback;
    }
};

CatPaintNode readCatPaintNode(std::istream& input, std::string line) {
    line.erase(0, line.find_first_not_of(' '));
    CatPaintNode node;
    if (line.ends_with(" {")) {
        node.name = line.substr(0, line.size() - 2);
        while (std::getline(input, line)) {
            line.erase(0, line.find_first_not_of(' '));
            if (line == "}") break;
            node.children.push_back(readCatPaintNode(input, line));
        }
    } else {
        const auto equals = line.find('=');
        node.name = line.substr(0, equals);
        if (equals != std::string::npos) node.value = line.substr(equals + 1);
    }
    return node;
}

huxerui::Transform2D catPaintTransform(const CatPaintNode* node) {
    if (!node) return {};
    return {node->Number("m11", 1), node->Number("m12"), node->Number("m21"),
            node->Number("m22", 1), node->Number("translate_x"), node->Number("translate_y")};
}

huxerui::Transform2D composeCatPaint(huxerui::Transform2D a, huxerui::Transform2D b) {
    return {a.m11 * b.m11 + a.m21 * b.m12, a.m12 * b.m11 + a.m22 * b.m12,
            a.m11 * b.m21 + a.m21 * b.m22, a.m12 * b.m21 + a.m22 * b.m22,
            a.m11 * b.translate_x + a.m21 * b.translate_y + a.translate_x,
            a.m12 * b.translate_x + a.m22 * b.translate_y + a.translate_y};
}

std::optional<huxerui::Rect> paintedCatCard(huxerui::testing::UiTest& ui) {
    std::istringstream input(ui.CaptureSnapshot().ToString());
    std::string line;
    std::getline(input, line); std::getline(input, line);
    const auto snapshot = readCatPaintNode(input, line);
    std::optional<huxerui::Rect> result;
    const auto visit = [&](auto&& self, const CatPaintNode& node, huxerui::Transform2D parent) -> void {
        auto local = catPaintTransform(node.Find("transform"));
        if (const auto* offset = node.Find("offset")) {
            local.translate_x += offset->Number("x");
            local.translate_y += offset->Number("y");
        }
        const auto world = composeCatPaint(parent, local);
        if (const auto* content = node.Find("content")) {
            for (const auto& command : content->children) {
                const auto* bounds = command.Find("bounds");
                if (command.name == "rect" && bounds &&
                    bounds->Number("width") == 280.0F && bounds->Number("height") == 88.0F) {
                    result = huxerui::Rect{
                        world.m11 * bounds->Number("x") + world.translate_x,
                        world.m22 * bounds->Number("y") + world.translate_y,
                        world.m11 * 280.0F, world.m22 * 88.0F};
                }
            }
        }
        if (const auto* children = node.Find("children"))
            for (const auto& child : children->children)
                self(self, child, composeCatPaint(world, catPaintTransform(node.Find("children_transform"))));
    };
    if (const auto* render = snapshot.Find("render"))
        for (const auto& node : render->children) visit(visit, node, huxerui::Transform2D{});
    return result;
}

struct PaintedHeaderCard {
    huxerui::Rect bounds;
    float outlineWidth;
    float cornerRadius;
};

std::vector<huxerui::Rect> paintedHomeCats(huxerui::testing::UiTest& ui) {
    std::istringstream input(ui.CaptureSnapshot().ToString());
    std::string line;
    std::getline(input, line); std::getline(input, line);
    const auto snapshot = readCatPaintNode(input, line);
    std::vector<huxerui::Rect> result;
    const auto visit = [&](auto&& self, const CatPaintNode& node, huxerui::Transform2D parent,
                           float opacity) -> void {
        if (const auto* visible = node.Find("visible"); visible && visible->value == "false") return;
        opacity *= node.Number("opacity", 1.0F);
        if (opacity < 0.01F) return;
        auto local = catPaintTransform(node.Find("transform"));
        if (const auto* offset = node.Find("offset")) {
            local.translate_x += offset->Number("x");
            local.translate_y += offset->Number("y");
        }
        const auto world = composeCatPaint(parent, local);
        if (const auto* foreground = node.Find("foreground"); foreground && !foreground->children.empty() &&
            foreground->children.front().name == "push-transform") {
            const auto artwork = composeCatPaint(world,
                catPaintTransform(foreground->children.front().Find("transform")));
            result.push_back({artwork.translate_x, artwork.translate_y,
                              artwork.m11 * 128.0F, artwork.m22 * 128.0F});
        }
        if (const auto* children = node.Find("children"))
            for (const auto& child : children->children)
                self(self, child, composeCatPaint(world, catPaintTransform(node.Find("children_transform"))), opacity);
    };
    if (const auto* render = snapshot.Find("render"))
        for (const auto& node : render->children) visit(visit, node, huxerui::Transform2D{}, 1.0F);
    return result;
}

std::optional<PaintedHeaderCard> paintedHeaderCard(huxerui::testing::UiTest& ui) {
    std::istringstream input(ui.CaptureSnapshot().ToString());
    std::string line;
    std::getline(input, line); std::getline(input, line);
    const auto snapshot = readCatPaintNode(input, line);
    std::optional<PaintedHeaderCard> result;
    float outlineWidth = 0.0F;
    const auto visit = [&](auto&& self, const CatPaintNode& node, huxerui::Transform2D parent) -> void {
        auto local = catPaintTransform(node.Find("transform"));
        if (const auto* offset = node.Find("offset")) {
            local.translate_x += offset->Number("x");
            local.translate_y += offset->Number("y");
        }
        const auto world = composeCatPaint(parent, local);
        if (const auto* content = node.Find("content")) {
            for (const auto& command : content->children) {
                const auto* bounds = command.Find("bounds");
                if (command.name == "border" && bounds && bounds->Number("height") == 48.0F &&
                    bounds->Number("width") > 200.0F) {
                    const auto* style = command.Find("style");
                    outlineWidth = style ? style->Number("width") : 0.0F;
                }
                if (command.name == "rect" && bounds && bounds->Number("height") == 48.0F &&
                    bounds->Number("width") > 200.0F) {
                    result = PaintedHeaderCard{
                        {world.m11 * bounds->Number("x") + world.translate_x,
                         world.m22 * bounds->Number("y") + world.translate_y,
                         world.m11 * bounds->Number("width"),
                         world.m22 * bounds->Number("height")},
                        0.0F,
                        command.Number("corner_radius")};
                }
            }
        }
        if (const auto* children = node.Find("children"))
            for (const auto& child : children->children)
                self(self, child, composeCatPaint(world, catPaintTransform(node.Find("children_transform"))));
    };
    if (const auto* render = snapshot.Find("render"))
        for (const auto& node : render->children) visit(visit, node, huxerui::Transform2D{});
    if (result) result->outlineWidth = outlineWidth;
    return result;
}

void testBoxCatMorphAfterSuccess() {
    using clashflux::ui::BoxCatPhase;
    const clashflux::ui::HomeControlMoveExpand path;
    const huxerui::Rect dock{800.0F, 620.0F, 56.0F, 56.0F};
    const huxerui::Rect card{400.0F, 280.0F, 280.0F, 88.0F};
    const auto shrinking = path.Evaluate(card, dock, 0.25F);
    check(shrinking.width < card.width && shrinking.width > dock.width &&
          std::abs(shrinking.x + shrinking.width / 2 - card.x - card.width / 2) < 0.001F &&
          std::abs(shrinking.y + shrinking.height / 2 - card.y - card.height / 2) < 0.001F,
          "collapse first shrinks the card in place before moving toward the dock");
    const auto returning = path.Evaluate(card, dock, 0.75F);
    check(returning.width == dock.width && returning.height == dock.height &&
          returning.x > card.x + card.width / 2 - dock.width / 2 && returning.x < dock.x &&
          path.Evaluate(card, dock, 1.0F) == dock,
          "only the collapsed button moves back to its original dock bounds");
    for (float progress : {0.0F, 0.25F, 0.5F, 0.75F, 1.0F}) {
        const auto expand = path.Evaluate(dock, card, progress);
        const auto collapse = path.Evaluate(card, dock, 1.0F - progress);
        check(std::abs(expand.x - collapse.x) < 0.001F &&
              std::abs(expand.y - collapse.y) < 0.001F &&
              std::abs(expand.width - collapse.width) < 0.001F &&
              std::abs(expand.height - collapse.height) < 0.001F,
              "card collapse follows the expansion track in reverse");
    }
    boxCatInitialPhase = BoxCatPhase::Sleeping;
    huxerui::testing::UiTest ui(kBoxCatMorphApplication, {
        .viewport = {900.0F, 700.0F}, .resource_provider = AppResources()});
    ui.UpdateWindowLifecycleState(huxerui::WindowLifecycleState::Active);
    const auto model = boxCatFixture;
    const auto advance = [&ui](int milliseconds) {
        for (int elapsed = 0; elapsed < milliseconds; elapsed += 50)
            ui.Pump(std::chrono::milliseconds(std::min(50, milliseconds - elapsed)));
    };
    const auto tap = [&ui](const char* text) { ui.Find(UiSelector::Text(text)).Tap(); ui.Pump(); };
    tap("Cat start");
    advance(2000);
    check(!model->cardShown.Get() && model->readyCount == 0 &&
          model->probe->handVisible && model->probe->loadingLooping &&
          !ui.Find(UiSelector::Key("production-status-badge")).Exists(),
          "slow startup keeps petting in the floating button without creating a card");
    tap("Cat run");
    advance(300);
    check(!model->cardShown.Get() && model->readyCount == 0 && model->probe->transitioning,
          "confirmed success still finishes the cat's rise before merging");
    advance(1200);
    check(model->cardShown.Get() && model->readyCount == 1 &&
          !model->probe->handVisible && model->probe->sawHandRetiredAfterRise &&
          ui.Find(UiSelector::Key("production-status-badge")).Exists(),
          "only the completed successful pose triggers the shared-bounds card merge");
    advance(1000);
    check(model->readyCount == 1, "running idle frames never repeat the card merge");
    const auto fullCard = paintedCatCard(ui);
    check(fullCard && std::abs(fullCard->width - 280.0F) < 0.01F,
          "running card background is present at its full painted size");

    tap("Cat stop"); advance(200);
    const auto shrinkingCard = paintedCatCard(ui);
    check(fullCard && shrinkingCard && shrinkingCard->width < fullCard->width * 0.6F &&
          shrinkingCard->width > 56.0F && shrinkingCard->height < fullCard->height &&
          std::abs(shrinkingCard->x + shrinkingCard->width / 2 - fullCard->x - fullCard->width / 2) < 0.1F &&
          std::abs(shrinkingCard->y + shrinkingCard->height / 2 - fullCard->y - fullCard->height / 2) < 0.1F,
          "the rendered card actually shrinks in place before retreating");
    if (fullCard && shrinkingCard)
        std::printf("cat collapse painted sizes: %.2fx%.2f -> %.2fx%.2f at center %.2f,%.2f\n",
            fullCard->width, fullCard->height, shrinkingCard->width, shrinkingCard->height,
            shrinkingCard->x + shrinkingCard->width / 2, shrinkingCard->y + shrinkingCard->height / 2);
    check(ui.CaptureSnapshot().ToString().find("text=\"Running\"") != std::string::npos,
          "the outgoing card stays painted while shrinking toward button size");
    check(model->cardShown.Get() && model->presentation.Get().collapsing &&
          model->probe->progress == 1.0F &&
          !model->probe->sleepLooping && !model->probe->handVisible,
          "the real card stays mounted with its running cat throughout the shrink stage");
    // The real stop can finish before the shared-bounds movement. Its stopped
    // snapshot must not discard the outgoing pose held by the new button.
    model->phase = BoxCatPhase::Sleeping;
    ui.Pump(); advance(200);
    check(!paintedCatCard(ui) && !model->presentation.Get().collapsing,
          "after shrinking, the original card is replaced by a committed small cat pose");
    check(model->probe->progress == 1.0F && !model->probe->sleepLooping,
          "a fast stop does not replace the returning cat with a sleeping snapshot");
    advance(600);
    const auto returningPaint = ui.CaptureSnapshot();
    const float returnProgress = model->probe->progress;
    check(returnProgress > 0.0F && returnProgress < 1.0F &&
          model->probe->transitioning && !model->probe->sleepLooping &&
          !model->probe->handVisible,
          "after shrinking, the button animates from running toward sleep without petting");
    advance(400);
    check(model->probe->progress < returnProgress && model->probe->progress > 0.0F &&
          ui.CaptureSnapshot() != returningPaint && !model->probe->sleepLooping,
          "tail, head and eyelids keep painting through the reverse transition");
    advance(1000);
    check(model->probe->progress == 0.0F && model->probe->sleepLooping &&
          !model->probe->transitioning && model->readyCount == 1,
          "zzz resumes only after returning to the settled sleeping pose");

    tap("Cat sleep"); advance(1500);
    tap("Cat start"); advance(450);
    tap("Cat fail"); advance(2800);
    check(!model->cardShown.Get() && model->readyCount == 1 && model->probe->sleepLooping,
          "failed startup stays in the button and returns to sleep without a card flash");
    tap("Cat start"); advance(450);
    tap("Cat stop"); advance(1500);
    check(!model->cardShown.Get() && model->readyCount == 1 && !model->probe->handVisible,
          "cancelling startup cannot create a running card");

    // The command may finish between two UI samples. Even without a rendered
    // Starting frame, a sleeping button must pet and rise before it can merge.
    tap("Cat run"); advance(150);
    check(!model->cardShown.Get() && model->probe->handVisible && model->probe->sawPetting,
          "an immediately successful start still pets before merging");
    tap("Cat stop"); advance(1500);
    check(!model->cardShown.Get() && model->readyCount == 1,
          "stopping during the successful rise discards the unfinished merge");
    tap("Cat start"); advance(450);
    tap("Cat run"); advance(1600);
    check(model->cardShown.Get() && model->readyCount == 2,
          "retry after failure or cancellation merges exactly once on success");
    advance(300);
    check(model->presentation.Get().action == clashflux::ui::CoreAction::Idle,
          "successful card merge finishes before testing another user stop");
    tap("Cat stop"); advance(1000);
    check(model->probe->transitioning && model->probe->progress > 0.0F,
          "a second stop starts another continuous return to sleep");
    tap("Cat start"); advance(150);
    tap("Cat run"); advance(1800);
    check(model->cardShown.Get() && model->readyCount == 3 && !model->probe->sleepLooping,
          "restarting during the return reverses smoothly and merges on fresh success");

    huxerui::testing::UiTest reduced(kReducedBoxCatMorphApplication, {
        .viewport = {900.0F, 700.0F}, .resource_provider = AppResources()});
    reduced.UpdateWindowLifecycleState(huxerui::WindowLifecycleState::Active);
    const auto reducedModel = boxCatFixture;
    reduced.Find(UiSelector::Text("Cat start")).Tap(); reduced.Pump();
    check(!reducedModel->cardShown.Get() && reducedModel->probe->handVisible,
          "reduced motion still waits in the static petting pose for real success");
    reduced.Find(UiSelector::Text("Cat run")).Tap();
    reduced.Pump(std::chrono::milliseconds(50)); reduced.Pump();
    check(reducedModel->cardShown.Get() && reducedModel->readyCount == 1,
          "reduced motion resolves success and merges without waiting for an animation timer");
    reduced.Find(UiSelector::Text("Cat stop")).Tap(); reduced.Pump();
    check(!reducedModel->cardShown.Get() && reducedModel->probe->progress == 0.0F &&
          !reducedModel->probe->transitioning && !reducedModel->probe->sleepLooping,
          "reduced motion returns directly to the static sleeping pose");
    boxCatInitialPhase = BoxCatPhase::Sleeping;
}

void CheckCoreOperationsGate() {
    using namespace clashflux::ui;
    // Exercise the production version gate, including equal-value/ABA changes.
    CoreOperations gate;
    const auto oldPoll = gate.revision.Get();
    const auto first = gate.Begin({.action = CoreAction::Start});
    check(first.has_value() && !gate.Begin({.action = CoreAction::Stop}),
          "all runtime entry points share one in-flight command");
    check(!gate.AcceptsPoll(gate.revision.Get()), "polls cannot publish intermediate restart state");
    check(gate.Finish(*first), "owner can settle a completed command");
    const auto second = gate.Begin({.action = CoreAction::Start});
    check(!gate.Finish(*first) && gate.Busy(), "old completion cannot clear a newer operation (ABA)");
    gate.Finish(*second);
    check(!gate.AcceptsPoll(oldPoll), "old snapshot is rejected even when both commands return to idle");
    const auto current = gate.revision.Get();
    check(gate.AcceptsPoll(current), "current external snapshot can publish while idle");
    gate.Invalidate();
    check(!gate.AcceptsPoll(current), "synchronous configuration edit invalidates an in-flight read");

}
void InstallControlFixture(huxerui::ApplicationContext& context) {
    CheckCoreOperationsGate();
    context.Provide(std::make_shared<ControlFixture>());
}
[[huxerui::composable]] huxerui::View ControlOrigin() {
    using namespace clashflux::ui;
    const auto model = huxerui::UseService<ControlFixture>();
    const auto tasks = huxerui::UseApplicationTaskScope();
    const auto run = [model, tasks](bool fail) {
        const bool launched = LaunchCoreOperation<bool>(tasks, model,
            {.action = CoreAction::Mode, .mode = "global"},
            [fail](CoreCommand) -> huxerui::Task<bool> {
                co_await huxerui::Delay(std::chrono::milliseconds(200));
                if (fail) throw std::runtime_error("control fixture");
                co_return true;
            }, [model](std::optional<bool> result, std::exception_ptr error) {
                check(!model->operations.Busy(), "production launcher clears pending before result feedback");
                model->result = std::string{result && !error ? "CONTROL-SUCCESS" : "CONTROL-FAILED"};
            });
        if (launched) model->executions = model->executions.Get() + 1;
    };
    const auto installThenRun = [model, tasks](int outcome, CoreAction action) {
        LaunchCoreOperation<bool>(tasks, model, {.action = action, .enabled = true},
            [model, outcome](CoreCommand original) -> huxerui::Task<bool> {
                const auto resumed = co_await ResumeCoreCommandAfterServiceInstall<bool>(
                    original, [model, outcome]() -> huxerui::Task<bool> {
                        co_await huxerui::Delay(std::chrono::milliseconds(200));
                        check(model->operations.Busy(), "installation retains the original operation ticket");
                        if (outcome == 2) throw std::runtime_error("install fixture");
                        co_return outcome != 1;
                    }, [model, original, outcome](CoreCommand command) -> huxerui::Task<bool> {
                        check(command == original && model->operations.command.Get() == original,
                              "installation resumes the unchanged command while shared pending is held");
                        model->resumedCommands = model->resumedCommands.Get() + 1;
                        if (outcome == 3) co_return false;
                        model->tunIntent = command.action == CoreAction::Tun && command.enabled;
                        co_return true;
                    });
                co_return resumed.value_or(false);
            }, [model](std::optional<bool> result, std::exception_ptr error) {
                model->result = std::string{result.value_or(false) && !error
                    ? "CONTROL-SUCCESS" : "CONTROL-FAILED"};
            });
    };
    return huxerui::Row {
        huxerui::Button("Run control").OnClick([run] { run(false); }),
        huxerui::Button("Fail control").OnClick([run] { run(true); }),
        huxerui::Button("Install then TUN").OnClick([installThenRun] { installThenRun(0, CoreAction::Tun); }),
        huxerui::Button("Cancel install").OnClick([installThenRun] { installThenRun(1, CoreAction::Tun); }),
        huxerui::Button("Throw install").OnClick([installThenRun] { installThenRun(2, CoreAction::Tun); }),
        huxerui::Button("Fail resumed TUN").OnClick([installThenRun] { installThenRun(3, CoreAction::Tun); }),
        huxerui::Button("Install then start").OnClick([installThenRun] { installThenRun(0, CoreAction::Start); }),
    }.Key("control-origin");
}
[[huxerui::composable]] huxerui::View ControlFixtureContent() {
    using namespace clashflux::ui;
    const auto model = huxerui::UseService<ControlFixture>();
    const auto tasks = huxerui::UseApplicationTaskScope();
    auto retired = huxerui::UseState(false);
    return huxerui::Column {
        retired.Get() ? huxerui::View{huxerui::Text("ORIGIN-RETIRED")} : ControlOrigin(),
        huxerui::Button("Retire control origin").OnClick([retired] { retired = true; }),
        huxerui::Button("Restore control origin").OnClick([retired] { retired = false; }),
        huxerui::Button("Try second control entry").OnClick([model] {
            auto ticket = model->operations.Begin({.action = CoreAction::Tun, .enabled = true});
            if (ticket) {
                model->executions = model->executions.Get() + 1;
                model->operations.Finish(*ticket);
            }
        }),
        huxerui::Button("Start control poll").OnClick([model, tasks] {
            const auto before = model->operations.revision.Get();
            tasks.Launch([model, before]() -> huxerui::Task<void> {
                co_await huxerui::Delay(std::chrono::milliseconds(400));
                model->poll = std::string{model->operations.AcceptsPoll(before) ? "CONTROL-POLL-ACCEPTED" : "CONTROL-POLL-REJECTED"};
            });
        }),
        huxerui::Text(model->operations.Busy() ? "CONTROL-BUSY" : "CONTROL-READY"),
        huxerui::Text(model->result.Get()),
        huxerui::Text(model->poll.Get()),
        huxerui::Text("CONTROL-EXECUTIONS:" + std::to_string(model->executions.Get())),
        huxerui::Text("RESUMED-COMMANDS:" + std::to_string(model->resumedCommands.Get())),
        huxerui::Text(model->tunIntent.Get() ? "TUN-INTENT-ON" : "TUN-INTENT-OFF"),
    }.With(huxerui::Grow(1.0F));
}
huxerui::View ControlFixtureRoot() { return huxerui::FlatTheme{ControlFixtureContent()}; }
const huxerui::Application kControlApplication{ControlFixtureRoot,
    {.show_debug_overlay = false, .application_hooks = {InstallControlFixture}}};

void testCoreOperations() {
    using namespace clashflux::ui;
    huxerui::testing::UiTest ui(kControlApplication, {.viewport = {700.0F, 600.0F}});
    ui.PumpAndSettle();
    ui.Find(UiSelector::Text("Start control poll")).Tap(); ui.Pump();
    ui.Find(UiSelector::Text("Run control")).Tap(); ui.Pump();
    ui.Find(UiSelector::Text("Try second control entry")).Tap(); ui.Pump();
    check(ui.Find(UiSelector::Text("CONTROL-BUSY")).Exists() &&
          ui.Find(UiSelector::Text("CONTROL-EXECUTIONS:1")).Exists(), "second entry sees pending and cannot launch conflicting work");
    ui.Find(UiSelector::Text("Retire control origin")).Tap(); ui.Pump();
    check(ui.Find(UiSelector::Text("ORIGIN-RETIRED")).Exists(), "control origin really unmounts during application work");
    ui.SetWindowMetrics({.viewport = {390.0F, 700.0F}});
    ui.Pump(std::chrono::milliseconds(500)); ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("CONTROL-SUCCESS")).Exists() &&
          ui.Find(UiSelector::Text("CONTROL-READY")).Exists() &&
          ui.Find(UiSelector::Text("CONTROL-POLL-REJECTED")).Exists(), "application task settles after origin retirement/resize and rejects stale poll");
    ui.Find(UiSelector::Text("Restore control origin")).Tap(); ui.PumpAndSettle();
    ui.Find(UiSelector::Text("Fail control")).Tap(); ui.Pump(std::chrono::milliseconds(300)); ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("CONTROL-FAILED")).Exists() && ui.Find(UiSelector::Text("CONTROL-READY")).Exists(), "failed work releases shared pending");
    ui.Find(UiSelector::Text("Run control")).Tap(); ui.Pump(std::chrono::milliseconds(300)); ui.PumpAndSettle();
    check(ui.Find(UiSelector::Text("CONTROL-SUCCESS")).Exists() && ui.Find(UiSelector::Text("CONTROL-EXECUTIONS:3")).Exists(), "failed operation remains retryable");
}

void testServiceInstallContinuation() {
    for (const auto* button : {"Install then TUN", "Cancel install", "Throw install",
                               "Fail resumed TUN", "Install then start"}) {
        huxerui::testing::UiTest ui(kControlApplication, {.viewport = {1800.0F, 700.0F}});
        ui.PumpAndSettle();
        ui.Find(UiSelector::Text("Start control poll")).Tap(); ui.Pump();
        ui.Find(UiSelector::Text(button)).Tap(); ui.Pump();
        ui.Find(UiSelector::Text("Try second control entry")).Tap(); ui.Pump();
        check(ui.Find(UiSelector::Text("CONTROL-BUSY")).Exists() &&
              ui.Find(UiSelector::Text("CONTROL-EXECUTIONS:0")).Exists(),
              "service installation prevents conflicting commands throughout authorization");
        ui.Find(UiSelector::Text("Retire control origin")).Tap(); ui.Pump();
        check(ui.Find(UiSelector::Text("ORIGIN-RETIRED")).Exists(),
              "service-install caller actually unmounts before installation completes");
        ui.SetWindowMetrics({.viewport = {390.0F, 700.0F}});
        ui.Pump(std::chrono::milliseconds(500)); ui.PumpAndSettle();
        const std::string_view scenario = button;
        const bool success = scenario == "Install then TUN" || scenario == "Install then start";
        const bool resumed = success || scenario == "Fail resumed TUN";
        check(ui.Find(UiSelector::Text(success ? "CONTROL-SUCCESS" : "CONTROL-FAILED")).Exists() &&
              ui.Find(UiSelector::Text("CONTROL-READY")).Exists() &&
              ui.Find(UiSelector::Text("CONTROL-POLL-REJECTED")).Exists(),
              "install completion settles after retirement/resize and rejects stale snapshots");
        check(ui.Find(UiSelector::Text(resumed ? "RESUMED-COMMANDS:1" : "RESUMED-COMMANDS:0")).Exists(),
              "only confirmed installation retries, exactly once, even when retry fails");
        check(ui.Find(UiSelector::Text(scenario == "Install then TUN" ? "TUN-INTENT-ON" : "TUN-INTENT-OFF")).Exists(),
              "successful resumed TUN preserves intent; cancellation/failure does not enable it");
        if (!success) {
            ui.Find(UiSelector::Text("Restore control origin")).Tap(); ui.PumpAndSettle();
            ui.Find(UiSelector::Text("Install then TUN")).Tap();
            ui.Pump(std::chrono::milliseconds(300)); ui.PumpAndSettle();
            check(ui.Find(UiSelector::Text("CONTROL-SUCCESS")).Exists() &&
                  ui.Find(UiSelector::Text("TUN-INTENT-ON")).Exists(),
                  "canceled, exceptional, or failed installation remains retryable");
        }
    }
}

struct HeaderControlFixture {
    huxerui::State<clashflux::ui::HomeHeaderSnapshot> snapshot{
        clashflux::ui::HomeHeaderSnapshot{.ready = true, .active = true, .canStart = true}};
    huxerui::State<bool> reduced{false};
    huxerui::State<bool> dark{false};
    huxerui::State<bool> desktop{false};
    std::optional<clashflux::ui::HomeHeaderControl> control;
    int toggles = 0;
};
std::shared_ptr<HeaderControlFixture> headerFixture;
bool headerColdStart = false;

void InstallHeaderControlFixture(huxerui::ApplicationContext& context) {
    headerFixture = std::make_shared<HeaderControlFixture>();
    if (headerColdStart) headerFixture->snapshot.Update([](clashflux::ui::HomeHeaderSnapshot& value) {
        value.ready = false;
    });
    context.Provide(headerFixture);
}

[[huxerui::composable]] huxerui::View HeaderControlFixtureContent() {
    using namespace clashflux::ui;
    const auto model = huxerui::UseService<HeaderControlFixture>();
    const auto theme = huxerui::UseTheme();
    auto snapshot = model->snapshot.Get();
    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    snapshot.active = snapshot.active && (!model->desktop.Get() || compact);
    const auto control = UseHomeHeaderControl(snapshot);
    const auto scroll = huxerui::UseScrollController();
    model->control = control;
    if (!snapshot.active) return huxerui::Text("Header inactive");
    const auto toggle = [model] {
        ++model->toggles;
        model->snapshot.Update([](HomeHeaderSnapshot& value) {
            value.busy = true;
            value.phase = value.running ? BoxCatPhase::Stopping : BoxCatPhase::Starting;
        });
    };
    std::vector<huxerui::View> rows;
    for (int index = 0; index < 25; ++index)
        rows.push_back(huxerui::Text("Header content " + std::to_string(index))
            .With(huxerui::Frame{.height = 80.0F}).Key("header-row-" + std::to_string(index)));
    const auto label = Localized(model->desktop.Get() ? "停止内核" : "停止 VPN");
    huxerui::View page = huxerui::Column {
      HomeHeaderTitle(control, toggle, label),
      huxerui::ScrollView(huxerui::Column(rows)).Controller(scroll)
          .With(huxerui::Grow(1.0F)).Key("header-scroll"),
    }.With(huxerui::Spacing(8.0F), huxerui::Padding(8.0F), huxerui::ClipChildren(),
           huxerui::Grow(1.0F), huxerui::Background(theme.colors.surface_container_low),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    return HomeHeaderFloatingAction(page, control, toggle, label, 80.0F);
}

[[huxerui::composable]] huxerui::View HeaderControlFixtureTheme() {
    const auto model = huxerui::UseService<HeaderControlFixture>();
    auto theme = model->dark.Get() ? huxerui::FlatDarkThemeSpec() : huxerui::FlatLightThemeSpec();
    theme.motion.reduced_motion = model->reduced.Get();
    return huxerui::FlatTheme(theme, HeaderControlFixtureContent());
}
huxerui::View HeaderControlFixtureRoot() { return HeaderControlFixtureTheme(); }
const huxerui::Application kHeaderControlApplication{HeaderControlFixtureRoot,
    {.show_debug_overlay = false, .application_hooks = {InstallHeaderControlFixture}}};

void testHomeHeaderControl() {
    using namespace clashflux::ui;
    huxerui::testing::UiTest ui(kHeaderControlApplication,
        {.viewport = {400.0F, 800.0F}, .resource_provider = AppResources()});
    ui.UpdateWindowLifecycleState(huxerui::WindowLifecycleState::Active);
    const auto model = headerFixture;
    const auto advance = [&ui](int ms) {
        for (int elapsed = 0; elapsed < ms; elapsed += 50) ui.Pump(std::chrono::milliseconds(50));
    };
    advance(100);
    const auto slot = ui.Find(UiSelector::Key("home-header-slot")).One().bounds;
    const auto viewport = ui.Find(UiSelector::Key("header-scroll")).One().bounds;
    const auto dock = ui.Find(UiSelector::Key("home-header-floating-cat")).One().bounds;
    check(slot.height == 48.0F && viewport.height > 600.0F &&
          ui.Find(UiSelector::Key("page-title")).Exists(), "idle header reserves 48pt without swallowing the content");
    ui.Find(UiSelector::Key("header-scroll")).ScrollBy({0.0F, 160.0F});
    ui.Pump();
    const float contentY = ui.Find(UiSelector::Key("header-row-5")).One().bounds.y;
    ui.Find(UiSelector::Key("home-header-floating-cat")).Tap();
    advance(1500);
    check(model->toggles == 1 && !model->control->shown.Get() &&
          ui.Find(UiSelector::Key("page-title")).Exists(), "authorization/slow start keeps the title and floating cat");
    model->snapshot.Update([](HomeHeaderSnapshot& value) {
        value.running = true; value.busy = false; value.phase = BoxCatPhase::Running; value.elapsedSeconds = 2;
    });
    advance(250);
    check(!model->control->shown.Get(), "real success waits for the completed cat rise before replacing the title");
    advance(2400);
    check(model->control->shown.Get() && !ui.Find(UiSelector::Key("home-header-floating-cat")).Exists() &&
          ui.Find(UiSelector::Key("home-header-uptime")).Exists(), "successful cat merges into the production running header");
    const auto fullHeaderCard = paintedHeaderCard(ui);
    check(fullHeaderCard && fullHeaderCard->bounds.width == slot.width &&
          fullHeaderCard->bounds.height == slot.height &&
          fullHeaderCard->outlineWidth == 0.0F &&
          fullHeaderCard->cornerRadius == ResolveIslandTheme(huxerui::FlatLightThemeSpec()).nested_radius,
          "compact running card shares the borderless surface and radius of other home cards");
    check(ui.Find(UiSelector::Key("home-header-slot")).One().bounds == slot &&
          ui.Find(UiSelector::Key("header-scroll")).One().bounds == viewport &&
          std::abs(ui.Find(UiSelector::Key("header-row-5")).One().bounds.y - contentY) < 0.5F,
          "title replacement preserves header geometry, content viewport and scroll offset");
    check(ui.Find(UiSelector::Text("00:00:02")).Exists(), "VPN header displays the backend session elapsed time");
    model->snapshot.Update([](HomeHeaderSnapshot& value) { value.elapsedSeconds = 754; });
    advance(100);
    check(ui.Find(UiSelector::Text("00:12:34")).Exists(), "session time refreshes without replaying the merge");
    ui.Find(UiSelector::Key("home-header-running")).Tap();
    advance(150);
    check(model->toggles == 2 && model->control->motion.Get().collapsing,
          "header stop begins the in-place shrink before returning to the dock");
    const auto shrinkingHeaderCard = paintedHeaderCard(ui);
    check(fullHeaderCard && shrinkingHeaderCard &&
          shrinkingHeaderCard->bounds.width < fullHeaderCard->bounds.width &&
          shrinkingHeaderCard->bounds.height < fullHeaderCard->bounds.height &&
          std::abs(shrinkingHeaderCard->bounds.x + shrinkingHeaderCard->bounds.width / 2.0F -
                   fullHeaderCard->bounds.x - fullHeaderCard->bounds.width / 2.0F) < 0.5F &&
          std::abs(shrinkingHeaderCard->bounds.y + shrinkingHeaderCard->bounds.height / 2.0F -
                   fullHeaderCard->bounds.y - fullHeaderCard->bounds.height / 2.0F) < 0.5F,
          "the borderless compact card stays painted while shrinking at its center");
    model->snapshot.Update([](HomeHeaderSnapshot& value) {
        value.running = false; value.busy = false; value.phase = BoxCatPhase::Sleeping; value.elapsedSeconds = 0;
    });
    for (int elapsed = 0; elapsed < 1000 && model->control->shown.Get(); elapsed += 50) advance(50);
    advance(100);
    const auto returningCats = paintedHomeCats(ui);
    check(std::any_of(returningCats.begin(), returningCats.end(), [&](const auto& cat) {
        const auto center = cat.y + cat.height / 2.0F;
        return center > slot.y + slot.height && center < dock.y;
    }), "compact return paints the cat between the header and dock instead of jumping to its destination");
    check(!returningCats.empty() && std::all_of(returningCats.begin(), returningCats.end(), [](const auto& cat) {
        return std::abs(cat.width - 48.0F) < 0.1F && std::abs(cat.height - 48.0F) < 0.1F;
    }), "compact return keeps the same square 48pt artwork throughout the shared crossfade");
    advance(100);
    const auto laterReturningCats = paintedHomeCats(ui);
    check(!returningCats.empty() && !laterReturningCats.empty() &&
          laterReturningCats.back().y > returningCats.back().y &&
          std::all_of(laterReturningCats.begin(), laterReturningCats.end(), [](const auto& cat) {
              return std::abs(cat.width - 48.0F) < 0.1F && std::abs(cat.height - 48.0F) < 0.1F;
          }), "successive compact return frames travel toward the dock without stretching or replaying the shrink");
    advance(1400);
    check(!model->control->shown.Get() && ui.Find(UiSelector::Key("page-title")).Exists() &&
          ui.Find(UiSelector::Key("home-header-floating-cat")).Exists(), "fast stop restores the title and dock without stale completion");

    // Failed/cancelled start, retry, and the immediate-success path.
    ui.Find(UiSelector::Key("home-header-floating-cat")).Tap(); advance(100);
    model->snapshot.Update([](HomeHeaderSnapshot& value) { value.busy = false; value.phase = BoxCatPhase::Failed; });
    advance(2200);
    check(!model->control->shown.Get(), "failed startup never creates a running header");
    ui.Find(UiSelector::Key("home-header-floating-cat")).Tap(); advance(100);
    model->snapshot.Update([](HomeHeaderSnapshot& value) { value.busy = false; value.phase = BoxCatPhase::Sleeping; });
    advance(1800);
    check(!model->control->shown.Get(), "cancelled consent never creates a running header");
    model->snapshot.Update([](HomeHeaderSnapshot& value) { value.running = true; value.phase = BoxCatPhase::Running; });
    advance(100);
    check(!model->control->shown.Get(), "instant success still completes the cat animation");
    advance(2800);
    ui.Find(UiSelector::Key("home-header-running")).Tap(); advance(800);
    model->snapshot.Update([](HomeHeaderSnapshot& value) {
        value.busy = false; value.phase = BoxCatPhase::Running; value.elapsedSeconds = 755;
    });
    advance(2800);
    check(model->control->shown.Get() && ui.Find(UiSelector::Text("00:12:35")).Exists(),
          "a rejected stop restores the running header and original session duration");

    // Off-page synchronization and compact/expanded changes skip re-entry motion.
    model->snapshot.Update([](HomeHeaderSnapshot& value) { value.active = false; }); advance(100);
    model->snapshot.Update([](HomeHeaderSnapshot& value) { value.elapsedSeconds = 10000; }); advance(100);
    model->snapshot.Update([](HomeHeaderSnapshot& value) { value.active = true; }); advance(100);
    check(model->control->shown.Get() && model->control->motion.Get().action == CoreAction::Idle &&
          ui.Find(UiSelector::Text("02:46:40")).Exists(), "returning to an already running session skips startup and preserves uptime");
    model->desktop = true;
    ui.SetWindowMetrics({.viewport = {1000.0F, 700.0F}}); advance(100);
    ui.SetWindowMetrics({.viewport = {400.0F, 800.0F}}); advance(100);
    check(model->control->shown.Get() && model->control->motion.Get().action == CoreAction::Idle,
          "desktop compact header synchronizes directly after a wide/narrow round trip");
    model->desktop = false;
    for (const auto size : {huxerui::Size{320.0F, 700.0F}, huxerui::Size{900.0F, 420.0F}}) {
        ui.SetWindowMetrics({.viewport = size}); advance(100);
        const auto header = ui.Find(UiSelector::Key("home-header-slot")).One().bounds;
        const auto badge = ui.Find(UiSelector::Key("box-cat-artwork")).One().bounds;
        check(header.height == 48.0F && badge.height == 36.0F &&
              badge.y >= header.y && badge.y + badge.height <= header.y + header.height,
              "mobile narrow/wide headers retain the 48pt slot and unclipped centered cat");
    }
    model->dark = true; model->reduced = true; advance(100);
    model->snapshot.Update([](HomeHeaderSnapshot& value) { value.running = false; value.phase = BoxCatPhase::Sleeping; });
    advance(100);
    check(!model->control->shown.Get() && model->control->motion.Get().action == CoreAction::Idle,
          "reduced motion stops immediately in the dark theme");
    model->snapshot.Update([](HomeHeaderSnapshot& value) { value.running = true; value.phase = BoxCatPhase::Running; });
    advance(100);
    check(model->control->shown.Get() && model->control->motion.Get().action == CoreAction::Idle,
          "reduced motion starts immediately without a pending completion");
    model->reduced = false;
    model->snapshot.Update([](HomeHeaderSnapshot& value) {
        value.running = false; value.phase = BoxCatPhase::Sleeping;
    });
    advance(1800);
    model->snapshot.Update([](HomeHeaderSnapshot& value) {
        value.running = true; value.phase = BoxCatPhase::Running;
    });
    for (int elapsed = 0; elapsed < 3500 && model->control->motion.Get().action != CoreAction::Start; elapsed += 50)
        advance(50);
    check(model->control->motion.Get().action == CoreAction::Start,
          "retry after a reduced-motion switch can start a fresh merge");
    model->snapshot.Update([](HomeHeaderSnapshot& value) {
        value.running = false; value.phase = BoxCatPhase::Sleeping;
    });
    advance(2200);
    check(!model->control->shown.Get() && model->control->motion.Get().action == CoreAction::Idle,
          "disconnecting during the merge settles back to the title without a stale running header");
    model->snapshot.Update([](HomeHeaderSnapshot& value) { value.active = false; });
    advance(100);
    model->snapshot.Update([](HomeHeaderSnapshot& value) {
        value.running = true; value.phase = BoxCatPhase::Running; value.elapsedSeconds = 3601;
    });
    advance(100);
    model->snapshot.Update([](HomeHeaderSnapshot& value) { value.active = true; });
    advance(100);
    check(model->control->shown.Get() && model->control->motion.Get().action == CoreAction::Idle &&
          ui.Find(UiSelector::Text("01:00:01")).Exists(),
          "a connection completed off-page restores directly with its existing duration");
    check(FormatHomeUptime(-1) == "00:00:00" && FormatHomeUptime(360000) == "100:00:00",
          "shared desktop/mobile format clamps negative time and keeps hours above 99");
}

void testHomeHeaderColdStart() {
    using namespace clashflux::ui;
    headerColdStart = true;
    huxerui::testing::UiTest ui(kHeaderControlApplication,
        {.viewport = {400.0F, 800.0F}, .resource_provider = AppResources()});
    ui.UpdateWindowLifecycleState(huxerui::WindowLifecycleState::Active);
    ui.Pump();
    const auto model = headerFixture;
    check(!model->control->shown.Get() &&
          !ui.Find(UiSelector::Key("home-header-floating-cat")).One().enabled,
          "unobserved backend defaults cannot enable a control or create a running header");
    model->snapshot.Update([](HomeHeaderSnapshot& value) {
        value.ready = true; value.running = true; value.phase = BoxCatPhase::Running;
        value.elapsedSeconds = 754;
    });
    ui.Pump(); ui.Pump();
    check(model->control->shown.Get() && model->control->motion.Get().action == CoreAction::Idle &&
          ui.Find(UiSelector::Text("00:12:34")).Exists(),
          "first confirmed running VPN snapshot restores its header and session time without replay");
    model->snapshot.Update([](HomeHeaderSnapshot& value) {
        value.elapsedSeconds = 0;
        value.runningSince = std::chrono::steady_clock::now() - std::chrono::seconds{754};
    });
    ui.Pump(); ui.Pump();
    check(ui.Find(UiSelector::Text("00:12:34")).Exists(),
          "desktop compact uses the real core-ready time instead of the VPN elapsed field");
    ui.UpdateWindowLifecycleState(huxerui::WindowLifecycleState::Background);
    ui.Pump(std::chrono::milliseconds(100));
    ui.UpdateWindowLifecycleState(huxerui::WindowLifecycleState::Active);
    ui.Pump(); ui.Pump();
    check(model->control->motion.Get().action == CoreAction::Idle && model->control->shown.Get(),
          "foreground restoration does not replay the startup animation");
    headerColdStart = false;
}

void testInboundSettings() {
    using namespace clashflux::ui;
    const auto ports = ParseInboundPorts(" 07899\n", "", " 1080 ");
    check(ports.error == PortSettingsError::None && ports.ports.mixed == "7899" &&
          ports.ports.http.empty() && ports.ports.socks == "1080", "shared port validation trims and normalizes optional listeners");
    check(ParseInboundPorts("65535", "1", "").error == PortSettingsError::None, "port boundaries remain accepted");
    for (const auto* invalid : {"", "0", "65536", "7899x", "-1", "999999999999999"})
        check(ParseInboundPorts(invalid, "", "").error == PortSettingsError::Invalid, "invalid mixed port rejected before writing settings");
    check(ParseInboundPorts("7899", "7899", "").error == PortSettingsError::Duplicate &&
          ParseInboundPorts("7899", "8080", "8080").error == PortSettingsError::Duplicate,
          "shared validation rejects collisions among all listener types");
}
int homeCardActionCount = 0;
huxerui::View HomeCardPressRoot(bool dark, bool reduced) {
    auto theme = dark ? huxerui::FlatDarkThemeSpec() : huxerui::FlatLightThemeSpec();
    theme.colors = dark ? clashflux::ui::FluxDarkColors() : clashflux::ui::FluxLightColors();
    theme.motion.reduced_motion = reduced;
    return huxerui::MaterialTheme(theme, huxerui::Column{
        clashflux::ui::HomeCardSurface(theme, huxerui::Column{
            huxerui::Text("Card body").With(huxerui::Grow(1.0F)),
            huxerui::Button("Nested card action").OnClick([] { ++homeCardActionCount; }).Key("nested-card-action"),
        }.With(huxerui::Grow(1.0F)), "press").With(huxerui::Frame{.width = 280.0F, .height = 188.0F}).Key("press-card"),
        huxerui::Spacer().With(huxerui::Frame{.height = 100.0F}),
    });
}
huxerui::View HomeCardPressLightRoot() { return HomeCardPressRoot(false, false); }
huxerui::View HomeCardPressDarkRoot() { return HomeCardPressRoot(true, false); }
huxerui::View HomeCardPressReducedRoot() { return HomeCardPressRoot(false, true); }
const huxerui::Application kHomeCardPressLight{HomeCardPressLightRoot, {.show_debug_overlay = false}};
const huxerui::Application kHomeCardPressDark{HomeCardPressDarkRoot, {.show_debug_overlay = false}};
const huxerui::Application kHomeCardPressReduced{HomeCardPressReducedRoot, {.show_debug_overlay = false}};

float paintedHomeCardScale(huxerui::testing::UiTest& ui) {
    std::istringstream input(ui.CaptureSnapshot().ToString());
    std::string line;
    std::getline(input, line); std::getline(input, line);
    const auto snapshot = readCatPaintNode(input, line);
    float result = 1.0F;
    const auto visit = [&](auto&& self, const CatPaintNode& node, float opacity) -> void {
        opacity *= node.Number("opacity", 1.0F);
        if (opacity < 0.01F) return;
        if (const auto* transform = node.Find("transform"))
            result = std::min(result, transform->Number("m11", 1.0F));
        if (const auto* children = node.Find("children"))
            for (const auto& child : children->children) self(self, child, opacity);
    };
    if (const auto* render = snapshot.Find("render"))
        for (const auto& node : render->children) visit(visit, node, 1.0F);
    return result;
}

void testHomeCardPress() {
    for (const auto* app : {&kHomeCardPressLight, &kHomeCardPressDark, &kHomeCardPressReduced}) {
        const bool reduced = app == &kHomeCardPressReduced;
        huxerui::testing::UiTest ui(*app, {.viewport = {400.0F, 400.0F}, .resource_provider = AppResources()});
        ui.PumpAndSettle();
        const auto card = ui.Find(UiSelector::Key("press-card")).One();
        huxerui::PointerEvent event{.type = huxerui::PointerEventType::Down, .pointer_id = 84,
            .position = {card.bounds.x + 8.0F, card.bounds.y + 8.0F}, .device_kind = huxerui::PointerDeviceKind::Mouse,
            .changed_button = huxerui::PointerButton::Primary, .pressed_buttons = huxerui::PointerButton::Primary};
        ui.SendPointer(event); ui.Pump(); ui.Pump(std::chrono::milliseconds(100));
        check(reduced ? paintedHomeCardScale(ui) == 1.0F : paintedHomeCardScale(ui) < 0.999F,
              "home card paints a press transform in both themes and honors reduced motion");
        check(ui.Find(UiSelector::Key("press-card")).One().size == card.size,
              "home card press animation does not change grid layout size");
        event.type = huxerui::PointerEventType::Cancel;
        event.pressed_buttons = huxerui::PointerButton::None;
        ui.SendPointer(event); ui.PumpAndSettle();
        check(std::abs(paintedHomeCardScale(ui) - 1.0F) < 0.0001F, "cancel restores the card's painted scale");

        const auto button = ui.Find(UiSelector::Key("nested-card-action")).One().bounds;
        event.type = huxerui::PointerEventType::Down;
        event.device_kind = huxerui::PointerDeviceKind::Touch;
        event.position = {button.x + button.width / 2.0F, button.y + button.height / 2.0F};
        event.pressed_buttons = huxerui::PointerButton::Primary;
        const int previous = homeCardActionCount;
        ui.SendPointer(event); ui.Pump(); ui.Pump(std::chrono::milliseconds(100));
        check(reduced || paintedHomeCardScale(ui) < 0.999F, "pressing a nested control also animates its card");
        event.type = huxerui::PointerEventType::Up;
        event.pressed_buttons = huxerui::PointerButton::None;
        ui.SendPointer(event); ui.PumpAndSettle();
        check(homeCardActionCount == previous + 1, "card feedback preserves the nested button's single click");
        check(std::abs(paintedHomeCardScale(ui) - 1.0F) < 0.0001F, "release restores the card's painted scale");
    }
}

int homeCardReveals = 0;
std::string homeEasterEggOpenedUrl;

huxerui::View HomeCardFlipRoot(bool dark, bool reduced) {
    auto theme = dark ? huxerui::FlatDarkThemeSpec() : huxerui::FlatLightThemeSpec();
    theme.colors = dark ? clashflux::ui::FluxDarkColors() : clashflux::ui::FluxLightColors();
    theme.motion.reduced_motion = reduced;
    return huxerui::MaterialTheme(theme, huxerui::Scope([theme] {
        auto backCount = huxerui::UseState(0);
        const auto content = huxerui::Scope([] {
            auto count = huxerui::UseState(0);
            return huxerui::Column {
              huxerui::Text("Flip front").With(huxerui::Grow(1.0F)),
              huxerui::Button("Counter-" + std::to_string(count.Get()))
                  .With(huxerui::Enabled(true))
                  .OnClick([count] { count = count.Get() + 1; }).Key("flip-counter"),
            }.With(huxerui::Grow(1.0F));
        });
        return huxerui::Column {
          clashflux::ui::HomeCardSurface(theme, content, "first", true,
              huxerui::Column {
                huxerui::Text("Flip back menu"),
                huxerui::Button("Back action-" + std::to_string(backCount.Get()))
                    .OnClick([backCount] { backCount = backCount.Get() + 1; })
                    .Key("flip-back-action"),
              }, [] { ++homeCardReveals; })
              .With(huxerui::Frame{.width = 280.0F, .height = 188.0F}).Key("flip-card-one"),
          clashflux::ui::HomeCardSurface(theme, huxerui::Text("Second front"), "second", true,
              clashflux::ui::HomeEasterEggContent(
                  clashflux::ui::kHomeEasterEggPool[clashflux::ui::PickHomeEasterEgg()],
                  [](std::string_view url) { homeEasterEggOpenedUrl = url; }))
              .With(huxerui::Frame{.width = 280.0F, .height = 188.0F}).Key("flip-card-two"),
          clashflux::ui::HomeCardSurface(theme, huxerui::Text("Cat control excluded"), "control", false)
              .With(huxerui::Frame{.width = 280.0F, .height = 88.0F}).Key("flip-card-excluded"),
        }.With(huxerui::Spacing(12.0F));
    }));
}
huxerui::View HomeCardFlipLightRoot() { return HomeCardFlipRoot(false, false); }
huxerui::View HomeCardFlipDarkRoot() { return HomeCardFlipRoot(true, false); }
huxerui::View HomeCardFlipReducedRoot() { return HomeCardFlipRoot(false, true); }
const huxerui::Application kHomeCardFlipLight{HomeCardFlipLightRoot, {.show_debug_overlay = false}};
const huxerui::Application kHomeCardFlipDark{HomeCardFlipDarkRoot, {.show_debug_overlay = false}};
const huxerui::Application kHomeCardFlipReduced{HomeCardFlipReducedRoot, {.show_debug_overlay = false}};

struct ModeChoiceFixture {
    huxerui::State<std::size_t> selected{0U};
    huxerui::State<bool> enabled{true};
    int changes = 0;
};
std::shared_ptr<ModeChoiceFixture> modeChoiceFixture;
bool modeChoiceDark = false;
std::string modeChoiceAccent = "blue";
bool modeThemeMatches = false;
huxerui::View ModeChoiceRoot() {
    using namespace clashflux::ui;
    auto theme = modeChoiceDark ? huxerui::MaterialDarkThemeSpec() : huxerui::MaterialLightThemeSpec();
    theme.colors = modeChoiceDark ? FluxDarkColors(modeChoiceAccent) : FluxLightColors(modeChoiceAccent);
    ApplyFluxInteractionTheme(theme);
    return huxerui::MaterialTheme(theme, FluxInteractionStyles(huxerui::Scope([theme] {
        auto model = huxerui::UseState(std::make_shared<ModeChoiceFixture>()).Get();
        modeChoiceFixture = model;
        const auto radio = huxerui::UseEnvironment<huxerui::RadioButtonStyle>();
        const auto toggle = huxerui::UseEnvironment<huxerui::SwitchStyle>();
        const auto icons = huxerui::UseEnvironment<huxerui::IconButtonStyle>();
        const auto chips = huxerui::UseEnvironment<huxerui::ChipStyle>();
        const auto tabs = huxerui::UseEnvironment<huxerui::TabsStyle>();
        const auto matches = [theme](const auto& indication) {
            return indication && indication->ripple &&
                indication->ripple->color == FluxStateLayerColor(theme.colors.primary, kInteractionRippleOpacity);
        };
        modeThemeMatches = radio.selected_color == theme.colors.primary &&
            toggle.checked_track == theme.colors.primary && matches(icons.indication) &&
            matches(chips.indication) && matches(tabs.indication) &&
            huxerui::UseTheme().interactions.indication == ResolveFluxIndication(theme);
        return huxerui::Column {
          huxerui::Stack {HomeModeChoices({"Rule", "Global", "Direct"}, model->selected.Get(), model->enabled.Get(),
            [model](std::size_t index) { model->selected = index; ++model->changes; })
          }.With(huxerui::Frame{.width = 280.0F, .height = 132.0F},
              huxerui::Align(huxerui::HorizontalAlignment::Stretch, huxerui::VerticalAlignment::Stretch))
              .Key("mode-options"),
          huxerui::RadioButton("Language baseline", true),
        };
    })));
}
const huxerui::Application kModeChoiceApplication{ModeChoiceRoot, {.show_debug_overlay = false, .window_hooks = {clashflux::ui::InstallToastLayers}}};

void testModeChoiceFeedback() {
    using namespace clashflux::ui;
    for (bool dark : {false, true}) for (const char* accent : {"blue", "green"}) {
        modeChoiceDark = dark; modeChoiceAccent = accent;
        for (auto device : {huxerui::PointerDeviceKind::Mouse, huxerui::PointerDeviceKind::Touch}) {
            huxerui::testing::UiTest ui(kModeChoiceApplication, {.viewport = {320.0F, 300.0F}});
            ui.PumpAndSettle();
            check(modeThemeMatches, "Material selection colors and owning feedback follow the current accent in both themes");
            const auto bounds = ui.Find(UiSelector::Key("home-mode-1")).One().bounds;
            huxerui::PointerEvent event{.type = huxerui::PointerEventType::Down, .pointer_id = 91,
                .position = {bounds.x + bounds.width - 4.0F, bounds.y + bounds.height * 0.5F},
                .device_kind = device, .changed_button = huxerui::PointerButton::Primary,
                .pressed_buttons = huxerui::PointerButton::Primary};
            ui.SendPointer(event); ui.Pump(); ui.Pump(std::chrono::milliseconds(100));
            std::istringstream input(ui.CaptureSnapshot().ToString());
            std::string line; std::getline(input, line); std::getline(input, line);
            const auto snapshot = readCatPaintNode(input, line);
            bool circle = false, wide = false;
            const auto scan = [&](auto&& self, const CatPaintNode& node) -> void {
                if (node.name == "circle" && node.Number("radius") >= 14.0F && node.Number("radius") <= 24.0F)
                    circle = true;
                if (node.name == "rect") {
                    const auto* b = node.Find("bounds");
                    const auto* brush = node.Find("brush");
                    const auto* color = brush ? brush->Find("color") : nullptr;
                    if (b && color && b->Number("width") > 70.0F && color->Number("alpha") > 0.0F &&
                        color->Number("alpha") < 0.3F) wide = true;
                }
                for (const auto& child : node.children) self(self, child);
            };
            scan(scan, snapshot);
            if (!circle || wide) std::ofstream("mode-feedback-failure.snapshot") << ui.CaptureSnapshot().ToString();
            check(circle && !wide, "clicking the radio label keeps feedback around the control rather than across the row");
            event.type = huxerui::PointerEventType::Up;
            event.pressed_buttons = huxerui::PointerButton::None;
            ui.SendPointer(event); ui.PumpAndSettle();
            check(modeChoiceFixture->selected.Get() == 1U && modeChoiceFixture->changes == 1,
                "the whole radio row remains clickable and selects once");
            modeChoiceFixture->enabled = false; ui.PumpAndSettle();
            const auto disabled = ui.Find(UiSelector::Key("home-mode-2")).One().bounds;
            ui.TapAt({disabled.x + 40.0F, disabled.y + disabled.height * 0.5F}, {.device_kind = device});
            ui.PumpAndSettle();
            check(modeChoiceFixture->selected.Get() == 1U && modeChoiceFixture->changes == 1,
                "busy mode controls reject a new selection");
        }
    }
}

bool toastFixtureDark = false, toastFixtureReduced = false;
std::optional<clashflux::ui::FluxToastHandle> savedStackToast;
std::vector<huxerui::LayerId> stackedToastIds;
std::function<void()> toastTestAction;
huxerui::View ToastMotionRoot() {
    using namespace clashflux::ui;
    auto theme = toastFixtureDark ? huxerui::MaterialDarkThemeSpec() : huxerui::MaterialLightThemeSpec();
    theme.colors = toastFixtureDark ? FluxDarkColors() : FluxLightColors();
    theme.motion.reduced_motion = toastFixtureReduced;
    ApplyFluxInteractionTheme(theme);
    return huxerui::MaterialTheme(theme, FluxInteractionStyles(huxerui::Scope([] {
        const auto toast = clashflux::ui::UseFluxToast();
        savedStackToast = toast;
        auto active = huxerui::UseState(std::optional<huxerui::LayerId>{});
        return huxerui::Column {
          huxerui::Button("Show short toast").OnClick([toast] { toast.Show("EXIT-TOAST", {0.8}); }),
          huxerui::Button("Show sticky toast").OnClick([toast, active] { active = toast.Show("EXIT-TOAST", {10.0}); }),
          huxerui::Button("Dismiss toast").OnClick([toast, active] { if (active.Get()) toast.Dismiss(*active.Get()); }),
          huxerui::Button("Show three toasts").OnClick([toast] {
              stackedToastIds.clear();
              for (int i = 1; i <= 3; ++i)
                  stackedToastIds.push_back(toast.Show("QUEUE-" + std::to_string(i), {3.0}));
          }),
          huxerui::Button("Run toast action").OnClick([] { if (toastTestAction) toastTestAction(); }),
        };
    })));
}
const huxerui::Application kToastMotionApplication{ToastMotionRoot, {.show_debug_overlay = false, .window_hooks = {clashflux::ui::InstallToastLayers}}};
struct ToastPaintSample { float scale, y, opacity; };
std::optional<ToastPaintSample> paintedToast(huxerui::testing::UiTest& ui,
    std::string_view text = "EXIT-TOAST") {
    std::istringstream input(ui.CaptureSnapshot().ToString());
    std::string line; std::getline(input, line); std::getline(input, line);
    const auto snapshot = readCatPaintNode(input, line);
    std::optional<ToastPaintSample> result;
    const auto containsText = [&](auto&& self, const CatPaintNode& node) -> bool {
        if (node.name == "text" && node.value == "\"" + std::string(text) + "\"") return true;
        return std::any_of(node.children.begin(), node.children.end(), [&](const auto& child) { return self(self, child); });
    };
    const auto visit = [&](auto&& self, const CatPaintNode& node, huxerui::Transform2D parent, float alpha) -> void {
        if (const auto* visible = node.Find("visible"); visible && visible->value == "false") return;
        auto local = catPaintTransform(node.Find("transform"));
        if (const auto* offset = node.Find("offset")) {
            local.translate_x += offset->Number("x"); local.translate_y += offset->Number("y");
        }
        const auto world = composeCatPaint(parent, local);
        alpha *= node.Number("opacity", 1.0F);
        if (const auto* content = node.Find("content"); content && containsText(containsText, *content))
            result = ToastPaintSample{world.m11, world.translate_y, alpha};
        if (const auto* children = node.Find("children"))
            for (const auto& child : children->children)
                self(self, child, composeCatPaint(world, catPaintTransform(node.Find("children_transform"))), alpha);
    };
    if (const auto* render = snapshot.Find("render"))
        for (const auto& node : render->children) visit(visit, node, huxerui::Transform2D{}, 1.0F);
    return result;
}
void testToastExitMotion() {
    for (bool dark : {false, true}) for (bool reduced : {false, true}) for (bool manual : {false, true}) {
        toastFixtureDark = dark; toastFixtureReduced = reduced;
        huxerui::testing::UiTest ui(kToastMotionApplication, {.viewport = {400.0F, 500.0F}});
        ui.PumpAndSettle();
        ui.Find(UiSelector::Text(manual ? "Show sticky toast" : "Show short toast")).Tap();
        ui.Pump();
        for (int frame = 0; frame < 16; ++frame) ui.Pump(std::chrono::milliseconds(20));
        const auto visible = paintedToast(ui);
        check(visible.has_value(), "toast remains present for its visible duration");
        if (!visible) continue;
        if (manual) { ui.Find(UiSelector::Text("Dismiss toast")).Tap(); ui.Pump(); }
        bool animatedExit = false;
        for (int frame = 0; frame < 65; ++frame) {
            ui.Pump(std::chrono::milliseconds(20));
            const auto sample = paintedToast(ui);
            if (sample && sample->opacity > 0.02F && sample->opacity < 0.99F) {
                if (reduced) check(std::abs(sample->scale - 1.0F) < 0.001F &&
                    std::abs(sample->y - visible->y) < 0.01F, "reduced motion suppresses toast slide and scale");
                else animatedExit = animatedExit || (sample->scale < visible->scale - 0.0001F &&
                    sample->y > visible->y + 0.05F);
            }
        }
        check(reduced || animatedExit, "automatic and explicit toast dismissal paint a downward, shrinking fade before removal");
        check(!ui.Find(UiSelector::Text("EXIT-TOAST")).Exists(), "toast is removed after exit completes");
        const auto settled = ui.CaptureSnapshot(); ui.Pump(std::chrono::seconds(1));
        check(ui.CaptureSnapshot() == settled, "dismissed toast has no continuing animation");
    }
}

void testToastStack() {
    for (bool dark : {false, true}) for (bool reduced : {false, true}) {
        toastFixtureDark = dark; toastFixtureReduced = reduced;
        {
            huxerui::testing::UiTest ui(kToastMotionApplication, {.viewport = {400.0F, 500.0F}});
            ui.PumpAndSettle();
            ui.Find(UiSelector::Text("Show three toasts")).Tap(); ui.Pump();
            const auto runAction = [&](std::function<void()> action) {
                toastTestAction = std::move(action);
                ui.Find(UiSelector::Text("Run toast action")).Tap(); ui.Pump();
                toastTestAction = {};
            };
            const auto advance = [&](int frames) {
                for (int frame = 0; frame < frames; ++frame) {
                    ui.Pump(std::chrono::milliseconds(20));
                    int count = 0;
                    for (int i = 1; i <= 9; ++i)
                        if (paintedToast(ui, "QUEUE-" + std::to_string(i))) ++count;
                    check(count <= 3, "toast stack never mounts more than three surfaces, including during eviction");
                }
            };
            advance(16);
            check(stackedToastIds.size() == 3 && stackedToastIds[0] != stackedToastIds[1],
                "stacked toast requests receive independent identifiers");
            const auto first = ui.Find(UiSelector::Text("QUEUE-1")).One().bounds;
            const auto second = ui.Find(UiSelector::Text("QUEUE-2")).One().bounds;
            const auto third = ui.Find(UiSelector::Text("QUEUE-3")).One().bounds;
            check(first.y + first.height < second.y && second.y + second.height < third.y,
                "three toast surfaces are vertically spaced without overlap");
            check(third.y + third.height <= 500.0F && first.x >= 0.0F && third.x + third.width <= 400.0F,
                "toast stack stays inside the viewport");
            runAction([] { savedStackToast->Show("QUEUE-4", {3.0}); });
            bool fading = false;
            for (int frame = 0; frame < 16; ++frame) {
                advance(1);
                const auto sample = paintedToast(ui, "QUEUE-1");
                fading = fading || (sample && sample->opacity > 0.02F && sample->opacity < 0.95F);
            }
            check(reduced || fading, "evicting the oldest toast paints its exit animation");
            advance(12);
            check(!paintedToast(ui, "QUEUE-1") && paintedToast(ui, "QUEUE-2") &&
                paintedToast(ui, "QUEUE-3") && paintedToast(ui, "QUEUE-4"),
                "fourth toast replaces only the oldest after its exit");
            ui.Find(UiSelector::Text("QUEUE-2")).Tap(); advance(16);
            check(!paintedToast(ui, "QUEUE-2") && paintedToast(ui, "QUEUE-3") && paintedToast(ui, "QUEUE-4"),
                "clicking a toast dismisses that row without cancelling its neighbors");
            runAction([] {
                check(!savedStackToast->Dismiss(stackedToastIds[1]), "dismissed identifiers cannot dismiss another row");
                for (int i = 5; i <= 9; ++i) savedStackToast->Show("QUEUE-" + std::to_string(i), {3.0});
            });
            ui.Pump(); advance(30);
            for (int i = 1; i <= 6; ++i)
                check(!paintedToast(ui, "QUEUE-" + std::to_string(i)), "a burst discards superseded notifications");
            for (int i = 7; i <= 9; ++i)
                check(paintedToast(ui, "QUEUE-" + std::to_string(i)).has_value(), "a burst retains the newest three notifications");
            ui.Find(UiSelector::Text("QUEUE-8")).Tap({.device_kind = huxerui::PointerDeviceKind::Touch}); advance(16);
            check(!paintedToast(ui, "QUEUE-8"), "touch can dismiss a middle toast");
            advance(180);
            check(!paintedToast(ui, "QUEUE-7") && !paintedToast(ui, "QUEUE-9"), "remaining toast timers expire independently");
            const auto settled = ui.CaptureSnapshot(); ui.Pump(std::chrono::seconds(1));
            check(settled == ui.CaptureSnapshot(), "empty toast stack has no continuing animation");
            const std::string longMessage = "A longer notification needs to wrap inside the available window width without clipping";
            runAction([longMessage] { savedStackToast->Show(longMessage, {3.0}); });
            advance(16);
            const auto wrapped = ui.Find(UiSelector::Text(longMessage)).One().bounds;
            check(wrapped.x >= 16.0F && wrapped.x + wrapped.width <= 384.0F &&
                wrapped.y >= 0.0F && wrapped.y + wrapped.height <= 476.0F,
                "long notifications wrap inside the padded viewport");
            ui.Find(UiSelector::Text("Show three toasts")).Tap(); ui.Pump();
        }
        check(savedStackToast->Show("AFTER-CLOSE") == 0,
            "retained handles cannot resurrect toasts after the window unmounts");
        savedStackToast.reset();
    }
}

struct RootDockFixture {
    huxerui::State<std::size_t> selected{0U};
    huxerui::State<int> count{0};
    huxerui::NavigationController navigation;
    int mounts = 0, unmounts = 0;
};
std::shared_ptr<RootDockFixture> rootDockFixture;
huxerui::View RootDockSecondary(int depth) {
    return huxerui::Scope([depth] {
        const auto nav = huxerui::UseNavigation();
        const auto& theme = huxerui::UseTheme();
        const huxerui::TransitionSpec enter{huxerui::SlideTransition{}, huxerui::TweenSpec{0.3}};
        return huxerui::Column {
          huxerui::Text("SECONDARY-" + std::to_string(depth)),
          huxerui::Button("DEEPER").OnClick([nav, depth] { nav.Push(RootDockSecondary, depth + 1); }),
          huxerui::Button("POP").OnClick([nav] { static_cast<void>(nav.Pop()); }),
        }.With(huxerui::Grow(1.0F), huxerui::Background(theme.colors.surface_container_low),
            huxerui::PageTransition{.push = enter, .pop = enter.Reversed(huxerui::TweenSpec{0.2})});
    });
}
huxerui::View RootDockRoot() {
    return huxerui::FlatTheme{huxerui::Scope([] {
        const auto model = huxerui::UseState(std::make_shared<RootDockFixture>()).Get();
        rootDockFixture = model;
        auto stack = huxerui::NavigationStack([model] {
            return huxerui::Scope([model] {
                const auto nav = huxerui::UseNavigation();
                model->navigation = nav;
                clashflux::ui::UsePrimaryPageNavigation(nav, model->selected);
                auto dock = huxerui::Scope([model] {
                    huxerui::Lifecycle([model] {
                        ++model->mounts;
                        return [model] { ++model->unmounts; };
                    }, 0);
                    return huxerui::Button("DOCK-" + std::to_string(model->count.Get()))
                        .With(huxerui::Frame{.height = 64.0F})
                        .OnClick([model] { model->count = model->count.Get() + 1; });
                });
                return clashflux::ui::CompactRootPageSurface(huxerui::Column {
                  huxerui::Text("PRIMARY-" + std::to_string(model->selected.Get())),
                  model->selected.Get() == 1 ? huxerui::View{huxerui::Column {
                    clashflux::ui::HomeCardSurface(huxerui::UseTheme(), huxerui::Text("HOME-FIRST"), "first")
                        .With(huxerui::Frame{.height = 88.0F}),
                    clashflux::ui::HomeCardSurface(huxerui::UseTheme(), huxerui::Text("HOME-SECOND"), "second")
                        .With(huxerui::Frame{.height = 88.0F}),
                  }} : huxerui::View{},
                  huxerui::Button("OPEN").OnClick([nav] { nav.Push(RootDockSecondary, 1); }),
                }.With(huxerui::Grow(1.0F)), dock);
            });
        });
        return clashflux::ui::ResponsiveDesktopShell(stack,
            huxerui::Text("SIDE").With(huxerui::Frame{.width = 80.0F}),
            huxerui::Text("CHROME").With(huxerui::Frame{.height = 40.0F}), {});
    })};
}
const huxerui::Application kRootDockApplication{RootDockRoot, {.show_debug_overlay = false}};
void testRetainedRootDock() {
    huxerui::testing::UiTest ui(kRootDockApplication, {.viewport = {400.0F, 700.0F}});
    ui.PumpAndSettle();
    ui.Find(UiSelector::Text("DOCK-0")).Tap(); ui.PumpAndSettle();
    check(paintedToast(ui, "DOCK-1").has_value(), "the root dock is painted before navigation");
    ui.Find(UiSelector::Text("OPEN")).Tap(); ui.PumpAndSettle();
    check(rootDockFixture->mounts == 1 && rootDockFixture->unmounts == 0,
        "pushing a secondary page retains the root dock instead of hiding/unmounting it");
    check(!paintedToast(ui, "DOCK-1"), "the settled secondary page covers the dock and scrim");
    ui.Find(UiSelector::Text("POP")).Tap(); ui.Pump(std::chrono::milliseconds(40));
    check(ui.Find(UiSelector::Text("DOCK-1")).Exists() && rootDockFixture->mounts == 1 &&
          rootDockFixture->unmounts == 0,
        "the return transition already owns the original mounted dock before any page cleanup");
    ui.PumpAndSettle();
    check(rootDockFixture->mounts == 1 && rootDockFixture->unmounts == 0,
        "returning preserves the same mounted dock and its state");
    ui.Find(UiSelector::Text("OPEN")).Tap(); ui.PumpAndSettle();
    for (float width : {960.0F, 320.0F, 599.0F, 400.0F}) {
        ui.SetWindowMetrics({.viewport = {width, 700.0F}}); ui.PumpAndSettle();
        check(rootDockFixture->navigation.Depth() == 2 && !paintedToast(ui, "DOCK-1"),
            "breakpoint changes keep the secondary route above the root dock");
    }
    ui.Find(UiSelector::Text("DEEPER")).Tap(); ui.PumpAndSettle();
    check(rootDockFixture->navigation.Depth() == 3, "nested secondary routes use the same navigation stack");
    rootDockFixture->selected = 1; ui.PumpAndSettle();
    check(rootDockFixture->navigation.Depth() == 1 && paintedToast(ui, "DOCK-1").has_value(),
        "changing primary pages clears nested routes through logical navigation, without route ownership flags");
    check(ui.Find(UiSelector::Text("HOME-FIRST")).Exists() && ui.Find(UiSelector::Text("HOME-SECOND")).Exists(),
        "switching directly from a secondary route to home accepts all card shared markers");
}

void testHomeCardFlip() {
    using huxerui::testing::UiSemanticSelector;
    for (const auto* app : {&kHomeCardFlipLight, &kHomeCardFlipDark, &kHomeCardFlipReduced}) {
        for (const auto device : {huxerui::PointerDeviceKind::Mouse, huxerui::PointerDeviceKind::Touch}) {
            const bool reduced = app == &kHomeCardFlipReduced;
            homeCardReveals = 0;
            homeEasterEggOpenedUrl.clear();
            huxerui::testing::UiTest ui(*app, {.viewport = {400.0F, 700.0F}, .resource_provider = AppResources()});
            ui.PumpAndSettle();
            const auto first = ui.Find(UiSelector::Key("flip-card-one"));
            const auto second = ui.Find(UiSelector::Key("flip-card-two"));
            const auto excluded = ui.Find(UiSelector::Key("flip-card-excluded"));
            const auto firstBounds = first.One().bounds;
            const auto secondBounds = second.One().bounds;
            const auto tap = [&](const huxerui::testing::UiNodeQuery& card) {
                const auto b = card.One().bounds;
                ui.TapAt({b.x + 8.0F, b.y + 8.0F}, {.device_kind = device});
            };
            const auto doubleTap = [&](const huxerui::testing::UiNodeQuery& card) {
                tap(card); ui.Pump(std::chrono::milliseconds(70)); tap(card); ui.Pump();
            };
            ui.Find(UiSelector::Key("flip-counter")).Tap();
            ui.PumpAndSettle();
            const auto counterBounds = ui.Find(UiSelector::Key("flip-counter")).One().bounds;
            const huxerui::Point counterPoint{counterBounds.x + counterBounds.width * 0.5F,
                                              counterBounds.y + counterBounds.height * 0.5F};
            tap(first); ui.PumpAndSettle();
            check(!first.Find(UiSelector::Key("home-card-back")).Exists() &&
                  ui.Find(UiSelector::Text("Counter-1")).Exists(),
                  "single card taps and nested controls retain their existing behavior");
            doubleTap(first);
            check(first.Find(UiSelector::Key("home-card-back")).Exists(),
                  "mouse double click and touch double tap both turn the card over");
            check(homeCardReveals == 1, "a reveal draws its Easter egg exactly once");
            bool scaled = false;
            bool backPainted = false;
            for (int frame = 0; frame < 8; ++frame) {
                ui.Pump(std::chrono::milliseconds(30));
                const float scale = paintedHomeCardScale(ui);
                scaled = scaled || scale < 0.999F;
                check(scale >= 0.95F, "content never collapses to an edge-on shared snapshot");
                backPainted = backPainted ||
                    ui.CaptureSnapshot().ToString().find("Flip back menu") != std::string::npos;
            }
            check(reduced ? !scaled : scaled,
                  "the live content lightly scales during reveal and reduced motion switches immediately");
            check(backPainted, "the short reveal paints the new face");
            check(first.One().bounds == firstBounds && second.One().bounds == secondBounds,
                  "flipping a card does not change dashboard slots or move its neighbors");
            ui.PumpAndSettle();
            check(!ui.FindSemantics(UiSemanticSelector::Label("Counter-1")).Exists(),
                  "the back face hides and disables front controls");
            ui.Find(UiSelector::Key("flip-back-action")).Tap(); ui.PumpAndSettle();
            check(ui.Find(UiSelector::Text("Back action-1")).Exists() &&
                  !ui.FindSemantics(UiSemanticSelector::Label("Counter-1")).Exists(),
                  "back menu actions stay clickable while front controls remain disabled");
            doubleTap(second); ui.PumpAndSettle();
            check(first.Find(UiSelector::Key("home-card-back")).Exists() &&
                  second.Find(UiSelector::Key("home-card-back")).Exists(),
                  "each card retains its own independent flip state and local transition");
            check(ui.Find(UiSelector::Text("FinalScave")).Exists() &&
                  homeEasterEggOpenedUrl.empty(), "the author Easter egg never opens its link automatically");
            ui.Find(UiSelector::Key("home-easter-egg-link")).Tap(); ui.PumpAndSettle();
            check(homeEasterEggOpenedUrl == "https://github.com/FinalScave",
                  "the only pool item opens the verified HuxerUI creator profile on click");
            ui.TapAt(counterPoint, {.device_kind = device}); ui.PumpAndSettle();
            check(first.Find(UiSelector::Key("home-card-back")).Exists() &&
                  ui.Find(UiSelector::Text("Counter-1")).Exists(),
                  "a single back tap over the original control cannot activate the hidden front");
            ui.Pump(std::chrono::seconds(1));
            ui.TapAt(counterPoint, {.device_kind = device}); ui.Pump(std::chrono::milliseconds(70));
            ui.TapAt(counterPoint, {.device_kind = device}); ui.PumpAndSettle();
            check(!first.Find(UiSelector::Key("home-card-back")).Exists() &&
                  ui.Find(UiSelector::Text("Counter-1")).Exists(),
                  "double tapping the back restores the front without remounting its state");
            check(homeCardReveals == 1, "returning to the front does not draw another Easter egg");
            ui.Pump(std::chrono::seconds(1));
            doubleTap(first); ui.Pump(std::chrono::milliseconds(30));
            doubleTap(first); ui.PumpAndSettle();
            check(!first.Find(UiSelector::Key("home-card-back")).Exists() &&
                  ui.Find(UiSelector::Text("Counter-1")).Exists() && homeCardReveals == 2,
                  "rapid reversal settles at the latest face and retains the front state");
            doubleTap(excluded); ui.PumpAndSettle();
            check(!excluded.Find(UiSelector::Key("home-card-back")).Exists(),
                  "cat controls explicitly opt out of the dashboard Easter egg");
            const auto settled = ui.CaptureSnapshot();
            ui.Pump(std::chrono::seconds(1));
            check(ui.CaptureSnapshot() == settled, "a completed flip schedules no continuous animation");
        }
    }
}
} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    {
        using namespace clashflux::ui;
        const auto same = ReadAppUpdate(R"({"tag_name":"v0.3.74","draft":false,"prerelease":false,"unknown":{"ok":true}})", "0.3.74");
        check(same.error.empty() && !same.newer, "same stable release is not an update");
        const auto newer = ReadAppUpdate(R"({"tag_name":"v0.3.100","draft":false,"prerelease":false})", "0.3.74");
        check(newer.error.empty() && newer.newer, "release comparison is numeric, not lexical");
        check(!ReadAppUpdate(R"({"tag_name":"v0.3.7","draft":false,"prerelease":false})", "0.3.74").newer,
            "older release never offers a downgrade");
        for (auto body : {"{}", "null", "[]", "{", R"({"tag_name":"v0.4.0","draft":true,"prerelease":false})",
             R"({"tag_name":"v0.4.0","draft":false,"prerelease":true})",
             R"({"tag_name":"v0.4.0","draft":"false","prerelease":false})",
             R"({"tag_name":"https://example.com","draft":false,"prerelease":false})"})
            check(!ReadAppUpdate(body, "0.3.74").error.empty(), "invalid release is an error, not latest");
        for (auto tag : {"v1.2.3-rc1", "v1.2", "v1.2.3/../../evil", "v1.2.4294967296", "v1.2.-1", "v1.2.3?x"})
            check(AppReleaseUrl(tag).empty(), "unsupported or unsafe release tag rejected");
        check(AppReleaseUrl(newer.tag) == "https://github.com/FarnaHerry/Clash-Flux/releases/tag/v0.3.100",
            "release URL stays in the application repository");
    }
    try { testTextDocuments(); }
    catch (const std::exception& exception) {
        std::printf("FAIL: text document fixture: %s\n", exception.what());
        return 1;
    }
    {
        using namespace clashflux::ui;
        const auto disconnected = ResolveHomeVpnStatus(0, {});
        check(disconnected.phase == BoxCatPhase::Sleeping && !disconnected.connected,
              "后台 VPN 未连接时猫咪休眠，不能把常驻内核当作 VPN");
        check(ResolveHomeVpnStatus(0, {.action = CoreAction::Start}).phase == BoxCatPhase::Sleeping,
              "core-only 启动不会显示 VPN 正在连接或已连接");
        check(ResolveHomeVpnStatus(1, {}).phase == BoxCatPhase::Starting,
              "后台 VPN 连接中驱动猫咪等待动画");
        check(ResolveHomeVpnStatus(0, {.action = CoreAction::Tun, .enabled = true}).phase == BoxCatPhase::Starting,
              "共享 VPN 操作等待授权时显示正在连接");
        const auto connected = ResolveHomeVpnStatus(2, {});
        check(connected.phase == BoxCatPhase::Running && connected.connected,
              "仅后台确认连接后猫咪出箱");
        check(ResolveHomeVpnStatus(2, {.action = CoreAction::Tun, .enabled = false}).phase == BoxCatPhase::Stopping,
              "共享断开操作驱动猫咪回箱");
        check(ResolveHomeVpnStatus(3, {}).phase == BoxCatPhase::Failed,
              "后台 VPN 失败驱动失败动画");
        check(ResolveHomeVpnStatus(3, {.action = CoreAction::Tun, .enabled = true}).phase == BoxCatPhase::Starting,
              "失败后重试不会保留旧失败动画");
    }
    reorderGeometryFixture = std::make_shared<clashflux::ui::ProfileGridGeometry>();
    {
        huxerui::testing::UiTest ui(kProfileReorderGeometryApplication,
                                     {.viewport = {1200.0F, 300.0F}});
        ui.PumpAndSettle();
        const auto& slots = reorderGeometryFixture->slots;
        check(slots.size() == 3 && slots.at(1).x < slots.at(2).x &&
              slots.at(2).x < slots.at(3).x,
              "虚拟网格实际记录三个稳定槽位的窗口坐标");
        check(reorderGeometryFixture->Closest({1, 2, 3},
              {slots.at(2).x + 10.0F, slots.at(2).y + 10.0F}) == 2,
              "真实虚拟网格的第二槽位可由指针命中");
    }
    reorderGeometryFixture->slots.clear();
    {
        huxerui::testing::UiTest ui(kProfileReorderDragApplication,
                                     {.viewport = {1200.0F, 300.0F}});
        ui.PumpAndSettle();
        const float secondX = reorderGeometryFixture->slots.at(2).x;
        const float thirdX = reorderGeometryFixture->slots.at(3).x;
        huxerui::PointerEvent event{.type = huxerui::PointerEventType::Down,
            .pointer_id = 81, .position = {thirdX + 20.0F, 20.0F},
            .device_kind = huxerui::PointerDeviceKind::Mouse,
            .changed_button = huxerui::PointerButton::Primary,
            .pressed_buttons = huxerui::PointerButton::Primary};
        ui.SendPointer(event); ui.Pump();
        event.type = huxerui::PointerEventType::Move;
        event.changed_button = huxerui::PointerButton::None;
        event.position.x -= 30.0F;
        ui.SendPointer(event); ui.Pump();
        event.position = {secondX + 20.0F, 20.0F};
        ui.SendPointer(event); ui.Pump();
        ui.Pump(std::chrono::milliseconds(220));
        const auto dragged = ui.Find(UiSelector::Text("profile-3")).One();
        const auto shifted = ui.Find(UiSelector::Text("profile-2")).One();
        check(std::abs(dragged.bounds.x - secondX) < 5.0F &&
              std::abs(shifted.bounds.x - thirdX) < 5.0F,
              "拖动中第三张进入第二槽，第二张平滑让位到第三槽");
        event.type = huxerui::PointerEventType::Up;
        event.changed_button = huxerui::PointerButton::Primary;
        event.pressed_buttons = huxerui::PointerButton::None;
        ui.SendPointer(event); ui.PumpAndSettle();
        check(std::abs(ui.Find(UiSelector::Text("profile-3")).One().bounds.x - secondX) < 5.0F &&
              std::abs(ui.Find(UiSelector::Text("profile-2")).One().bounds.x - thirdX) < 5.0F,
              "松手后第三张确实提交到第二槽");
        const float firstX = reorderGeometryFixture->slots.at(1).x;
        ui.Drag({firstX + 20.0F, 20.0F}, {secondX + 20.0F, 20.0F},
                {.duration = std::chrono::milliseconds(300), .segments = 12,
                 .pointer = {.device_kind = huxerui::PointerDeviceKind::Mouse,
                             .pointer_id = 82}});
        ui.PumpAndSettle();
        check(std::abs(ui.Find(UiSelector::Text("profile-1")).One().bounds.x - secondX) < 5.0F &&
              std::abs(ui.Find(UiSelector::Text("profile-3")).One().bounds.x - firstX) < 5.0F,
              "从第一槽拖到第二槽也能提交排序");
    }
    {
        clashflux::ui::ProfileGridGeometry geometry;
        geometry.slots.emplace(1, huxerui::Rect{0.0F, 80.0F, 620.0F, 310.0F});
        geometry.slots.emplace(2, huxerui::Rect{640.0F, 80.0F, 620.0F, 310.0F});
        geometry.slots.emplace(3, huxerui::Rect{1280.0F, 80.0F, 620.0F, 310.0F});
        const std::vector<std::int64_t> ids{1, 2, 3};
        check(geometry.Closest(ids, {690.0F, 160.0F}) == 2,
              "三张订阅卡时第二个固定槽位可命中");
        check(geometry.Closest(ids, {635.0F, 160.0F}) == 2,
              "卡片间隙落到最近的固定槽位");
        check(clashflux::ui::ProfileReorderDestination(2, 2, 1) == 1 &&
              clashflux::ui::ProfileReorderDestination(1, 2, 1) == 2 &&
              clashflux::ui::ProfileReorderDestination(0, 2, 1) == 0,
              "第三张拖到第二槽时拖动卡和让位卡都进入目标位置");
        check(clashflux::ui::ProfileReorderDestination(0, 0, 1) == 1 &&
              clashflux::ui::ProfileReorderDestination(1, 0, 1) == 0 &&
              clashflux::ui::ProfileReorderDestination(2, 0, 1) == 2,
              "第一张拖到第二槽时两张卡对向滑动");
    }
    testBoxCat();
    testBoxCatMorphAfterSuccess();
    testHomeHeaderControl();
    testHomeHeaderColdStart();
    try { testFixedHomeGeometry(); testSkeletonShimmer(); }
    catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: home geometry / skeleton shimmer: %s\n", error.what());
        ++failures;
    }
    testCoreOperations();
    testServiceInstallContinuation();
    testInboundSettings();
    testConnectionList();
    testDiagnosticsContent();
    testDiagnosticsThemeAndLayout();
    testPptpProgressResources();
    testVpnPublication();
    testCustomThemeColors();
    testThemeColorSurfaces();
    testHomeCardPress();
    try { testModeChoiceFeedback(); testToastExitMotion(); testToastStack(); testRetainedRootDock(); testHomeCardFlip(); }
    catch (const std::exception& error) {
        std::printf("FAIL: home card flip: %s\n", error.what());
        ++failures;
    }
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
    testFilteredRuleIndices();
    testLargeRuleList();
    testDesktopTouchpadSectionPager();
    testCommonMenu();
    testGroupDrawers();
    testGroupPickerSelection();
    testResponsiveDesktopShell();
    testDesktopPageSwitcher();
    testHeaderGeometry();
    testSearchFocusOutline();
    testMobileSearchGeometry();
    testResponsiveAnchorMount();
    testCompactPageBottom();
    testPinnedPageAction();
    testProfileFormNavigation();
    testProfileListSections();
    testUnifiedProfileList();
    try { testCompactProfileCards(); }
    catch (const std::exception& error) {
        std::printf("FAIL: compact profile cards: %s\n", error.what());
        ++failures;
    }
    testEmptyState();
    testThemeSwitch();
    testProfileFileFilter();
    testProfileImportCompletion();
    testScopedProfileRefresh();
    testRuleSourceEligibility();
    try { testSubscriptionRuleLayout(); }
    catch (const std::exception& error) {
        std::printf("FAIL: subscription rule layout: %s\n", error.what());
        ++failures;
    }
    testProfileListPublication();
    testProfileReorderPublish();
    testLatestProfileSelection();
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
