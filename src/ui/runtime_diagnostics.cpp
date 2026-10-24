#include <huxerui/huxerui.h>
#include "runtime_diagnostics.h"
#include "ui.h"
#include "app_resources.h"
namespace clashflux::ui {
[[huxerui::composable]] huxerui::View RuntimeDiagnosticsContent(
    huxerui::State<huxerui::TextEditingValue> target, bool busy, bool canRun,
    std::string report, std::function<void()> start, std::function<void()> cancel,
    std::function<void()> copy, std::function<void()> close) {
    return huxerui::Column {
        huxerui::Text(Localized("运行诊断"), huxerui::TextRole::Title),
        huxerui::Text(Localized("留空仅检查运行状态；填写 HTTP(S) 目标后，通过当前代理入站检查。结果不代表全部网络。")),
        huxerui::TextField(target.Get()).Label(Localized("检查目标（可选）"))
            .With(huxerui::Enabled(!busy)).OnChanged([target](const huxerui::TextEditingValue& value) { target = value; }),
        huxerui::ScrollView(huxerui::Text(busy ? huxerui::UseString(Localized("正在收集诊断证据…")) : report))
            .With(huxerui::Frame{.height = 240.0F}),
        huxerui::Row {
            huxerui::Button(Localized("开始诊断")).With(huxerui::Enabled(!busy && canRun)).OnClick(start),
            huxerui::Button(Localized("取消")).With(huxerui::Enabled(busy)).OnClick(std::move(cancel)),
        }.With(huxerui::Spacing(8.0F)),
        huxerui::Button(Localized("复制脱敏报告")).With(huxerui::Enabled(!busy && !report.empty()))
            .OnClick(std::move(copy)),
        huxerui::Button(Localized("关闭")).OnClick(std::move(close)),
    }.With(huxerui::Spacing(12.0F), huxerui::Frame{.width = 320.0F},
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}
}
