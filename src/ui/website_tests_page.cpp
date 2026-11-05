#include <huxerui/huxerui.h>
#include "app_resources.h"
#include "ui.h"
#include "page_layout.h"
#include "website_test_card.h"
#include "website_test_grid.h"

namespace clashflux::ui {

#if defined(__ANDROID__)
constexpr bool kWebsiteApplicationBypassesVpn = true;
#else
constexpr bool kWebsiteApplicationBypassesVpn = false;
#endif

[[huxerui::composable]] huxerui::View WebsiteTestsPage(std::function<void()> onBack, bool active) {
    const auto model = huxerui::UseService<WebsiteTestModel>();
    const auto http = huxerui::UseService<AppHttpClient>();
    const auto tasks = huxerui::UseApplicationTaskScope();
    const auto toast = UseFluxToast();
    const auto offset = huxerui::UseState(std::make_shared<PageScrollOffset>()).Get();
    const auto controller = huxerui::UseScrollController(offset->value);
    const auto& theme = huxerui::UseTheme();
    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    const auto rows = model->sites.Get();
    const bool busy = model->busy.Get();
    const auto invalidUrl = huxerui::UseString(Localized("请输入不重复的 HTTP(S) 网站地址，最多添加 24 项。"));
    const WebsiteTestModel::Probe probe = [http](std::string url) { return ProbeWebsite(http, std::move(url)); };
    const auto run = [model, tasks, probe](std::vector<std::size_t> indices) {
        model->Start(tasks, probe, std::move(indices));
    };
    if (!active) return huxerui::View{};

    const auto actions = busy ? DialogAction(Localized("取消"), [model] { model->Cancel(); }, false)
        : DialogAction(Localized("全部测试"), [run, count = rows.size()] {
            std::vector<std::size_t> indices;
            for (std::size_t i = 0; i < count; ++i) indices.push_back(i);
            run(std::move(indices));
        }, true);
    const auto header = huxerui::Column {
      huxerui::Text(Localized("使用当前应用网络发送 HTTP HEAD 请求，显示响应状态与耗时。网站已响应不代表网页完整加载。"))
          .With(huxerui::Foreground(theme.colors.on_surface_variant)),
      kWebsiteApplicationBypassesVpn ? huxerui::View{huxerui::Text(
          Localized("Android 上应用自身绕过虚拟网卡；此处结果不代表其他应用的 VPN 分流。"))
          .With(huxerui::Foreground(theme.colors.on_surface_variant))} : huxerui::View{},
      huxerui::Flow {
        huxerui::TextField(model->customUrl.Get()).Label(Localized("自定义网站 URL"))
            .OnChanged([model](const huxerui::TextEditingValue& value) { model->customUrl = value; })
            .With(huxerui::Grow(1.0F), huxerui::Frame{.min_width = compact ? 180.0F : 280.0F},
                huxerui::Enabled(!busy)).Key("website-custom-url"),
        DialogAction(Localized("添加网站"), [model, toast, invalidUrl] {
            if (!model->Add(model->customUrl.Get().text)) toast.Show(invalidUrl);
        }, false, !busy && rows.size() < 24),
      }.With(huxerui::Spacing(8.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
    }.With(huxerui::Spacing(16.0F),
        huxerui::Padding(huxerui::EdgeInsets{.top = 16.0F, .bottom = 8.0F}),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)).Key("website-test-header");
    std::vector<huxerui::View> cards;
    for (std::size_t i = 0; i < rows.size(); ++i)
        cards.push_back(Card(WebsiteTestCardContent(rows[i], !busy, [run, i] { run({i}); })).Key(rows[i].url));
    const auto content = WebsiteTestGrid(header, std::move(cards), compact, controller, offset);
    if (onBack) return SecondaryPageScaffold(huxerui::Text(Localized("网站测试")), actions, content, onBack);
    return PageScaffold(Localized("网站测试"), actions, content);
}

} // namespace clashflux::ui
