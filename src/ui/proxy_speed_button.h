#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>
#include <utility>

#include <huxerui/huxerui.h>

#include "app_resources.h"
#include "ui.h"

namespace clashflux::ui {

inline constexpr float kProxySpeedButtonSize = 32.0F;

struct ProxySpeedIrisChanged : huxerui::Event<void(bool)> {};

struct ProxySpeedIris {
    class Extension;
    huxerui::VectorAsset icon;
    huxerui::Color foreground;
    float iconSize;
    bool testing;
    bool snap;
    bool paintButton;
    bool operator==(const ProxySpeedIris&) const = default;
};

class ProxySpeedIris::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode&, const ProxySpeedIris& iris)
        : iris_(iris), closure_(iris.testing ? 1.0F : 0.0F), open_(!iris.testing) {}

    void Update(huxerui::ViewNode&, const ProxySpeedIris& iris) {
        iris_ = iris;
        InvalidatePaint();
    }

    FrameResult OnFrame(huxerui::ViewNode&, const huxerui::FrameInfo& frame) override {
        const float target = iris_.testing ? 1.0F : 0.0F;
        if (iris_.snap || frame.reduced_motion) {
            if (closure_.Value() != target || closure_.IsRunning()) {
                closure_.Set(target);
                InvalidatePaint();
            }
        } else if (closure_.Target() != target) {
            // Reflect the CSS timing curve across its midpoint to play it backwards.
            const auto timing = iris_.testing ? huxerui::CubicBezierCurve{0.65F, 0.0F, 0.85F, 0.4F}
                                              : huxerui::CubicBezierCurve{0.15F, 0.6F, 0.35F, 1.0F};
            closure_.AnimateTo(target, huxerui::TweenSpec{0.52 * std::abs(target - closure_.Value()), timing});
        }
        const auto motion = closure_.Advance(frame);
        if (motion.changed) InvalidatePaint();
        const bool open = !iris_.testing && !closure_.IsRunning();
        if (open != open_) {
            open_ = open;
            EmitEvent<ProxySpeedIrisChanged>(open_);
        }
        return {motion.needs_frame, motion.wake_after};
    }

    void PaintAboveContent(const huxerui::ViewNode& node, huxerui::PaintContext& paint) const override {
        const float closure = closure_.Value();
        if (!iris_.paintButton || closure >= 1.0F) return;

        const auto bounds = node.ContentBounds();
        const huxerui::Point center{bounds.x + bounds.width * 0.5F, bounds.y + bounds.height * 0.5F};
        const float radius = 0.75F * bounds.width * (1.0F - closure);
        const float angle = std::numbers::pi_v<float> * closure;
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        const float opacity = std::lerp(1.0F, 0.2F, closure);
        paint.PushClip({center.x - radius, center.y - radius, 2.0F * radius, 2.0F * radius},
                       huxerui::CornerRadii{radius});
        paint.PushTransform({c, s, -s, c, center.x - c * center.x + s * center.y,
                                         center.y - s * center.x - c * center.y});
        const float iconSize = std::clamp(iris_.iconSize, 0.0F, bounds.width);
        paint.DrawImage(iris_.icon, {center.x - iconSize * 0.5F, center.y - iconSize * 0.5F, iconSize, iconSize},
                        iris_.foreground, opacity);
        paint.PopTransform();
        paint.PopClip();
    }

private:
    ProxySpeedIris iris_;
    huxerui::MotionController closure_;
    bool open_;
};

inline huxerui::View ProxySpeedButton(bool testing, bool enabled, std::function<void()> start) {
    return huxerui::Scope([testing, enabled, start = std::move(start)] {
        const auto& theme = huxerui::UseTheme();
        const auto open = huxerui::UseState(!testing);
        const bool showButton = open.Get() && !testing;
        const bool background = huxerui::UseWindow().LifecycleState() == huxerui::WindowLifecycleState::Background;
        const auto icon = app::images::proxy_node_probe;
        const float iconSize = huxerui::UseEnvironment<huxerui::IconButtonStyle>().icon_size;
        constexpr huxerui::Frame size{.width = kProxySpeedButtonSize, .height = kProxySpeedButtonSize};
        huxerui::View iris = huxerui::Row {}.With(size, ProxySpeedIris{
            huxerui::UseVectorImage(icon), theme.colors.on_surface, iconSize, testing, background, !showButton,
        }).On<ProxySpeedIrisChanged>([open](bool value) { open = value; }).Key("proxy-speed-iris");
        if (testing) {
            iris = std::move(iris).With(huxerui::Semantics{
                .label = Localized("测速中…"), .busy = true, .live_region = huxerui::SemanticLiveRegion::Polite,
            });
        }

        huxerui::View button;
        if (showButton) {
            button = huxerui::IconButton(icon, Localized("测速"))
                .With(size, huxerui::Enabled(enabled), huxerui::Foreground(theme.colors.on_surface),
                      huxerui::Tooltip(Localized("测试当前分组延迟")))
                .OnClick(start)
                .Key("proxy-speed-action");
        }
        return huxerui::Stack {
          iris,
          button,
        }.With(size).Key("proxy-speed-button");
    });
}

} // namespace clashflux::ui
