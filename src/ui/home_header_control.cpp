#include "home_header_control.h"
#include "page_layout.h"
#include "theme_colors.h"
#include "ui.h"
#include <stop_token>

namespace clashflux::ui {
namespace {

constexpr const char* kHeaderMarker = "home-header-control";

void MoveHeader(bool show, HomeHeaderControl control, huxerui::TaskScope tasks, bool reduced) {
    if (control.shown.Get() == show || control.motion.Get().action != CoreAction::Idle) return;
    AnimateHomeControlCard(show, control.shown, control.motion, control.transition, tasks, reduced);
}

[[huxerui::composable]] huxerui::View HeaderRunningContent(HomeHeaderSnapshot snapshot, bool animate) {
    const auto theme = huxerui::UseTheme();
    const auto tasks = huxerui::UseTaskScope();
    auto clock = huxerui::UseState(std::chrono::steady_clock::now());
    const bool foreground = huxerui::UseWindow().LifecycleState() != huxerui::WindowLifecycleState::Background;
    huxerui::Lifecycle([tasks, clock, foreground, since = snapshot.runningSince] {
        std::stop_source cancel;
        if (foreground && since != std::chrono::steady_clock::time_point{}) {
            tasks.Launch([clock, token = cancel.get_token()]() -> huxerui::Task<void> {
                while (!token.stop_requested()) {
                    clock = std::chrono::steady_clock::now();
                    co_await huxerui::Delay(std::chrono::seconds{1});
                }
            });
        }
        return [cancel]() mutable { cancel.request_stop(); };
    }, snapshot.runningSince, foreground);
    // Subscribe to the one-second wakeup, but sample now on recomposition too:
    // a restored/replaced session must not use the previous session's tick.
    static_cast<void>(clock.Get());
    const auto seconds = snapshot.runningSince == std::chrono::steady_clock::time_point{}
        ? snapshot.elapsedSeconds
        : std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::steady_clock::now() - snapshot.runningSince).count();
    return huxerui::Row {
      BoxCatStatusIcon(BoxCatPhase::Running, animate),
      huxerui::Column {
        huxerui::Text(Localized(snapshot.phase == BoxCatPhase::Stopping ? "正在停止"
            : snapshot.phase == BoxCatPhase::Starting ? "正在启动" : "运行中"))
            .Style(huxerui::TextStyle{
                huxerui::Font::System(font_size::kCaption).WithWeight(huxerui::FontWeight::SemiBold),
                theme.colors.on_surface}),
        huxerui::Text(FormatHomeUptime(seconds))
            .Style(huxerui::TextStyle{
                huxerui::Font::Monospace(font_size::kCaption), theme.colors.on_surface_variant})
            .Key("home-header-uptime"),
      }.With(huxerui::Spacing(2.0F)),
    }.With(huxerui::Spacing(6.0F),
           huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

[[huxerui::composable]] huxerui::View HeaderRunningCard(
    HomeHeaderSnapshot snapshot, bool animate) {
    const auto& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    // Build the surface on the real view node, as Card does. Decorating the
    // composable's returned scope would leave the rounded corners unpainted.
    return huxerui::Column {HeaderRunningContent(snapshot, animate)}.With(
        huxerui::Frame{.height = kPageHeaderHeight},
        huxerui::Padding(huxerui::EdgeInsets{
            .right = theme.spacing.medium, .left = theme.spacing.medium}),
        huxerui::Background(islands.raised),
        huxerui::CornerRadius(islands.nested_radius),
        huxerui::Border(islands.outline_soft, kCardOutlineWidth),
        huxerui::ClipChildren(),
        huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace

HomeHeaderControl UseHomeHeaderControl(HomeHeaderSnapshot snapshot) {
    const auto theme = huxerui::UseTheme();
    const auto tasks = huxerui::UseTaskScope();
    HomeHeaderControl control{
        huxerui::UseState(false), huxerui::UseState(false),
        huxerui::UseState(HomeControlMotion{}), huxerui::UseState(snapshot),
        huxerui::UseSharedTransition()};
    auto synced = huxerui::UseState(false);
    auto wasActive = huxerui::UseState(false);
    const bool foreground = huxerui::UseWindow().LifecycleState() != huxerui::WindowLifecycleState::Background;
    huxerui::Lifecycle([control, snapshot, tasks, synced, wasActive, foreground,
                       reduced = theme.motion.reduced_motion]() -> std::function<void()> {
        control.latest = snapshot;
        const bool active = snapshot.active && foreground;
        if (!snapshot.ready) return [] {};
        if (!synced.Get() || !active || !wasActive.Get() || reduced) {
            if (control.motion.Get().action != CoreAction::Idle)
                control.motion = HomeControlMotion{CoreAction::Idle, control.motion.Get().serial + 1};
            control.shown = snapshot.running;
            control.catReady = snapshot.running;
            synced = true;
        } else {
            if (!snapshot.running || snapshot.phase == BoxCatPhase::Starting || snapshot.phase == BoxCatPhase::Stopping)
                control.catReady = false;
            if (snapshot.phase == BoxCatPhase::Stopping || !snapshot.running) {
                MoveHeader(false, control, tasks, reduced);
            } else if (!snapshot.busy && control.catReady.Get()) {
                MoveHeader(true, control, tasks, reduced);
            }
        }
        wasActive = active;
        return [] {};
    }, snapshot, foreground, theme.motion.reduced_motion, control.catReady, control.motion);
    return control;
}

[[huxerui::composable]] huxerui::View HomeHeaderTitle(
    HomeHeaderControl control, std::function<void()> toggle, huxerui::StringVariant stopLabel) {
    const auto theme = huxerui::UseTheme();
    const auto tasks = huxerui::UseTaskScope();
    const auto snapshot = control.latest.Get();
    const auto motion = control.motion.Get();
    huxerui::View content = PageTitleText(theme, Localized("首页"));
    if (control.shown.Get()) {
        if (motion.collapsed) {
            content = huxerui::Stack {
              BoxCatAnimation(BoxCatPhase::Running, false)
                  .With(huxerui::Frame{.width = 48.0F, .height = 48.0F},
                        huxerui::SharedBounds(kHeaderMarker)
                      .BoundsTransform(HomeControlReturnToDock{})),
            }.With(huxerui::Align(huxerui::HorizontalAlignment::Center, huxerui::VerticalAlignment::Center));
        } else {
            content = HeaderRunningCard(snapshot, motion.action == CoreAction::Idle)
                .OnClick([control, toggle, tasks, reduced = theme.motion.reduced_motion] {
                    MoveHeader(false, control, tasks, reduced);
                    toggle();
                })
                .With(huxerui::Scale(huxerui::AnimateTo(
                          motion.collapsing ? kHomeControlCollapsedScale : 1.0F,
                          kHomeControlCollapseMotion)),
                      huxerui::Enabled(!snapshot.busy && motion.action == CoreAction::Idle),
                      huxerui::Focusable(true), huxerui::Indication{},
                      huxerui::Tooltip(stopLabel),
                      huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                         .label = huxerui::UseString(stopLabel)},
                      huxerui::SharedBounds(kHeaderMarker).BoundsTransform(HomeControlMoveExpand{}))
                .Key("home-header-running");
        }
    }
    return huxerui::Column {content}.With(
        huxerui::Frame{.height = kPageHeaderHeight},
        huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)).Key("home-header-slot");
}

[[huxerui::composable]] huxerui::View HomeHeaderFloatingAction(
    huxerui::View page, HomeHeaderControl control, std::function<void()> toggle,
    huxerui::StringVariant actionLabel, float bottomInset) {
    const auto theme = huxerui::UseTheme();
    const auto snapshot = control.latest.Get();
    const auto motion = control.motion.Get();
    const bool returning = motion.action == CoreAction::Stop;
    const bool changing = !returning &&
        (snapshot.phase == BoxCatPhase::Starting || snapshot.phase == BoxCatPhase::Stopping);
    const bool enabled = snapshot.ready && !snapshot.busy && motion.action == CoreAction::Idle &&
        (snapshot.running || snapshot.canStart) && snapshot.phase != BoxCatPhase::Starting;
    const auto ready = [control] {
        const auto latest = control.latest.Get();
        if (latest.active && latest.running && latest.phase == BoxCatPhase::Running)
            control.catReady = true;
    };
    auto bounds = huxerui::SharedBounds(kHeaderMarker);
    if (returning) bounds = std::move(bounds).BoundsTransform(HomeControlReturnToDock{});
    else bounds = std::move(bounds).BoundsTransform(HomeControlMoveExpand{});
    // A 56pt marker is constrained by the 48pt header on the outgoing side.
    // Pair the actual 48pt artwork at both ends, preserving the full 56pt hit
    // target separately. The same cat node survives until playback resumes.
    huxerui::View artwork = BoxCatAnimation(snapshot.phase,
          motion.action == CoreAction::Idle, {},
          snapshot.phase == BoxCatPhase::Failed ? theme.colors.error : theme.colors.on_surface,
          ready, returning ? std::optional{BoxCatPhase::Running} : std::nullopt)
          .With(huxerui::Frame{.width = 48.0F, .height = 48.0F}, std::move(bounds))
          .Key("box-cat-artwork");
    huxerui::View button = huxerui::Stack {std::move(artwork)}
          .With(huxerui::Frame{.width = 56.0F, .height = 56.0F},
                huxerui::Align(huxerui::HorizontalAlignment::Center,
                               huxerui::VerticalAlignment::Center))
          .OnClick(toggle)
          .With(huxerui::Enabled(enabled), huxerui::Focusable(true), huxerui::Indication{},
                huxerui::Tooltip(actionLabel),
                huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                   .label = huxerui::UseString(actionLabel)})
          .Key("home-header-floating-cat");
    huxerui::View floating = huxerui::Stack {std::move(button)}.With(
        huxerui::Frame{.width = 56.0F, .height = 56.0F},
        huxerui::Align(huxerui::HorizontalAlignment::Center,
                       huxerui::VerticalAlignment::Center),
        huxerui::Scale(huxerui::AnimateTo(changing ? 0.94F : 1.0F,
            huxerui::TweenSpec{.duration = 0.18, .easing = huxerui::Easing::EaseOut})));
    huxerui::View dock = huxerui::Column {
      std::move(floating),
    }.With(huxerui::Padding(huxerui::EdgeInsets{
               .right = theme.spacing.medium, .bottom = bottomInset, .left = theme.spacing.medium}),
           huxerui::MainAlign(huxerui::MainAxisAlignment::End),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::End));
    return huxerui::Stack {
      page,
      control.shown.Get() ? huxerui::View{huxerui::Row {}} : dock,
    }.With(huxerui::Grow(1.0F),
           huxerui::Align(huxerui::HorizontalAlignment::Stretch, huxerui::VerticalAlignment::Stretch),
           control.transition.Scope()).Key("home-header-control-scope");
}

} // namespace clashflux::ui
