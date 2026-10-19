// settings_page.cpp — 通用、网络参数、平台维护与关于；日常运行控制放在首页。
//
// 平台专属内容由模块入口处的编译宏选择，平台函数内部自洽管理状态、
// 任务、权限和控件；这里不维护 Android/桌面能力矩阵。
#include <huxerui/huxerui.h>

#include <algorithm>
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
import clashflux.core;
import clashflux.db;
import clashflux.singbox;
import clashflux.stream;
import clashflux.store.core;
import clashflux.store.profiles;

#include "core_model.h"
#include "core_control.h"
#include "profiles_cache.h"
#include "settings_model.h"
#include "theme_colors.h"
#include "theme_color_card.h"

namespace clashflux::ui {
namespace {

const std::vector<huxerui::StringVariant> kThemeNames{
    Localized("自动"), Localized("深色"), Localized("浅色")};
const std::vector<std::string> kLanguages{"system", "zh", "en"};
const std::vector<huxerui::StringVariant> kLanguageNames{
    Localized("自动"), "简体中文", "English"};
constexpr float kPortSettingsDialogWidth = 320.0F;
constexpr float kPortSettingsEntryMinHeight = 48.0F;
constexpr float kPortSettingsEntryHorizontalInset = 10.0F;

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

// 宏只选择模块级平台函数，不把平台能力拆成控件级过滤条件。
#if defined(__ANDROID__)
#define CLASHFLUX_GENERAL_PLATFORM_SECTION AndroidGeneralSettings
#define CLASHFLUX_NETWORK_PLATFORM_SECTION AndroidNetworkSettings
#define CLASHFLUX_MAINTENANCE_PLATFORM_SECTION AndroidKernelSettings
#define CLASHFLUX_MORE_SETTINGS AndroidMoreSettings
#define CLASHFLUX_LANGUAGE_SETTING AndroidLanguageSetting
#define CLASHFLUX_THEME_SETTING AndroidThemeSetting
#else
#define CLASHFLUX_GENERAL_PLATFORM_SECTION DesktopGeneralSettings
#define CLASHFLUX_NETWORK_PLATFORM_SECTION DesktopNetworkSettings
#define CLASHFLUX_MAINTENANCE_PLATFORM_SECTION DesktopKernelSettings
#define CLASHFLUX_MORE_SETTINGS DesktopMoreSettings
#define CLASHFLUX_LANGUAGE_SETTING DesktopLanguageSetting
#define CLASHFLUX_THEME_SETTING DesktopThemeSetting
#endif

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
        const auto error = settingsModel->SetPorts(mixedPort.Get().text,
            httpPort.Get().text, socksPort.Get().text);
        if (error != PortSettingsError::None) {
            toast.Show(error == PortSettingsError::Duplicate
                ? Localized("不同类型的端口不能重复") : Localized("端口无效"));
            return;
        }
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
    auto toast = huxerui::UseToast();
    const auto control = UseCoreControl();
    const auto coreModel = control.model;
    const CoreView coreView = coreModel->view.Get();
    // 不可见时只保留本页 State/Lifecycle，不构建重子树：huxerui 的 Pager 会把
    // 四个一级页同时挂载，隐藏页即使不重组，其已挂载子树仍随每一帧被重新测量。
    // 真机实测（代理页大分组）：四页同挂时每帧 1443 次测量请求 / ~20ms，
    // 只留当前页后降到 28 次 / ~0ms；因此不可见页必须返回空占位。
    if (!active) return huxerui::View{huxerui::Row{}}.Key("settings-idle");

    return PageScaffold(
        Localized("设置"), huxerui::View{},
        huxerui::ScrollView(
            huxerui::Column {
                CLASHFLUX_MORE_SETTINGS(navPage, profilesCache),
                // 参数与维护分区；首页独占日常模式与接管控制。
                huxerui::Column {
                    SectionTitle(Localized("通用")),
                    CLASHFLUX_THEME_SETTING(themeMode),
                    CLASHFLUX_LANGUAGE_SETTING(),
                    CLASHFLUX_GENERAL_PLATFORM_SECTION(),
                }.With(huxerui::Spacing(10.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),

                huxerui::Column {
                    SectionTitle(Localized("网络")),
                    PortSettingsEntry(),
                    CLASHFLUX_NETWORK_PLATFORM_SECTION(),
                    SettingSwitchRow(
                        Localized("局域网连接"), Localized("允许局域网设备接入（下次启动生效）"),
                        huxerui::Switch(coreView.allowLan)
                            .With(huxerui::Enabled(coreView.ready && !control.Busy()))
                            .OnChanged([coreModel, toast](bool on) {
                                coreModel->SetAllowLan(on);
                                toast.Show(Localized(
                                    on ? "已允许局域网连接（重启内核生效）"
                                       : "已关闭局域网连接"));
                            }), false, app::images::network),
                    SettingSwitchRow(
                        "IPv6", Localized("重启内核生效"),
                        huxerui::Switch(coreView.ipv6Enabled)
                            .With(huxerui::Enabled(coreView.ready && !control.Busy()))
                            .OnChanged([coreModel, toast](bool on) {
                                coreModel->SetIpv6(on);
                                toast.Show(Localized(
                                    on ? "已启用 IPv6（重启内核生效）"
                                       : "已关闭 IPv6（重启内核生效）"));
                            }), false, app::images::globe),
                }.With(huxerui::Spacing(10.0F),
                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),

                CLASHFLUX_MAINTENANCE_PLATFORM_SECTION(),

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
#undef CLASHFLUX_NETWORK_PLATFORM_SECTION
#undef CLASHFLUX_MAINTENANCE_PLATFORM_SECTION
#undef CLASHFLUX_MORE_SETTINGS
#undef CLASHFLUX_LANGUAGE_SETTING
#undef CLASHFLUX_THEME_SETTING

} // namespace clashflux::ui
