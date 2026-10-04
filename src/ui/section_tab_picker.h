#pragma once

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ui.h"

namespace clashflux::ui {

struct SectionPickerLayers {
    explicit SectionPickerLayers(huxerui::LayerController controller) : layers(std::move(controller)) {}
    huxerui::LayerController layers;
};
inline void InstallSectionPickerLayers(huxerui::WindowContext& context) {
    context.Provide(std::make_shared<SectionPickerLayers>(context.Layers()));
}

class GroupSideDrawerLayout final : public huxerui::Layout<GroupSideDrawerLayout> {
public:
    using Layout::Layout;
    static huxerui::LayoutResult Measure(huxerui::LayoutContext& context,
        huxerui::ViewNode& node, huxerui::Constraints constraints) {
        const auto size = constraints.Constrain({constraints.max_width, constraints.max_height});
        const float width = std::min(360.0F, size.width);
        static_cast<void>(context.Measure(node.ChildAt(0), {size.width, size.width, size.height, size.height}));
        static_cast<void>(context.Measure(node.ChildAt(1), {width, width, size.height, size.height}));
        huxerui::LayoutResult result;
        result.SetSize(size);
        result.Place(node.ChildAt(0), {});
        result.Place(node.ChildAt(1), {size.width - width, 0.0F});
        return result;
    }
};

inline huxerui::View GroupPickerContent(const std::vector<SectionTab>& tabs,
    const std::string& selected, std::function<void(const std::string&)> select,
    std::function<void()> dismiss) {
    const auto& theme = huxerui::UseTheme();
    std::vector<huxerui::View> choices;
    for (const auto& tab : tabs) {
        const bool active = tab.key == selected;
        std::vector<huxerui::View> label{
            huxerui::Text(tab.label, huxerui::TextRole::Label)
                .With(huxerui::Foreground(active ? theme.colors.on_primary_container : theme.colors.on_surface)),
        };
        if (!tab.badge.empty()) label.push_back(huxerui::Text(tab.badge, huxerui::TextRole::Label)
            .With(huxerui::Foreground(SemanticWarningColor(theme))));
        choices.push_back(huxerui::Row(std::move(label)).With(
            huxerui::Spacing(theme.spacing.extra_small),
            huxerui::Padding(huxerui::EdgeInsets::Symmetric(12.0F, 10.0F)),
            huxerui::CornerRadius(theme.shapes.small),
            huxerui::Background(active ? theme.colors.primary_container : theme.colors.surface_container_high),
            theme.interactions.indication, huxerui::Focusable(true),
            huxerui::Semantics{.role = huxerui::SemanticRole::Button, .label = tab.label, .selected = active})
            .OnClick([dismiss, select, key = tab.key] { dismiss(); select(key); })
            .Key("group-picker-item-" + tab.key));
    }
    return huxerui::Column{
        huxerui::Stack{
            huxerui::Row{
                huxerui::Text(Localized("策略组"), huxerui::TextRole::Title),
            }.With(huxerui::MainAlign(huxerui::MainAxisAlignment::Center)),
            huxerui::Row{
                huxerui::IconButton(huxerui::ImageResource{"app", "images/close"}, Localized("关闭"))
                    .With(huxerui::Tooltip(Localized("关闭"))).OnClick(dismiss),
            }.With(huxerui::MainAlign(huxerui::MainAxisAlignment::End)),
        }.With(huxerui::Align(huxerui::HorizontalAlignment::Stretch,
                              huxerui::VerticalAlignment::Center)),
        huxerui::ScrollView(huxerui::Flow(std::move(choices)).With(
            huxerui::Spacing(theme.spacing.small),
            huxerui::MainAlign(huxerui::MainAxisAlignment::Center)))
            .With(huxerui::Grow(1.0F)).Key("group-picker-scroll"),
    }.With(huxerui::Padding(theme.spacing.medium), huxerui::Spacing(theme.spacing.medium),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

struct GroupPickerSession {
    huxerui::LayerId id = 0;
    std::function<void()> close;
};

inline void ShowGroupSideDrawer(huxerui::LayerController layers,
    std::vector<SectionTab> tabs, std::string selected,
    std::function<void(const std::string&)> select, float topInset) {
    auto session = std::make_shared<GroupPickerSession>();
    const auto requestClose = [session, layers] {
        if (session->close) session->close();
        else layers.Dismiss(session->id);
    };
    session->id = layers.Attach({
        .pointer_policy = huxerui::LayerPointerPolicy::Content,
        .trap_focus = true,
        .cancel_policy = huxerui::LayerCancelPolicy::Dismiss,
        .on_dismiss_request = requestClose,
    }, [=] {
        return huxerui::Scope([=] {
            const auto& theme = huxerui::UseTheme();
            const double duration = theme.motion.reduced_motion ? 0.0 : theme.motion.normal;
            auto shown = huxerui::UseState(theme.motion.reduced_motion);
            auto closing = huxerui::UseState(false);
            auto tasks = huxerui::UseTaskScope();
            const auto close = [shown, closing, tasks, layers, id = session->id, duration] {
                if (closing.Get()) return;
                closing = true;
                shown = false;
                tasks.Launch([layers, id, duration]() -> huxerui::Task<void> {
                    co_await huxerui::Delay(std::chrono::duration<double>{duration});
                    layers.Dismiss(id);
                });
            };
            huxerui::Lifecycle([session, close, tasks, shown, closing] {
                session->close = close;
                tasks.Launch([shown, closing]() -> huxerui::Task<void> {
                    co_await huxerui::Delay(std::chrono::duration<double>{0});
                    if (!closing.Get()) shown = true;
                });
                return [session] { session->close = {}; };
            }, 0);
            auto panel = GroupPickerContent(tabs, selected, select, close).With(
                huxerui::SafeAreaPadding{},
                huxerui::Background(theme.colors.surface_container),
                huxerui::Enabled(!closing.Get()), huxerui::Indication{})
                .OnClick([] {})
                .With(huxerui::Offset(huxerui::AnimateTo(
                    huxerui::Point{shown.Get() ? 0.0F : 360.0F, 0.0F},
                    huxerui::TweenSpec{.duration = duration, .easing = huxerui::Easing::EaseOut})))
                .Key("group-picker-side");
            return GroupSideDrawerLayout{
                huxerui::Row{}.With(huxerui::Indication{})
                    .OnClick(close).Key("group-picker-scrim"),
                std::move(panel),
            }.With(huxerui::Padding(huxerui::EdgeInsets{.top = topInset}),
                   huxerui::ClipChildren());
        });
    });
}

} // namespace clashflux::ui
