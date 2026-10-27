#pragma once

#include <huxerui/huxerui.h>

namespace clashflux::ui {

// Paint-only focus reveal. The caller supplies focus and foreground/reduced-motion
// policy; the retained modifier owns frame progression without recomposition.
huxerui::View WithSearchFocusOutline(huxerui::View content, bool focused,
                                     huxerui::Color color, bool animated = true);

// 桌面标题栏搜索层：把页面动作行与搜索胶囊叠放。搜索胶囊不再定宽——外层
// 「容量槽」按权重（0.3）取标题栏剩余空间并保持居中，内层胶囊不带 Grow、用
// Frame.max_width 封顶（kDesktopTitleBarSearchWidth）：空间足够时 320，窄窗口
// 随容量槽收缩且不溢出。Grow 子节点拿的是紧约束（Frame min/max 与紧约束冲突
// 时以父侧为准），因此封顶必须放在非 Grow 的内层，不能直接写在 Grow 子节点上。
huxerui::View TitleBarSearchStack(huxerui::View title_bar_content,
                                  huxerui::View title_search_field);

} // namespace clashflux::ui
