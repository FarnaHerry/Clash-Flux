// settings_page.cpp — 设置页的三个固定模块：通用 / 内核 / 关于。
//
// 平台专属内容由模块入口处的编译宏选择，平台函数内部自洽管理状态、
// 任务、权限和控件；这里不维护 Android/桌面能力矩阵。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app_resources.h"
#include "ui.h"
#include "task_bridge.h"

import clashflux.config;
import clashflux.db;
import clashflux.singbox;
import clashflux.stream;
import clashflux.store.core;
import clashflux.store.profiles;

#include "core_model.h"
#include "profiles_cache.h"
#include "settings_model.h"

namespace clashflux::ui {
namespace {

const std::vector<std::string> kModes{"rule", "global", "direct"};
const std::vector<huxerui::StringVariant> kModeNames{
    Localized("规则"), Localized("全局"), Localized("直连")};
const std::vector<huxerui::SegmentedButtonItem> kThemeItems{
    huxerui::SegmentedButtonItem::IconOnly(app::images::sun_moon, Localized("自动")),
    huxerui::SegmentedButtonItem::IconOnly(app::images::moon, Localized("深色")),
    huxerui::SegmentedButtonItem::IconOnly(app::images::sun, Localized("浅色"))};
const std::vector<std::string> kLanguages{"system", "zh", "en"};
const std::vector<huxerui::StringVariant> kLanguageNames{
    Localized("自动"), "简体中文", "English"};

std::size_t ModeIndex(const std::string& mode) {
    for (std::size_t i = 0; i < kModes.size(); ++i) {
        if (mode == kModes[i]) return i;
    }
    return 0;
}

#if defined(__ANDROID__)

struct SettingsModeIndicator {
    class Extension;

    std::size_t selected_index = 0;
    huxerui::Color fill = huxerui::Color::Transparent();

    bool operator==(const SettingsModeIndicator&) const = default;
};

class SettingsModeIndicator::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode& node, const SettingsModeIndicator& spec) {
        Update(node, spec);
    }

    void Update(huxerui::ViewNode& node, const SettingsModeIndicator& spec) {
        static_cast<void>(node);
        geometry_pending_ =
            geometry_pending_ || !initialized_ ||
            selected_index_ != spec.selected_index;
        selected_index_ = spec.selected_index;
        fill_ = spec.fill;
        initialized_ = true;
    }

    FrameResult OnFrame(huxerui::ViewNode& node,
                        const huxerui::FrameInfo& frame) override {
        static_cast<void>(node);
        const huxerui::MotionAdvanceResult result = offset_.Advance(frame);
        if (result.changed) InvalidatePaint(PaintInvalidation::Content);
        return FrameResult{
            .needs_frame = geometry_pending_ || result.needs_frame,
            .wake_after = result.wake_after};
    }

    PaintInvalidation PrepareGeometry(
        huxerui::ViewNode& node, huxerui::TextMeasurer&) override {
        if (selected_index_ >= node.ChildCount()) {
            return PaintInvalidation::None;
        }
        const float target = node.ChildAt(selected_index_).LayoutOffset().x;
        const float width = node.ChildAt(selected_index_).LayoutSize().width;
        geometry_pending_ = false;
        if (!geometry_initialized_) {
            geometry_initialized_ = true;
            width_ = width;
            offset_.Set(target);
            return PaintInvalidation::Content;
        }
        bool changed = false;
        if (width_ != width) {
            width_ = width;
            changed = true;
        }
        if (offset_.Target() != target) {
            offset_.AnimateTo(
                target, huxerui::TweenSpec{0.22, huxerui::Easing::EaseOut});
            changed = true;
        }
        return changed ? PaintInvalidation::Content : PaintInvalidation::None;
    }

    void PaintBehindContent(const huxerui::ViewNode& node,
                            huxerui::PaintContext& context) const override {
        if (!geometry_initialized_ || width_ <= 0.0F || fill_.alpha <= 0.0F) {
            return;
        }
        context.DrawRect(
            huxerui::Rect{offset_.Value(), 0.0F, width_, node.Bounds().height},
            fill_, 8.0F);
    }

private:
    std::size_t selected_index_ = 0;
    huxerui::Color fill_ = huxerui::Color::Transparent();
    huxerui::MotionController offset_;
    float width_ = 0.0F;
    bool initialized_ = false;
    bool geometry_initialized_ = false;
    bool geometry_pending_ = false;
};

[[huxerui::composable]] huxerui::View AndroidOutboundModeSelector(
    huxerui::State<std::size_t> selected,
    huxerui::State<bool> busy,
    std::function<void(std::size_t)> onChanged) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const std::size_t active = selected.Get();
    huxerui::Color indicator = theme.colors.primary_container;
    huxerui::Color hover = theme.colors.on_surface;
    hover.alpha = 0.08F;
    huxerui::Color press = theme.colors.on_surface;
    press.alpha = 0.14F;
    const huxerui::Indication indication{
        .hover = huxerui::IndicationLayer{
            .fill = huxerui::VisualFill{huxerui::Brush{hover}}},
        .press = huxerui::IndicationLayer{
            .fill = huxerui::VisualFill{huxerui::Brush{press}}},
    };

    std::vector<huxerui::View> entries;
    entries.reserve(kModeNames.size());
    for (std::size_t i = 0; i < kModeNames.size(); ++i) {
        const bool isSelected = i == active;
        const huxerui::Color content =
            isSelected ? theme.colors.on_primary_container
                       : theme.colors.on_surface_variant;
        entries.push_back(
            huxerui::Row {
                huxerui::Text(kModeNames[i]).Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kBody)
                        .WithWeight(isSelected ? huxerui::FontWeight::SemiBold
                                               : huxerui::FontWeight::Regular),
                    content}),
            }
                .With(huxerui::Grow(1.0F),
                      huxerui::Frame{.height = 38.0F},
                      huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                      huxerui::CornerRadius(8.0F), indication,
                      huxerui::Semantics{
                          .role = huxerui::SemanticRole::Tab,
                          .label = kModeNames[i],
                          .selected = isSelected},
                      huxerui::Focusable(true),
                      huxerui::Enabled(!busy.Get()))
                .OnClick([busy, onChanged, i] {
                    if (!busy.Get()) onChanged(i);
                })
                .Key("settings-outbound-" + std::to_string(i)));
    }

    return huxerui::Row(std::move(entries))
        .With(huxerui::Background(theme.colors.surface_container_high),
              huxerui::CornerRadius(10.0F), huxerui::ClipChildren(),
              SettingsModeIndicator{active, indicator});
}

#endif

#if defined(__ANDROID__)
#define CLASHFLUX_OUTBOUND_MODE_SELECTOR AndroidOutboundModeSelector
#else
[[huxerui::composable]] huxerui::View DesktopOutboundModeSelector(
    huxerui::State<std::size_t> selected,
    huxerui::State<bool> busy,
    std::function<void(std::size_t)> onChanged) {
    // 组合期读 busy + 模型值：切换进行中显示乐观值，空闲显示内核权威值。这既让
    // 外部（CLI/托盘）改模式能反映出来，也保证 busy 翻转会触发重组——否则点击被
    // busy 早退时控件已经视觉翻转、却没有任何 State 变化去把它拉回来。
    const auto coreModel = huxerui::UseService<CoreModel>();
    const std::size_t shown =
        busy.Get() ? selected.Get()
                   : ModeIndex(coreModel->view.Get().core.mode);
    return huxerui::SegmentedButton(kModeNames, shown)
        .OnChanged(std::move(onChanged));
}
#define CLASHFLUX_OUTBOUND_MODE_SELECTOR DesktopOutboundModeSelector
#endif

// 宏只选择模块级平台函数，不把平台能力拆成控件级过滤条件。
#if defined(__ANDROID__)
#define CLASHFLUX_GENERAL_PLATFORM_SECTION AndroidGeneralSettings
#define CLASHFLUX_KERNEL_PLATFORM_SECTION AndroidKernelSettings
#define CLASHFLUX_MORE_SETTINGS AndroidMoreSettings
#else
#define CLASHFLUX_GENERAL_PLATFORM_SECTION DesktopGeneralSettings
#define CLASHFLUX_KERNEL_PLATFORM_SECTION DesktopKernelSettings
#define CLASHFLUX_MORE_SETTINGS DesktopMoreSettings
#endif

// 编译保真度报告（见 docs/singbox-layers-and-fidelity.md §2）：订阅里存在降级 /
// 跳过时才渲染，空报告返回空占位——不做常驻提示。subject 留给程序化消费（例如
// 代理页给被降级的组打角标），这里直接展示可读的 detail + 建议动作。
[[huxerui::composable]] huxerui::View CoreFidelityReport(
    const std::vector<singbox::FidelityNote>& notes) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto expanded = huxerui::UseState(false);
    if (notes.empty()) return huxerui::View{huxerui::Row{}};
    constexpr std::size_t kMaxRows = 6;
    const std::size_t shown = expanded.Get() ? notes.size() : std::min(notes.size(), kMaxRows);
    std::vector<huxerui::View> rows;
    rows.reserve(shown + 1);
    for (std::size_t i = 0; i < shown; ++i) {
        const singbox::FidelityNote& note = notes[i];
        const std::string level = huxerui::UseString(Localized(
            note.level == singbox::Fidelity::Unsupported ? "不支持" : "已近似"));
        std::string text = huxerui::UseString(
            LocalizedFormat("· [{}] {}", level, (note.sourceId.empty() ? "" : note.sourceId + " · ") + note.detail));
        if (!note.action.empty()) {
            text += huxerui::UseString(LocalizedFormat("（{}）", note.action));
        }
        rows.push_back(huxerui::Text(std::move(text))
                           .Style(huxerui::TextStyle{
                               huxerui::Font::System(font_size::kCaption),
                               theme.colors.on_surface_variant}));
    }
    if (notes.size() > shown) {
        rows.push_back(huxerui::Text(LocalizedFormat(
                           "· 另有 {} 条降级 / 跳过记录", notes.size() - shown))
                           .Style(huxerui::TextStyle{
                               huxerui::Font::System(font_size::kCaption),
                               theme.colors.on_surface_variant}));
    }
    if (notes.size() > kMaxRows) rows.push_back(
        huxerui::Button(Localized(expanded.Get() ? "收起" : "显示全部保真度记录"))
            .OnClick([expanded] { expanded = !expanded.Get(); }));
    return huxerui::Column {
        SectionTitle(Localized("配置保真度")),
        huxerui::Column(std::move(rows))
            .With(huxerui::Spacing(4.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
    }.With(huxerui::Spacing(6.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

const huxerui::StringVariant kAboutText = LocalizedFormat(
    "Clash-Flux v{} · sing-box 内核（桌面 spawn / Android libbox）",
    CLASHFLUX_VERSION);

} // namespace

#if defined(__ANDROID__)

// 单行导航项：整行可点，自带触控高度；用于「更多」入口段内部。
// 手机端二级页经 NavigationStack push，因此进入/返回动画与一级页切换无关。
[[huxerui::composable]] huxerui::View MoreNavRow(huxerui::StringVariant label,
                                                 std::function<void()> open) {
    return huxerui::Row {
        huxerui::Text(label),
        huxerui::Spacer(),
        huxerui::Text("›"),
    }.With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(0.0F, 10.0F)),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
        .OnClick(std::move(open))
        .With(huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                 .label = huxerui::UseString(label)},
              huxerui::Focusable(true), huxerui::Enabled(true));
}

[[huxerui::composable]] huxerui::View AndroidMoreSettings(
    huxerui::State<std::size_t>, ProfilesCache profilesCache) {
    const huxerui::NavigationController navigation = huxerui::UseNavigation();
    // 同组入口合并成一段，行间用 Divider 分隔；不再套分组卡——设置页整体不用
    // 卡片包裹（桌面端页面岛即唯一卡片，移动端直接铺在页面底色上）。
    return huxerui::Column {
        SectionTitle(Localized("更多")),
        huxerui::Column {
            MoreNavRow(Localized("连接"),
                       [navigation] { navigation.Push(AndroidConnectionsPage); }),
            huxerui::Divider(),
            MoreNavRow(Localized("日志"),
                       [navigation] { navigation.Push(AndroidLogsPage); }),
            huxerui::Divider(),
            MoreNavRow(Localized("规则"), [navigation, profilesCache] {
                navigation.Push([profilesCache] {
                    return AndroidRulesPage(profilesCache);
                });
            }),
        }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
    }.With(huxerui::Spacing(10.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

#else

[[huxerui::composable]] huxerui::View DesktopMoreSettings(
    huxerui::State<std::size_t> navPage, ProfilesCache) {
    if (huxerui::UseViewportClass() != huxerui::ViewportClass::Compact) return huxerui::View{};
    return huxerui::Column{
        SectionTitle(Localized("更多")),
        huxerui::Button(Localized("连接")).OnClick([navPage] { navPage = 4U; }),
        huxerui::Button(Localized("日志")).OnClick([navPage] { navPage = 5U; }),
        huxerui::Button(Localized("规则")).OnClick([navPage] { navPage = 3U; }),
    }.With(huxerui::Spacing(10.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

#endif

[[huxerui::composable]] huxerui::View SettingsPage(
    huxerui::State<int> themeMode, huxerui::State<std::size_t> navPage,
    ProfilesCache profilesCache, bool active) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const bool compact =
        huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    auto tasks = huxerui::UseTaskScope();
    auto transition = huxerui::UseSceneTransition();
    struct ThemeAnimationFlag {
        bool animating = false;
    };
    auto animating =
        huxerui::UseState(std::make_shared<ThemeAnimationFlag>());
    auto toast = huxerui::UseToast();
    const auto settingsModel = huxerui::UseService<SettingsModel>();
    const SettingsView settingsView = settingsModel->view.Get();
    auto portValue = huxerui::UseState(huxerui::TextEditingValue{""});
    // 出站模式：HTTP 热更，快且可能失败 → 保留本地乐观值 + busy 单飞；
    // 成功时把权威值写透模型，失败才回落（见 applyOutboundMode）。
    auto busy = huxerui::UseState(false);
    auto modeSelection = huxerui::UseState(
        ModeIndex(store::coreStore().snapshot().mode));

    // 内核/接管状态来自唯一来源 CoreModel（见 core_model.h）：以模型的 State
    // 作依赖镜像到本页的受控值——模型一变就同步一次，不再是 1s 定时器。
    const auto coreModel = huxerui::UseService<CoreModel>();
    const CoreView coreView = coreModel->view.Get();
    huxerui::Lifecycle(
        [portValue, busy, modeSelection, coreModel] {
            const CoreView view = coreModel->view.Get();
            if (!busy.Get()) modeSelection = ModeIndex(view.core.mode);
            if (portValue.Get().text.empty() && view.core.mixedPort > 0) {
                portValue = huxerui::TextEditingValue{
                    std::to_string(view.core.mixedPort)};
            }
            return [] {};
        },
        coreModel->view);

    // 不可见时只保留本页 State/Lifecycle，不构建重子树：huxerui 的 Pager 会把
    // 四个一级页同时挂载，隐藏页即使不重组，其已挂载子树仍随每一帧被重新测量。
    // 真机实测（代理页大分组）：四页同挂时每帧 1443 次测量请求 / ~20ms，
    // 只留当前页后降到 28 次 / ~0ms；因此不可见页必须返回空占位。
    if (!active) return huxerui::View{huxerui::Row{}}.Key("settings-idle");

    const auto applyLanguage = [settingsModel](std::size_t index) {
        if (index >= kLanguages.size()) return;
        const std::string language = kLanguages[index];
        store::coreStore().setSetting("ui.language", language);
        settingsModel->Update([language](SettingsView& settings) {
            settings.language = language;
        });
    };

    // 主题模式：0=自动，1=深色，2=浅色。
    const auto applyTheme = [themeMode, transition, tasks, animating,
                             settingsModel](int mode) {
        if (animating.Get()->animating) return;
        const bool currentDark =
            themeMode.Get() == 1 ||
            (themeMode.Get() == 0 && cfg::systemPrefersDark());
        const bool targetDark =
            mode == 1 || (mode == 0 && cfg::systemPrefersDark());
        const auto mutation = [themeMode, mode, settingsModel] {
            store::coreStore().setSetting("ui.theme_mode", std::to_string(mode));
            settingsModel->Update([mode](SettingsView& settings) {
                settings.themeMode = mode;
            });
            themeMode = mode;
        };
        if (currentDark == targetDark) {
            mutation();
            return;
        }

        animating.Get()->animating = true;
        tasks.Launch([animating]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0.5});
            animating.Get()->animating = false;
        });
        const huxerui::TransitionSpec reveal{
            huxerui::CircularRevealTransition{}, huxerui::TweenSpec{0.36}};
        transition.RunFromCurrentInteraction(
            currentDark ? reveal.Reversed() : reveal, std::move(mutation));
    };

    // 出站模式：HTTP 热更，快但可能失败 → 本地乐观值 + busy 单飞；成功把权威值
    // 写透模型，失败做目标值校验后回落。
    const auto applyOutboundMode = [tasks, toast, busy, modeSelection,
                                    coreModel](
                                       std::size_t index) {
        if (busy.Get() || index >= kModes.size()) return;
        const std::size_t previous = modeSelection.Get();
        if (index == previous) return;
        modeSelection = index;
        busy = true;
        tasks.Launch([=]() -> huxerui::Task<void> {
            bool ok = false;
            std::string error;
            try {
                ok = co_await RunOnTaskThread(
                    [index] { return store::coreStore().applyMode(kModes[index]); });
            } catch (const std::exception& exception) {
                error = exception.what();
                stream::logApplication(
                    "error", std::format("出站模式切换失败：{}", error));
            }
            busy = false;
            if (!ok) {
                // 失败回落：目标值校验（busy 期间点击被早退，正常情况下界面还停在
                // 本任务的意图值；若外部改过则不覆盖）。
                if (modeSelection.Get() == index) modeSelection = previous;
                if (error.empty()) {
                    toast.Show(Localized("出站模式切换失败"));
                } else {
                    toast.Show(error);
                }
                co_return;
            }
            // 成功不回写本地 State：把权威值写透模型，镜像与其它入口立刻一致。
            const std::string mode = kModes[index];
            coreModel->Update(
                [mode](CoreView& view) { view.core.mode = mode; });
        });
    };

    return PageScaffold(
        Localized("设置"), huxerui::View{},
        huxerui::ScrollView(
            huxerui::Column {
                CLASHFLUX_MORE_SETTINGS(navPage, profilesCache),
                // 分区（通用/内核/关于）由 SectionTitle 与间距表达。
                huxerui::Column {
                    SectionTitle(Localized("通用")),
                    SettingRow(
                        Localized("主题"), "",
                        huxerui::SegmentedButton(
                            kThemeItems, static_cast<std::size_t>(themeMode.Get()))
                            .OnChanged([applyTheme](std::size_t index) {
                                applyTheme(static_cast<int>(index));
                            })),
                    SettingRow(
                        Localized("语言"), Localized("自动跟随系统语言"),
                        huxerui::Select(
                            kLanguageNames,
                            settingsView.language == "en"
                                ? 2U
                                : settingsView.language == "zh" ? 1U : 0U,
                            [](const huxerui::StringVariant& name) {
                                return huxerui::Text(name);
                            })
                            .OnChanged(applyLanguage)
                            .With(huxerui::Frame{.width = 180.0F})),
                    CLASHFLUX_GENERAL_PLATFORM_SECTION(),
                }.With(huxerui::Spacing(10.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),

                huxerui::Column {
                    SectionTitle(Localized("内核")),
                    SettingRow(
                        Localized("出站模式"), "",
                        CLASHFLUX_OUTBOUND_MODE_SELECTOR(
                            modeSelection, busy, applyOutboundMode)),
                    SettingRow(
                        Localized("混合端口"), Localized("HTTP/SOCKS 混合入站端口（下次启动生效）"),
                        huxerui::Row {
                            huxerui::TextField(portValue.Get())
                                .Variant(huxerui::TextFieldVariant::Outlined)
                                .OnChanged([portValue](
                                               const huxerui::TextEditingValue& value) {
                                    portValue = value;
                                })
                                .With(huxerui::Frame{.width = 100.0F}),
                            huxerui::IconButton(app::images::save, Localized("保存设置"))
                                .With(huxerui::Tooltip(Localized("保存设置")))
                                .OnClick([portValue, toast] {
                                try {
                                    const int port = std::stoi(portValue.Get().text);
                                    if (port < 1 || port > 65535) throw 0;
                                    store::coreStore().setSetting(
                                        "core.mixed_port", std::to_string(port));
                                    toast.Show(Localized("端口已保存（重启内核生效）"));
                                } catch (...) {
                                    toast.Show(Localized("端口无效"));
                                }
                            }),
                        }.With(huxerui::Spacing(8.0F))),
                    CLASHFLUX_KERNEL_PLATFORM_SECTION(),
                    SettingSwitchRow(
                        Localized("局域网连接"), Localized("允许局域网设备接入（下次启动生效）"),
                        huxerui::Switch(coreView.allowLan)
                            .OnChanged([coreModel, toast](bool on) {
                                // KV 写内存缓存是同步的：写完立刻写透模型，界面
                                // 不需要 pending，也不会被下一个泵节拍打回去
                                // （显示值取 KV 意图，见 CoreView::allowLan）。
                                store::coreStore().setSetting(
                                    "core.allow_lan", on ? "true" : "false");
                                coreModel->Update([on](CoreView& view) {
                                    view.allowLan = on;
                                });
                                toast.Show(Localized(
                                    on ? "已允许局域网连接（重启内核生效）"
                                       : "已关闭局域网连接"));
                            })),
                    SettingSwitchRow(
                        "IPv6", Localized("重启内核生效"),
                        huxerui::Switch(coreView.ipv6Enabled)
                            .OnChanged([coreModel, toast](bool on) {
                                store::coreStore().setSetting(
                                    "core.ipv6_enabled", on ? "true" : "false");
                                coreModel->Update([on](CoreView& view) {
                                    view.ipv6Enabled = on;
                                });
                                toast.Show(Localized(
                                    on ? "已启用 IPv6（重启内核生效）"
                                       : "已关闭 IPv6（重启内核生效）"));
                            })),
                    CoreFidelityReport(coreView.core.fidelity),
                }.With(huxerui::Spacing(10.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),

                huxerui::Column {
                    SectionTitle(Localized("关于")),
                    huxerui::Text(kAboutText).Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kChip),
                        theme.colors.on_surface_variant}),
                }.With(huxerui::Spacing(6.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
                compact ? CompactFloatingNavigationFooter() : huxerui::View{},
            }.With(huxerui::Spacing(12.0F),
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)))
            .With(huxerui::Grow(1.0F)), false, false, true);
}

#undef CLASHFLUX_GENERAL_PLATFORM_SECTION
#undef CLASHFLUX_KERNEL_PLATFORM_SECTION
#undef CLASHFLUX_MORE_SETTINGS

} // namespace clashflux::ui
