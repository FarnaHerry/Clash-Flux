#include "box_cat.h"
#include <chrono>

namespace {

using namespace huxerui;
using namespace std::chrono_literals;
using clashflux::ui::BoxCatPhase;

[[huxerui::composable]] View RestartPreview() {
    const auto theme = UseTheme();
    const auto tasks = UseTaskScope();
    const auto phase = UseState(BoxCatPhase::Running);
    const auto restarting = phase.Get() == BoxCatPhase::Restarting;
    const auto restart = [phase, tasks](std::chrono::milliseconds duration) {
        if (phase.Get() == BoxCatPhase::Restarting) return;
        phase = BoxCatPhase::Restarting;
        tasks.Launch([phase, duration]() -> Task<void> {
            co_await Delay(duration);
            phase = BoxCatPhase::Running;
        });
    };
    return Column {
      clashflux::ui::BoxCatAnimation(phase.Get()).With(Frame{.width = 224.0F, .height = 224.0F}),
      Row {
        clashflux::ui::BoxCatStatusIcon(phase.Get()),
        Column {
          Text(restarting ? "正在重启" : "运行中"),
          Text("00:12:34").Style(TextStyle{Font::Monospace(12.0F), theme.colors.on_surface_variant}),
        },
      }.With(Frame{.width = 220.0F, .height = 48.0F},
             Background(theme.colors.surface_container_high), CornerRadius(14.0F),
             Spacing(6.0F), MainAlign(MainAxisAlignment::Center),
             CrossAlign(CrossAxisAlignment::Center)),
      Row {
        Button("原地重启").OnClick([restart] { restart(1500ms); }).With(Enabled(!restarting)),
        Button("慢速重启").OnClick([restart] { restart(3500ms); }).With(Enabled(!restarting)),
      }.With(Spacing(8.0F)),
    }.With(Spacing(18.0F), Grow(1.0F), MainAlign(MainAxisAlignment::Center),
           CrossAlign(CrossAxisAlignment::Center), Background(theme.colors.surface_container_low));
}

View App() { return FlatTheme(FlatDarkThemeSpec(), RestartPreview()); }

} // namespace

int main() {
    const huxerui::Application application{App, {
        .window = {.title = "Clash Flux · 猫咪原地重启", .initial_size = {560.0F, 420.0F}},
        .show_debug_overlay = false}};
    return huxerui::RunApplication();
}
