#pragma once

#include <algorithm>
#include <cmath>
#include <huxerui/huxerui.h>
#include <huxerui/qrcode.h>

namespace clashflux::ui::profile_detail {

// Even a one-point module retains the complete finder patterns at this radius.
inline constexpr float kShareQrCornerRadius = 4.0F;

struct ShareQrModules { using Value = int; };

// Dialogs can constrain their content below 300pt. Align after receiving the
// real bounds, rather than letting a pre-aligned Frame be clamped afterwards.
class ShareQrLayout final : public huxerui::Layout<ShareQrLayout> {
public:
    using Layout::Layout;
    static huxerui::LayoutResult Measure(huxerui::LayoutContext& context,
        huxerui::ViewNode& node, huxerui::Constraints constraints) {
        const auto modules = node.LayoutValueOr<ShareQrModules>(1);
        const float available = std::max(0.0F, std::min({300.0F, constraints.max_width, constraints.max_height}));
        const float scale = std::floor(available / static_cast<float>(modules));
        const float side = scale >= 1.0F ? scale * static_cast<float>(modules) : available;
        const auto size = constraints.Constrain({side, side});
        static_cast<void>(context.Measure(node.ChildAt(0), {side, side, side, side}));
        huxerui::LayoutResult result;
        result.SetSize(size).Place(node.ChildAt(0),
            {(size.width - side) * 0.5F, (size.height - side) * 0.5F});
        return result;
    }
};

inline huxerui::View ShareQrView(huxerui::qrcode::Symbol symbol, int layoutModules = 0) {
    if (layoutModules <= 0) layoutModules = symbol.Width();
    return ShareQrLayout{huxerui::qrcode::QRCode(std::move(symbol)).Key("share-qr-symbol")}
        .LayoutValue<ShareQrModules>(layoutModules);
}

} // namespace clashflux::ui::profile_detail
