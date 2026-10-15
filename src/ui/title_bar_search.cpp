// title_bar_search.cpp — 桌面标题栏搜索胶囊的响应式叠放层。
// 单独成 TU：无头几何回归（test_page_transition）直链本文件，不拉入
// common.cpp 的存储与页面依赖。
#include "title_bar_search.h"

#include "ui.h"

namespace clashflux::ui {

huxerui::View TitleBarSearchStack(huxerui::View title_bar_content,
                                  huxerui::View title_search_field) {
    return huxerui::Stack{
        std::move(title_bar_content),
        huxerui::Row{
            huxerui::Spacer{}.With(huxerui::Grow(0.35F)),
            // 三层结构，绕开两个框架语义：
            // 1) Grow 子节点拿紧约束，Frame.max_width 会被压过——封顶不能写在
            //    任何 Grow 子节点上；
            // 2) 胶囊（PillSearchField）自身带 Grow(1.0F)，直接放进 Row 会成为
            //    Grow 子节点按权重分宽（0.3×1/3 ≈ 0.1 栏宽，缩成一小块）。
            // 因此：容量槽以 Grow(0.3) 拿标题栏剩余空间的 30%（紧约束）；
            // 槽内非 Grow 的 Column 按松约束测量、宽度 = min(槽宽, 320)（封顶在
            // Column 的 Frame 上生效）；胶囊在 Column 内横向铺满。
            huxerui::Row{
                huxerui::Spacer{}.With(huxerui::Grow(1.0F)),
                huxerui::Column{std::move(title_search_field)}
                    .With(huxerui::Frame{.max_width = kDesktopTitleBarSearchWidth},
                          huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
                huxerui::Spacer{}.With(huxerui::Grow(1.0F)),
            }
                .With(huxerui::Grow(0.3F),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
                .Key("title-search-slot"),
            huxerui::Spacer{}.With(huxerui::Grow(0.35F)),
        }
            .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
    }
        .With(huxerui::Grow(1.0F),
              huxerui::Align(huxerui::HorizontalAlignment::Stretch,
                             huxerui::VerticalAlignment::Stretch));
}

} // namespace clashflux::ui
