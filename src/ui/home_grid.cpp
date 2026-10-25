#include "home_grid.h"

#include <algorithm>

namespace clashflux::ui {

huxerui::LayoutResult HomeGrid::Measure(huxerui::LayoutContext& context,
                                        huxerui::ViewNode& node,
                                        huxerui::Constraints constraints) {
    huxerui::LayoutResult result;
    const float available = constraints.HasBoundedWidth()
                                ? constraints.max_width
                                : kHomeGridFallbackWidth;
    const int columns = HomeGridColumns(
        available, node.LayoutValueOr<HomeGridCompact>(false));
    const float column_width =
        std::max(0.0F, (available - kHomeGridGap * static_cast<float>(columns - 1)) /
                           static_cast<float>(columns));

    struct Placement {
        huxerui::ViewNode* child;
        int width;
        int height;
        int row;
        int column;
    };

    std::vector<Placement> placements;
    placements.reserve(node.Children().Size());
    for (huxerui::ViewNode& child : node.Children()) {
        HomeCardSize span = child.LayoutValueOr<HomeCardSpan>(HomeCardSize{});
        if (child.LayoutValueOr<HomeControlSpan>(false)) {
            span.width = columns >= 5 ? 2 : 1;
        }
        placements.push_back(Placement{
            &child,
            std::clamp(span.width, 1, columns),
            std::clamp(span.height, 1, kHomeGridMaxSpan),
            0,
            0,
        });
    }

    // 二维占位表：行按需增长（外层高度无界，不能依赖 constraints.max_height）。
    std::vector<std::vector<bool>> occupied;
    const auto ensure_rows = [&occupied, columns](int rows) {
        while (static_cast<int>(occupied.size()) < rows) {
            occupied.emplace_back(static_cast<std::size_t>(columns), false);
        }
    };
    const auto fits = [&occupied](int row, int column, int width, int height) {
        for (int r = row; r < row + height; ++r) {
            const std::vector<bool>& line =
                occupied[static_cast<std::size_t>(r)];
            for (int c = column; c < column + width; ++c) {
                if (line[static_cast<std::size_t>(c)]) return false;
            }
        }
        return true;
    };

    int total_rows = 0;
    for (Placement& placement : placements) {
        bool placed = false;
        for (int row = 0; !placed; ++row) {
            ensure_rows(row + placement.height);
            for (int column = 0; column + placement.width <= columns; ++column) {
                if (!fits(row, column, placement.width, placement.height)) {
                    continue;
                }
                placement.row = row;
                placement.column = column;
                for (int r = row; r < row + placement.height; ++r) {
                    for (int c = column; c < column + placement.width; ++c) {
                        occupied[static_cast<std::size_t>(r)]
                                [static_cast<std::size_t>(c)] = true;
                    }
                }
                total_rows = std::max(total_rows, row + placement.height);
                placed = true;
                break;
            }
        }
    }

    for (const Placement& placement : placements) {
        const float width =
            column_width * static_cast<float>(placement.width) +
            kHomeGridGap * static_cast<float>(placement.width - 1);
        const float height =
            kHomeGridUnitHeight * static_cast<float>(placement.height) +
            kHomeGridGap * static_cast<float>(placement.height - 1);
        static_cast<void>(context.Measure(
            *placement.child,
            huxerui::Constraints{width, width, height, height}));
        result.Place(*placement.child,
                     huxerui::Point{
                         (column_width + kHomeGridGap) *
                             static_cast<float>(placement.column),
                         (kHomeGridUnitHeight + kHomeGridGap) *
                             static_cast<float>(placement.row),
                     });
    }

    const float total_height =
        total_rows == 0
            ? 0.0F
            : kHomeGridUnitHeight * static_cast<float>(total_rows) +
                  kHomeGridGap * static_cast<float>(total_rows - 1);
    return result.SetSize(constraints.Constrain({available, total_height}));
}

huxerui::View HomeCardSkeleton(HomeCardKind kind, const huxerui::ThemeSpec& theme) {
    const auto block = [&theme](float height) {
        return huxerui::Row {}.With(huxerui::Frame{.height = height},
            huxerui::Background(theme.colors.surface_container_highest), huxerui::CornerRadius(4.0F));
    };
    std::vector<huxerui::View> lines;
    lines.push_back(huxerui::Row {
      huxerui::View{block(20.0F)}.With(huxerui::Frame{.width = 20.0F}),
      huxerui::View{block(16.0F)}.With(huxerui::Grow(1.0F)),
    }.With(huxerui::Spacing(8.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
    if (kind == HomeCardKind::Traffic) {
        lines.push_back(huxerui::Row {}.With(huxerui::Grow(1.0F),
            huxerui::Background(theme.colors.surface_container_highest), huxerui::CornerRadius(4.0F)));
    } else {
        const int count = kind == HomeCardKind::Mode ? 3
            : kind == HomeCardKind::Proxy || kind == HomeCardKind::Tun ? 1 : 2;
        for (int index = 0; index < count; ++index) lines.push_back(block(12.0F));
        lines.push_back(huxerui::Spacer());
    }
    return huxerui::Column(lines).With(huxerui::Spacing(8.0F), huxerui::Grow(1.0F),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)).Key("home-card-skeleton");
}

} // namespace clashflux::ui
