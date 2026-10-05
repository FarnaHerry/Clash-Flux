// settings_page.cpp — 设置页的三个固定模块：通用 / 内核 / 关于。
//
// 平台专属内容由模块入口处的编译宏选择，平台函数内部自洽管理状态、
// 任务、权限和控件；这里不维护 Android/桌面能力矩阵。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app_resources.h"
#include "ui.h"
#include "action_menu.h"
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
#include "theme_colors.h"
#include "theme_color_card.h"

namespace clashflux::ui {
namespace {

const std::vector<std::string> kModes{"rule", "global", "direct"};
const std::vector<huxerui::StringVariant> kModeNames{
    Localized("规则"), Localized("全局"), Localized("直连")};
const std::vector<huxerui::StringVariant> kThemeNames{
    Localized("自动"), Localized("深色"), Localized("浅色")};
const std::vector<std::string> kLanguages{"system", "zh", "en"};
const std::vector<huxerui::StringVariant> kLanguageNames{
    Localized("自动"), "简体中文", "English"};
constexpr float kPortSettingsDialogWidth = 320.0F;
constexpr float kPortSettingsEntryMinHeight = 48.0F;
constexpr float kPortSettingsEntryHorizontalInset = 10.0F;

std::optional<int> ParsePortText(const huxerui::TextEditingValue& value,
                                 bool optional) {
    std::string_view text = value.text;
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                             text.front() == '\r' || text.front() == '\n')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                             text.back() == '\r' || text.back() == '\n')) {
        text.remove_suffix(1);
    }
    if (text.empty()) return optional ? std::optional<int>{0} : std::nullopt;
    int port = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), port);
    if (error != std::errc{} || end != text.data() + text.size() ||
        port < 1 || port > 65535) {
        return std::nullopt;
    }
    return port;
}

std::size_t LanguageIndex(const std::string& language) {
    const auto found = std::find(kLanguages.begin(), kLanguages.end(), language);
    return found == kLanguages.end() ? 0U : static_cast<std::size_t>(found - kLanguages.begin());
}

void ApplyLanguage(const std::shared_ptr<SettingsModel>& model, std::size_t index) {
    if (index >= kLanguages.size()) return;
    const std::string language = kLanguages[index];
    store::coreStore().setSetting("ui.language", language);
    model->Update([language](SettingsView& settings) { settings.language = language; });
}

std::size_t ModeIndex(const std::string& mode) {
    for (std::size_t i = 0; i < kModes.size(); ++i) {
        if (mode == kModes[i]) return i;
    }
    return 0;
}

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

[[huxerui::composable]] huxerui::View CompactOutboundModeSelector(
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

[[huxerui::composable]] huxerui::View WideOutboundModeSelector(
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

[[huxerui::composable]] huxerui::View ResponsiveOutboundModeSelector(
    huxerui::State<std::size_t> selected,
    huxerui::State<bool> busy,
    std::function<void(std::size_t)> onChanged) {
    if (huxerui::UseViewportClass() == huxerui::ViewportClass::Compact)
        return CompactOutboundModeSelector(selected, busy, std::move(onChanged));
    return WideOutboundModeSelector(selected, busy, std::move(onChanged));
}

// 宏只选择模块级平台函数，不把平台能力拆成控件级过滤条件。
#if defined(__ANDROID__)
#define CLASHFLUX_GENERAL_PLATFORM_SECTION AndroidGeneralSettings
#define CLASHFLUX_KERNEL_PLATFORM_SECTION AndroidKernelSettings
#define CLASHFLUX_MORE_SETTINGS AndroidMoreSettings
#define CLASHFLUX_LANGUAGE_SETTING AndroidLanguageSetting
#define CLASHFLUX_THEME_SETTING AndroidThemeSetting
#else
#define CLASHFLUX_GENERAL_PLATFORM_SECTION DesktopGeneralSettings
#define CLASHFLUX_KERNEL_PLATFORM_SECTION DesktopKernelSettings
#define CLASHFLUX_MORE_SETTINGS DesktopMoreSettings
#define CLASHFLUX_LANGUAGE_SETTING DesktopLanguageSetting
#define CLASHFLUX_THEME_SETTING DesktopThemeSetting
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
        huxerui::Row {
          SettingItemIcon(app::images::shield_check),
          SectionTitle(Localized("配置保真度")),
        }.With(huxerui::Spacing(12.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
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

// 图标与名称共用整行点击区域，入口布局不依赖平台。
[[huxerui::composable]] huxerui::View SettingNavigationItem(
    huxerui::StringVariant label, huxerui::ImageResource icon, std::function<void()> open) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Row {
      SettingItemIcon(icon),
      huxerui::Text(label).With(huxerui::Grow(1.0F)),
    }.With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(0.0F, 10.0F)),
           huxerui::Frame{.min_height = 48.0F}, huxerui::Spacing(12.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
           theme.interactions.indication, huxerui::Focusable(true),
           huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                             .label = huxerui::UseString(label)})
        .OnClick(std::move(open));
}

[[huxerui::composable]] huxerui::View MoreNavRow(
    huxerui::StringVariant label, huxerui::ImageResource icon, std::function<void()> open) {
    return SettingNavigationItem(label, icon, open);
}

void SelectThemeColor(const std::shared_ptr<SettingsModel>& model, const std::string& id) {
    if (model->view.Get().themeColor == id) return;
    store::coreStore().setSetting("ui.theme_color", id);
    model->Update([id](SettingsView& settings) { settings.themeColor = id; });
}

void DeleteThemeColor(const std::shared_ptr<SettingsModel>& model, const std::string& id) {
    const SettingsView current = model->view.Get();
    const auto colors = ReadCustomThemeColors(current.customThemeColors);
    const auto hidden = ReadHiddenThemeColors(current.hiddenThemeColors);
    const bool preset = IsPresetThemeColor(id);
    const auto remaining = preset ? colors : RemoveCustomThemeColor(colors, id);
    const auto remainingHidden = preset ? HidePresetThemeColor(hidden, id) : hidden;
    const bool removeColor = remaining != colors;
    const bool hideColor = remainingHidden != hidden;
    const bool resetSelection = current.themeColor == id;
    if (!removeColor && !hideColor && !resetSelection) return;

    const std::string saved = removeColor ? SaveCustomThemeColors(remaining) : current.customThemeColors;
    const std::string savedHidden = hideColor ? SaveHiddenThemeColors(remainingHidden) : current.hiddenThemeColors;
    std::string selectedColor = current.themeColor;
    if (resetSelection) {
        const auto firstVisiblePreset = std::find_if(kFluxAccents.begin(), kFluxAccents.end(),
            [&remainingHidden](const FluxAccent& accent) {
                return std::find(remainingHidden.begin(), remainingHidden.end(), accent.id) == remainingHidden.end();
            });
        if (firstVisiblePreset != kFluxAccents.end()) selectedColor = firstVisiblePreset->id;
        else if (!remaining.empty()) selectedColor = remaining.front();
        else selectedColor = "blue";
    }
    if (removeColor) store::coreStore().setSetting("ui.custom_theme_colors", saved);
    if (hideColor) store::coreStore().setSetting("ui.hidden_theme_colors", savedHidden);
    if (resetSelection) store::coreStore().setSetting("ui.theme_color", selectedColor);
    model->Update([saved, savedHidden, selectedColor, removeColor, hideColor, resetSelection](SettingsView& settings) {
        if (removeColor) settings.customThemeColors = saved;
        if (hideColor) settings.hiddenThemeColors = savedHidden;
        if (resetSelection) settings.themeColor = selectedColor;
    });
}

[[huxerui::composable]] huxerui::View ThemeColorDialog(
    std::shared_ptr<SettingsModel> model, huxerui::DialogContext context,
    std::string initialHex, std::optional<std::string> editingId) {
    const auto& theme = huxerui::UseTheme();
    const bool editing = editingId.has_value();
    auto input = huxerui::UseState(huxerui::TextEditingValue{initialHex});
    auto hsb = huxerui::UseState(ThemeColorToHsb(ThemeColorFromHex(initialHex)));
    const auto hex = NormalizeThemeColor(input.Get().text);
    const HsbColor currentHsb = hsb.Get();
    const auto preview = hex ? ThemeColorFromHex(*hex) : ThemeColorFromHsb(currentHsb);
    std::vector<huxerui::View> sliders;
    const std::array<float, 3> channels{
        currentHsb.hue,
        currentHsb.saturation * kHsbPercentMaximum,
        currentHsb.brightness * kHsbPercentMaximum};
    const std::array<float, 3> maximums{
        kHsbHueSliderMaximum, kHsbPercentMaximum, kHsbPercentMaximum};
    const std::array<std::string_view, 3> units{"°", "%", "%"};
    const std::array<HsbChannel, 3> channelsByKind{
        HsbChannel::Hue, HsbChannel::Saturation, HsbChannel::Brightness};
    const auto sliderStyle = HsbGradientSliderStyle(huxerui::UseEnvironment<huxerui::SliderStyle>(), preview);
    for (std::size_t index = 0; index < channels.size(); ++index) {
        const HsbChannel channel = channelsByKind[index];
        sliders.push_back(huxerui::Row {
          huxerui::Stack {
            HsbGradientSliderTrack(HsbChannelGradient(currentHsb, channel), sliderStyle),
            huxerui::ProvideEnvironment(sliderStyle,
                huxerui::Slider(std::round(channels[index])).Range(0.0F, maximums[index]).Step(1.0F)
                    .OnChanged([input, hsb, channel](float value) {
                        const HsbColor changed = UpdateHsbChannel(hsb.Get(), channel, value);
                        hsb = changed;
                        input = huxerui::TextEditingValue{ThemeColorToHex(ThemeColorFromHsb(changed))};
                    })),
          }.With(huxerui::Grow(1.0F), huxerui::Align(huxerui::HorizontalAlignment::Stretch,
                                                    huxerui::VerticalAlignment::Center)),
          huxerui::Text(std::to_string(static_cast<int>(std::round(channels[index]))) +
                        std::string{units[index]})
              .With(huxerui::Frame{.width = theme.spacing.extra_large}),
        }.With(huxerui::Spacing(theme.spacing.small), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
    }
    const auto save = [model, input, context, editingId] {
        const auto color = NormalizeThemeColor(input.Get().text);
        if (!color || !model->view.Get().ready) return;
        const SettingsView current = model->view.Get();
        auto colors = ReadCustomThemeColors(current.customThemeColors);
        auto hiddenColors = ReadHiddenThemeColors(current.hiddenThemeColors);
        if (editingId) {
            colors = ReplaceCustomThemeColor(std::move(colors), *editingId, *color);
            hiddenColors = HidePresetThemeColor(std::move(hiddenColors), *editingId);
        } else if (std::find(colors.begin(), colors.end(), *color) == colors.end()) {
            colors.push_back(*color);
        }
        const std::string saved = SaveCustomThemeColors(colors);
        const std::string savedHidden = SaveHiddenThemeColors(hiddenColors);
        store::coreStore().setSetting("ui.custom_theme_colors", saved);
        if (savedHidden != current.hiddenThemeColors)
            store::coreStore().setSetting("ui.hidden_theme_colors", savedHidden);
        const std::string selectedColor =
            !editingId || current.themeColor == *editingId ? *color : current.themeColor;
        if (current.themeColor != selectedColor)
            store::coreStore().setSetting("ui.theme_color", selectedColor);
        model->Update([saved, savedHidden, selectedColor](SettingsView& settings) {
            settings.customThemeColors = saved;
            settings.hiddenThemeColors = savedHidden;
            settings.themeColor = selectedColor;
        });
        context.Dismiss();
    };
    return DialogCard(huxerui::Column {
      huxerui::Text(Localized(editing ? "编辑颜色" : "添加颜色"), huxerui::TextRole::Title),
      huxerui::Row {}.With(huxerui::Frame{.height = kThemeColorPreviewHeight},
          huxerui::Background(preview), huxerui::CornerRadius(theme.shapes.medium)),
      huxerui::TextField(input.Get()).Label(Localized("颜色值"))
          .Placeholder("#RRGGBB")
          .Validation(hex ? huxerui::ValidationResult::None() :
              huxerui::ValidationResult::Invalid(Localized("请输入六位十六进制颜色值")))
          .OnChanged([input, hsb](const huxerui::TextEditingValue& value) {
              input = value;
              if (const auto normalized = NormalizeThemeColor(value.text))
                  hsb = ThemeColorToHsb(ThemeColorFromHex(*normalized));
          })
          .OnSubmitted(save),
      huxerui::Column(std::move(sliders)).With(huxerui::Spacing(theme.spacing.small),
          huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
      huxerui::Row {
        huxerui::Button(Localized("取消")).OnClick([context] { context.Dismiss(); }),
        huxerui::Button(Localized(editing ? "保存" : "添加")).OnClick(save)
            .With(huxerui::Enabled(hex.has_value() && model->view.Get().ready)),
      }.With(huxerui::Spacing(theme.spacing.small), huxerui::MainAlign(huxerui::MainAxisAlignment::End)),
    }.With(huxerui::Frame{.width = kThemeColorDialogWidth}, huxerui::Spacing(theme.spacing.medium),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
}

[[huxerui::composable]] huxerui::View ThemeColorCard(
    FluxAccent accent, bool selected, std::shared_ptr<SettingsModel> model, float edge,
    huxerui::DialogHandle dialog) {
    const auto& theme = huxerui::UseTheme();
    auto menu = UseActionMenu();
    auto tasks = huxerui::UseTaskScope();
    const auto fill = IsDarkTheme(theme) ? accent.dark : accent.light;
    const auto foreground = IsDarkTheme(theme) ? accent.onDark : accent.onLight;
    const huxerui::StringVariant label = accent.id.starts_with('#') ? huxerui::StringVariant{accent.name} : Localized(accent.name);
    huxerui::View card = ThemeColorCardSurface(theme, fill, foreground,
        selected ? std::optional<huxerui::ImageResource>{app::images::check} : std::nullopt,
        selected, huxerui::UseString(label), [model, id = accent.id] { SelectThemeColor(model, id); }, std::nullopt, edge)
        .Key("theme-color-" + accent.id);
    const std::string id = accent.id;
    const std::string initialHex = id.starts_with('#') ? id : ThemeColorToHex(accent.light);
    const auto edit = [dialog, model, id, initialHex] {
        dialog.Show([model, id, initialHex](huxerui::DialogContext context) {
            return ThemeColorDialog(model, context, initialHex, id);
        });
    };
    const auto remove = [model, id] { DeleteThemeColor(model, id); };
    return std::move(card).On<huxerui::ViewEvents::ContextMenuRequested>(
        [menu, tasks, edit, remove](huxerui::Point position) {
            std::vector<ActionMenuEntry> entries{
                ActionMenuItem(app::images::edit, Localized("编辑"), edit),
                ActionMenuItem(app::images::trash, Localized("删除"), remove).Danger(),
            };
            tasks.Launch([menu, position, entries = std::move(entries)]() mutable -> huxerui::Task<void> {
                co_await huxerui::Delay(std::chrono::duration<double>{0});
                menu.ShowAt(position, std::move(entries));
            });
        });
}

[[huxerui::composable]] huxerui::View ThemePage(
    huxerui::State<int> themeMode, std::function<void()> onBack, bool windowTitle) {
    const auto& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto transition = huxerui::UseSceneTransition();
    struct ThemeAnimationFlag {
        bool animating = false;
    };
    auto animating =
        huxerui::UseState(std::make_shared<ThemeAnimationFlag>());
    const auto settingsModel = huxerui::UseService<SettingsModel>();
    // 主题模式：0=自动，1=深色，2=浅色。
    const auto applyTheme = [themeMode, transition, tasks, animating,
                             settingsModel, motion = theme.motion](int mode) {
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
        if (currentDark == targetDark || motion.reduced_motion) {
            mutation();
            return;
        }

        animating.Get()->animating = true;
        tasks.Launch([animating, duration = motion.slow + motion.fast]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{duration});
            animating.Get()->animating = false;
        });
        const huxerui::TransitionSpec reveal{
            huxerui::CircularRevealTransition{}, huxerui::TweenSpec{motion.slow}};
        transition.RunFromCurrentInteraction(
            currentDark ? reveal.Reversed() : reveal, std::move(mutation));
    };

    const auto dialog = huxerui::UseDialog();
    const SettingsView settings = settingsModel->view.Get();
    const float cardEdge = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact ?
        kThemeColorCompactCardEdge : kThemeColorCardEdge;
    const auto selectedAccent = ResolveFluxAccent(settings.themeColor);
    std::vector<huxerui::View> colors;
    const auto hiddenColors = ReadHiddenThemeColors(settings.hiddenThemeColors);
    for (const auto& accent : kFluxAccents)
        if (std::find(hiddenColors.begin(), hiddenColors.end(), accent.id) == hiddenColors.end())
            colors.push_back(ThemeColorCard(
                accent, accent.id == selectedAccent.id, settingsModel, cardEdge, dialog));
    auto customColors = ReadCustomThemeColors(settings.customThemeColors);
    if (selectedAccent.id.starts_with('#') &&
        std::find(customColors.begin(), customColors.end(), selectedAccent.id) == customColors.end())
        customColors.push_back(selectedAccent.id);
    for (const auto& id : customColors)
        colors.push_back(ThemeColorCard(
            ResolveFluxAccent(id), id == selectedAccent.id, settingsModel, cardEdge, dialog));
    const std::array<huxerui::ImageResource, 3> modeIcons{
        app::images::sun_moon, app::images::moon, app::images::sun};
    std::vector<huxerui::View> modes;
    for (std::size_t index = 0; index < kThemeNames.size(); ++index) {
        const bool selected = static_cast<int>(index) == themeMode.Get();
        const auto colors = ResolveSelectableTileColors(theme, selected);
        modes.push_back(ThemeColorCardSurface(theme, colors.surface, colors.fg, modeIcons[index],
            selected, huxerui::UseString(kThemeNames[index]),
            [applyTheme, index] { applyTheme(static_cast<int>(index)); }, kThemeNames[index], cardEdge)
            .Key("theme-mode-" + std::to_string(index)));
    }
    colors.push_back(ThemeColorCardSurface(theme, ResolveIslandTheme(theme).active,
        theme.colors.on_surface_variant, app::images::add, false,
        huxerui::UseString(Localized("添加颜色")), [dialog, settingsModel] {
            dialog.Show([settingsModel](huxerui::DialogContext context) {
                return ThemeColorDialog(settingsModel, context,
                    ThemeColorToHex(FluxPalette::water()), std::nullopt);
            });
        }, std::nullopt, cardEdge).Key("theme-color-add"));
    huxerui::View content = huxerui::ScrollView(huxerui::Column {
      SectionTitle(Localized("模式")),
      huxerui::Row(std::move(modes)).With(huxerui::Spacing(kSectionCardSpacing),
          huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
      SectionTitle(Localized("主题色")),
      huxerui::Flow(std::move(colors)).With(huxerui::Spacing(kSectionCardSpacing)),
    }.With(huxerui::Spacing(theme.spacing.medium),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
    if (windowTitle) {
        huxerui::View back = huxerui::IconButton(app::images::arrow_back, Localized("返回上一页"))
            .OnClick(onBack);
        return PageScaffold(Localized("主题"), huxerui::View{}, content,
                            true, false, true, std::nullopt, back)
            .On<huxerui::ViewEvents::BackRequested>(onBack);
    }
    return SecondaryPageScaffold(
        huxerui::Text(Localized("主题"), huxerui::TextRole::Title), huxerui::View{}, content, onBack);
}

// 独立语言页与桌面选择框共用同一模型及写入口，选择立即更新全应用语言。
[[huxerui::composable]] huxerui::View LanguageChoices(std::function<void()> onSelected) {
    const auto settingsModel = huxerui::UseService<SettingsModel>();
    const std::size_t selected = LanguageIndex(settingsModel->view.Get().language);
    std::vector<huxerui::View> choices;
    for (std::size_t index = 0; index < kLanguages.size(); ++index) {
        if (index != 0) choices.push_back(huxerui::Divider());
        choices.push_back(
            huxerui::RadioButton(index == 0 ? Localized("自动跟随系统语言") : kLanguageNames[index], index == selected)
                .OnChanged([settingsModel, index, onSelected](bool checked) {
                    if (!checked) return;
                    ApplyLanguage(settingsModel, index);
                    if (onSelected) onSelected();
                })
                .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(12.0F, 10.0F)),
                      huxerui::Frame{.min_height = 48.0F})
                .Key("language-option-" + kLanguages[index]));
    }
    return huxerui::Column(std::move(choices)).With(
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View LanguagePage(std::function<void()> onBack) {
    return SecondaryPageScaffold(
        huxerui::Text(Localized("语言"), huxerui::TextRole::Title), huxerui::View{},
        huxerui::ScrollView(LanguageChoices({})), onBack);
}

#if defined(__ANDROID__)

[[huxerui::composable]] huxerui::View AndroidThemeSetting(huxerui::State<int> themeMode) {
    const huxerui::NavigationController navigation = huxerui::UseNavigation();
    return SettingNavigationItem(Localized("主题"), app::images::sun_moon,
        [navigation, themeMode] { navigation.Push(AndroidThemePage, themeMode); })
        .Key("settings-theme");
}

[[huxerui::composable]] huxerui::View AndroidLanguageSetting() {
    const huxerui::NavigationController navigation = huxerui::UseNavigation();
    return SettingNavigationItem(Localized("语言"), app::images::language,
        [navigation] { navigation.Push(AndroidLanguagePage); })
        .Key("settings-language");
}

[[huxerui::composable]] huxerui::View AndroidMoreSettings(
    huxerui::State<std::size_t>, ProfilesCache profilesCache) {
    const huxerui::NavigationController navigation = huxerui::UseNavigation();
    // 同组入口合并成一段，行间用 Divider 分隔；不再套分组卡——设置页整体不用
    // 卡片包裹（桌面端页面岛即唯一卡片，移动端直接铺在页面底色上）。
    return huxerui::Column {
        SectionTitle(Localized("更多")),
        huxerui::Column {
            MoreNavRow(Localized("连接"), app::images::connections,
                       [navigation] { navigation.Push(AndroidConnectionsPage); }),
            huxerui::Divider(),
            MoreNavRow(Localized("日志"), app::images::logs,
                       [navigation] { navigation.Push(AndroidLogsPage); }),
            huxerui::Divider(),
            MoreNavRow(Localized("规则"), app::images::route, [navigation, profilesCache] {
                navigation.Push([profilesCache] {
                    return AndroidRulesPage(profilesCache);
                });
            }),
        }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
    }.With(huxerui::Spacing(10.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

#else

[[huxerui::composable]] huxerui::View DesktopThemeSetting(huxerui::State<int> themeMode) {
    const huxerui::NavigationController navigation = huxerui::UseNavigation();
    const auto model = huxerui::UseEnvironment<DesktopSettingsNavigation>().model;
    return SettingNavigationItem(Localized("主题"), app::images::sun_moon,
        [navigation, themeMode, model] {
            model->secondaryOpen = true;
            navigation.Push(DesktopThemePage, themeMode);
        }).Key("settings-theme");
}

[[huxerui::composable]] huxerui::View DesktopLanguageSetting() {
    const auto dialog = huxerui::UseDialog();
    return SettingNavigationItem(Localized("语言"), app::images::language,
        [dialog] {
            dialog.Show([](huxerui::DialogContext context) {
                return DialogCard(huxerui::Column {
                  huxerui::Text(Localized("语言"), huxerui::TextRole::Title),
                  LanguageChoices([context] { context.Dismiss(); }),
                  huxerui::Button(Localized("取消")).OnClick([context] { context.Dismiss(); }),
                }.With(huxerui::Spacing(12.0F), huxerui::Frame{.width = 320.0F},
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
            });
        }).Key("settings-language");
}

#endif // platform-specific theme and language entries

[[huxerui::composable]] huxerui::View PortSettingsDialog(
    huxerui::DialogContext context,
    huxerui::State<huxerui::TextEditingValue> mixedPort,
    huxerui::State<huxerui::TextEditingValue> httpPort,
    huxerui::State<huxerui::TextEditingValue> socksPort,
    std::shared_ptr<SettingsModel> settingsModel) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const auto toast = huxerui::UseToast();
    const auto save = [context, mixedPort, httpPort, socksPort, settingsModel, toast] {
        const auto mixed = ParsePortText(mixedPort.Get(), false);
        const auto http = ParsePortText(httpPort.Get(), true);
        const auto socks = ParsePortText(socksPort.Get(), true);
        if (!mixed || !http || !socks) {
            toast.Show(Localized("端口无效"));
            return;
        }
        if ((*http > 0 && (*http == *mixed || *http == *socks)) ||
            (*socks > 0 && *socks == *mixed)) {
            toast.Show(Localized("不同类型的端口不能重复"));
            return;
        }

        const std::string mixedValue = std::to_string(*mixed);
        const std::string httpValue = *http == 0 ? std::string{} : std::to_string(*http);
        const std::string socksValue = *socks == 0 ? std::string{} : std::to_string(*socks);
        auto& core = store::coreStore();
        core.setSetting("core.mixed_port", mixedValue);
        core.setSetting("core.http_port", httpValue);
        core.setSetting("core.socks_port", socksValue);
        settingsModel->Update([mixedValue, httpValue, socksValue](SettingsView& view) {
            view.mixedPort = mixedValue;
            view.httpPort = httpValue;
            view.socksPort = socksValue;
        });
        toast.Show(Localized("端口已保存（重启内核生效）"));
        context.Dismiss();
    };

    return DialogCard(huxerui::Column {
        huxerui::Text(Localized("入站端口"), huxerui::TextRole::Title),
        huxerui::Text(Localized(
            "配置混合、HTTP 和 SOCKS 代理入站端口（重启内核生效）"))
            .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption),
                                      theme.colors.on_surface_variant}),
        huxerui::TextField(mixedPort.Get())
            .Variant(huxerui::TextFieldVariant::Outlined)
            .Label(Localized("混合端口"))
            .OnChanged([mixedPort](const huxerui::TextEditingValue& value) {
                mixedPort = value;
            }),
        huxerui::TextField(httpPort.Get())
            .Variant(huxerui::TextFieldVariant::Outlined)
            .Label(Localized("HTTP 端口"))
            .Placeholder(Localized("留空以关闭"))
            .OnChanged([httpPort](const huxerui::TextEditingValue& value) {
                httpPort = value;
            }),
        huxerui::TextField(socksPort.Get())
            .Variant(huxerui::TextFieldVariant::Outlined)
            .Label(Localized("SOCKS 端口"))
            .Placeholder(Localized("留空以关闭"))
            .OnChanged([socksPort](const huxerui::TextEditingValue& value) {
                socksPort = value;
            }),
        huxerui::Row {
            huxerui::Button(Localized("取消")).OnClick([context] {
                context.Dismiss();
            }),
            huxerui::Button(Localized("保存")).OnClick(save)
                .With(huxerui::Enabled(settingsModel->view.Get().ready)),
        }.With(huxerui::Spacing(theme.spacing.small),
               huxerui::MainAlign(huxerui::MainAxisAlignment::End)),
    }.With(huxerui::Frame{.width = kPortSettingsDialogWidth},
           huxerui::Spacing(theme.spacing.medium),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
}

[[huxerui::composable]] huxerui::View PortSettingsEntry() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const auto settingsModel = huxerui::UseService<SettingsModel>();
    const auto dialog = huxerui::UseDialog();
    auto mixedPort = huxerui::UseState(huxerui::TextEditingValue{""});
    auto httpPort = huxerui::UseState(huxerui::TextEditingValue{""});
    auto socksPort = huxerui::UseState(huxerui::TextEditingValue{""});
    const SettingsView settings = settingsModel->view.Get();
    const huxerui::StringVariant label = Localized("入站端口");
    const auto open = [dialog, settingsModel, mixedPort, httpPort, socksPort] {
        const SettingsView current = settingsModel->view.Get();
        if (!current.ready) return;
        mixedPort = huxerui::TextEditingValue{current.mixedPort};
        httpPort = huxerui::TextEditingValue{current.httpPort == "0" ? "" : current.httpPort};
        socksPort = huxerui::TextEditingValue{current.socksPort == "0" ? "" : current.socksPort};
        dialog.Show([mixedPort, httpPort, socksPort, settingsModel](huxerui::DialogContext context) {
            return PortSettingsDialog(context, mixedPort, httpPort, socksPort, settingsModel);
        });
    };
    return huxerui::Row {
        SettingItemIcon(app::images::port),
        huxerui::Column {
            huxerui::Text(label).Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kBody), theme.colors.on_surface}),
            huxerui::Text(Localized(
                "配置混合、HTTP 和 SOCKS 代理入站端口（重启内核生效）"))
                .Style(huxerui::TextStyle{
                    huxerui::Font::System(font_size::kCaption),
                    theme.colors.on_surface_variant}),
        }.With(huxerui::Grow(1.0F), huxerui::Spacing(2.0F)),
    }.With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(
               0.0F, kPortSettingsEntryHorizontalInset)),
           huxerui::Frame{.min_height = kPortSettingsEntryMinHeight},
           huxerui::Spacing(theme.spacing.medium),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
           theme.interactions.indication, huxerui::Focusable(true),
           huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                             .label = huxerui::UseString(label)},
           huxerui::Enabled(settings.ready))
        .OnClick(open);
}

#if !defined(__ANDROID__)
[[huxerui::composable]] huxerui::View DesktopMoreSettings(
    huxerui::State<std::size_t> navPage, ProfilesCache) {
    if (huxerui::UseViewportClass() != huxerui::ViewportClass::Compact) return huxerui::View{};
    return huxerui::Column {
        SectionTitle(Localized("更多")),
        huxerui::Column {
          MoreNavRow(Localized("连接"), app::images::connections, [navPage] { navPage = 4U; }),
          huxerui::Divider(),
          MoreNavRow(Localized("日志"), app::images::logs, [navPage] { navPage = 5U; }),
          huxerui::Divider(),
          MoreNavRow(Localized("规则"), app::images::route, [navPage] { navPage = 3U; }),
        }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
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
    auto toast = huxerui::UseToast();
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
        [busy, modeSelection, coreModel] {
            const CoreView view = coreModel->view.Get();
            if (!busy.Get()) modeSelection = ModeIndex(view.core.mode);
            return [] {};
        },
        coreModel->view);

    // 不可见时只保留本页 State/Lifecycle，不构建重子树：huxerui 的 Pager 会把
    // 四个一级页同时挂载，隐藏页即使不重组，其已挂载子树仍随每一帧被重新测量。
    // 真机实测（代理页大分组）：四页同挂时每帧 1443 次测量请求 / ~20ms，
    // 只留当前页后降到 28 次 / ~0ms；因此不可见页必须返回空占位。
    if (!active) return huxerui::View{huxerui::Row{}}.Key("settings-idle");

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
                    CLASHFLUX_THEME_SETTING(themeMode),
                    CLASHFLUX_LANGUAGE_SETTING(),
                    CLASHFLUX_GENERAL_PLATFORM_SECTION(),
                }.With(huxerui::Spacing(10.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),

                huxerui::Column {
                    SectionTitle(Localized("内核")),
                    SettingRow(
                        Localized("出站模式"), "",
                        ResponsiveOutboundModeSelector(
                            modeSelection, busy, applyOutboundMode), app::images::route),
                    PortSettingsEntry(),
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
                            }), false, app::images::network),
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
                            }), false, app::images::globe),
                    CoreFidelityReport(coreView.core.fidelity),
                }.With(huxerui::Spacing(10.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),

                huxerui::Column {
                    SectionTitle(Localized("关于")),
                    huxerui::Row {
                      SettingItemIcon(app::images::info),
                      huxerui::Text(kAboutText).Style(huxerui::TextStyle{
                        huxerui::Font::System(font_size::kChip),
                        theme.colors.on_surface_variant}).With(huxerui::Grow(1.0F)),
                    }.With(huxerui::Spacing(12.0F),
                           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
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
#undef CLASHFLUX_LANGUAGE_SETTING
#undef CLASHFLUX_THEME_SETTING

} // namespace clashflux::ui
