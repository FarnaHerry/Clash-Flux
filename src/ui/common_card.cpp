#include "ui.h"

namespace clashflux::ui {

[[huxerui::composable]] huxerui::View Card(huxerui::View content, std::optional<float> contentPadding) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    const bool flat = huxerui::UseEnvironment<DialogSurfaceContext>().flat;
    // 二级岛由 raised 表面与页面底色区分。
    //
    // 表面/圆角/描边必须在**建树时**落在这个 Column 上：composable 返回的是 hcg
    // 生成的 Scope 包装节点，调用方在 `Card(...)` 的返回值上再补
    // `.With(Background/Border/CornerRadius)` 只会画在整棵子树背后，从圆角差里漏出
    // 一圈底色（首页「当前订阅」卡、订阅卡都踩过：深灰卡 + 蓝色圆角边）。需要另一种
    // 表面就再加一个建树型 composable（如 `SelectableTile`），不要事后装修。
    return huxerui::Column { std::move(content) }
        .With(huxerui::Padding(contentPadding.value_or(islands.island_padding)),
              huxerui::Background(flat ? huxerui::Color::Transparent() : islands.raised),
              huxerui::CornerRadius(islands.nested_radius),
              huxerui::ClipChildren(),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace clashflux::ui
