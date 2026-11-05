// Manual diagnostic for a queued state update followed by application shutdown.
// Kept separate from passing regression gates until the framework fix is integrated.
#include <huxerui/huxerui.h>
#include <huxerui/testing/ui_test.h>

#include <cstdio>
#include <exception>
#include <optional>
#include <string>

namespace {
std::optional<huxerui::State<int>> tick;
std::optional<huxerui::ApplicationHandle> application;
std::optional<huxerui::TaskScope> tasks;
int builds = 0;

huxerui::View Root() {
    ++builds;
    tasks = huxerui::UseApplicationTaskScope();
    static_cast<void>(tick->Get());
    return huxerui::Spacer();
}
}

int main() {
    huxerui::Application declaration(Root, {
        .show_debug_overlay = false,
        .application_hooks = {[](huxerui::ApplicationContext&) {
            application = huxerui::UseApplication();
            tick.emplace(0);
        }},
    });
    huxerui::testing::UiTest ui(declaration);
    ui.Pump();
    tasks->Post([] {
        *tick = 1;
        application->Quit();
    });
    try {
        ui.Pump();
    } catch (const std::exception& error) {
        std::printf("after queued shutdown: builds=%d, exception=%s\n", builds, error.what());
        return std::string(error.what()) == "HuxerUI application is stopping" ? 10 : 11;
    }
    std::printf("after queued shutdown: builds=%d, no exception\n", builds);
}
