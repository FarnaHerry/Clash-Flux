#pragma once

#include <huxerui/huxerui.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

namespace clashflux::ui {

// 全项目统一的空白占位语义标识：视觉留白（不渲染横线 "—"），但保留槽位尺寸、
// 未 hydrate 控件的扣留闸门以及无障碍/自动化测试可识别的占位身份。
inline constexpr std::string_view kValuePlaceholderId = "value-placeholder";

inline huxerui::Semantics ValuePlaceholderSemantics() {
    return huxerui::Semantics{
        .role = huxerui::SemanticRole::Generic,
        .identifier = std::string{kValuePlaceholderId},
    };
}

inline huxerui::View PlaceholderSlot(float minWidth = 0.0F, float minHeight = 0.0F) {
    huxerui::Frame frame{};
    if (minWidth > 0.0F) frame.min_width = minWidth;
    if (minHeight > 0.0F) frame.min_height = minHeight;
    return huxerui::Row{}.With(frame, ValuePlaceholderSemantics());
}

inline huxerui::View PlaceholderText(huxerui::Font font, float minWidth = 0.0F) {
    return huxerui::Scope([font, minWidth] {
        const auto fm = huxerui::UseTextMeasurer().Metrics(font);
        const float height = std::max(1.0F, std::ceil(fm.ascent + fm.descent + fm.leading));
        huxerui::Frame frame{.height = height};
        if (minWidth > 0.0F) frame.min_width = minWidth;
        return huxerui::Row{}.With(frame);
    }).With(ValuePlaceholderSemantics());
}

} // namespace clashflux::ui
