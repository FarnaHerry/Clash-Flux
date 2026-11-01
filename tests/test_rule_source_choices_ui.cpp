#include <huxerui/huxerui.h>
#include <huxerui/testing/ui_test.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "rule_source_choices.h"

namespace {

class AppResourceProvider final : public huxerui::PlatformResources {
public:
    huxerui::ResourceConfiguration Configuration() const override { return {}; }
    std::optional<huxerui::InputStream> OpenRead(std::string_view packagePath) override {
        const huxerui::File file =
            huxerui::File(CLASHFLUX_TEST_RESOURCE_PACKAGE).Resolve(packagePath);
        auto result = file.OpenRead();
        if (!result.Succeeded()) return std::nullopt;
        return std::move(result).Value();
    }
};

struct TestProfile {
    std::int64_t id = 0;
    std::string name;
    bool selected = false;
    std::string type = "remote";
    bool operator==(const TestProfile&) const = default;
};

huxerui::View Root() {
    auto profiles = huxerui::UseStateList<TestProfile>();
    const auto choices = clashflux::ui::TargetProfiles(profiles, "直连", true);
    const auto names = clashflux::ui::TargetNames(choices);
    return huxerui::MaterialTheme(huxerui::Column {
        huxerui::Text(std::to_string(names.size())),
        huxerui::Select(names, 0, [](const std::string& value) {
            return huxerui::Text(value);
        }),
    }.With(huxerui::Spacing(8.0F)));
}

} // namespace

int main() {
    const huxerui::Application app{Root, {.show_debug_overlay = false}};
    huxerui::testing::UiTestOptions options;
    options.viewport = {480.0F, 320.0F};
    options.resource_provider = std::make_shared<AppResourceProvider>();
    huxerui::testing::UiTest ui(app, options);
    const bool direct = ui.Find(huxerui::testing::UiSelector::Text("直连")).Exists();
    const bool count = ui.Find(huxerui::testing::UiSelector::Text("1")).Exists();
    std::printf("direct=%d count=%d\n", direct ? 1 : 0, count ? 1 : 0);
    return direct && count ? 0 : 1;
}
