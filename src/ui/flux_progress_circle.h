#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

#include <huxerui/huxerui.h>

#include "theme_colors.h"

namespace clashflux::ui {

namespace detail {

inline float ProgressArcCubicBezierCoord(float time, float first, float second) noexcept {
    const float inv = 1.0F - time;
    return 3.0F * inv * inv * time * first + 3.0F * inv * time * time * second + time * time * time;
}

inline float ProgressArcCubicBezier(float progress, float x1, float y1, float x2, float y2) noexcept {
    const float target = std::clamp(progress, 0.0F, 1.0F);
    if (target <= 0.0F || target >= 1.0F) return target;
    float lower = 0.0F;
    float upper = 1.0F;
    for (int i = 0; i < 16; ++i) {
        const float mid = (lower + upper) * 0.5F;
        if (ProgressArcCubicBezierCoord(mid, x1, x2) < target) lower = mid;
        else upper = mid;
    }
    return ProgressArcCubicBezierCoord((lower + upper) * 0.5F, y1, y2);
}

inline float ProgressCirclePulsingArcProgress(float phase, float minimum, float maximum) noexcept {
    if (phase < 0.5F) return minimum + (maximum - minimum) * phase * 2.0F;
    const float eased = ProgressArcCubicBezier((phase - 0.5F) * 2.0F, 0.2F, 0.0F, 0.0F, 1.0F);
    return maximum + (minimum - maximum) * eased;
}

inline float ProgressCirclePulsingArcRotation(float phase) noexcept {
    constexpr float kPi = 3.14159265358979323846F;
    constexpr float kStageDuration = 0.25F;
    constexpr float kRotationDuration = 0.05F;
    const float stage = std::floor(phase / kStageDuration);
    const float stageProgress = phase - stage * kStageDuration;
    const float easedRotation = ProgressArcCubicBezier(
        std::min(stageProgress / kRotationDuration, 1.0F), 0.05F, 0.7F, 0.1F, 1.0F);
    return phase * kPi * 6.0F + (stage + easedRotation) * kPi * 0.5F;
}

struct MultiColorProgressCircleArc {
    class Extension;
    float strokeWidth = 2.0F;
    double duration = 0.0;
    bool reducedMotion = false;
    huxerui::ProgressCircleIndeterminateMotion motion =
        huxerui::ProgressCircleIndeterminateMotion::PulsingArc;
    float minArc = 0.1F;
    float maxArc = 0.87F;
    huxerui::ThemeBrightness brightness = huxerui::ThemeBrightness::Light;
    FluxMultiColorPalette palette = DefaultMultiColorPalette();
    bool operator==(const MultiColorProgressCircleArc&) const = default;
};

class MultiColorProgressCircleArc::Extension final : public huxerui::NodeExtension {
public:
    static constexpr float kReducedMotionPhase = 0.35F;

    Extension(huxerui::ViewNode&, const MultiColorProgressCircleArc& spec)
        : spec_(spec), phase_(spec.reducedMotion ? kReducedMotionPhase : 0.0F) {}

    void Update(huxerui::ViewNode&, const MultiColorProgressCircleArc& spec) {
        if (spec_ == spec) return;
        spec_ = spec;
        if (spec_.reducedMotion) phase_ = kReducedMotionPhase;
        InvalidatePaint(PaintInvalidation::Foreground);
    }

    FrameResult OnFrame(huxerui::ViewNode&, const huxerui::FrameInfo& frame) override {
        if (frame.reduced_motion || spec_.reducedMotion) {
            if (phase_ != kReducedMotionPhase) {
                phase_ = kReducedMotionPhase;
                InvalidatePaint(PaintInvalidation::Foreground);
            }
            return {};
        }
        if (!std::isfinite(spec_.duration) || spec_.duration <= 0.0) {
            return {};
        }
        phase_ += static_cast<float>(frame.delta_time / spec_.duration);
        phase_ -= std::floor(phase_);
        InvalidatePaint(PaintInvalidation::Foreground);
        return {.needs_frame = true};
    }

    void PaintAboveContent(const huxerui::ViewNode& node, huxerui::PaintContext& paint) const override {
        constexpr float kPi = 3.14159265358979323846F;
        constexpr float kFullCircle = kPi * 2.0F;
        const auto frame = node.Bounds();
        const float strokeWidth = std::max(0.0F, spec_.strokeWidth);
        const float radius = std::max(
            0.0F, std::min(frame.width, frame.height) * 0.5F - strokeWidth * 0.5F);
        if (radius <= 0.0F || strokeWidth <= 0.0F) return;

        const float minArc = std::clamp(spec_.minArc, 0.0F, 1.0F);
        const float maxArc = std::clamp(spec_.maxArc, minArc, 1.0F);
        const bool pulsing = spec_.motion == huxerui::ProgressCircleIndeterminateMotion::PulsingArc;
        const float progress = pulsing
            ? ProgressCirclePulsingArcProgress(phase_, minArc, maxArc)
            : minArc + (maxArc - minArc) * (1.0F - std::cos(phase_ * kFullCircle)) * 0.5F;
        if (progress <= 0.0F) return;

        const float start = pulsing
            ? ProgressCirclePulsingArcRotation(phase_)
            : -kPi * 0.5F + phase_ * kFullCircle * 2.0F;
        const float sweep = std::clamp(progress, 0.0F, 1.0F) * kFullCircle;
        if (sweep <= 0.0F) return;

        const huxerui::Point center{frame.x + frame.width * 0.5F, frame.y + frame.height * 0.5F};
        const auto pointAt = [&](float angle) {
            return huxerui::Point{center.x + std::cos(angle) * radius,
                                  center.y + std::sin(angle) * radius};
        };

        huxerui::Path arc;
        arc.MoveTo(pointAt(start));
        const huxerui::Size radii{radius, radius};
        if (sweep >= kPi) {
            const float mid = start + sweep * 0.5F;
            arc.ArcTo(radii, 0.0F, huxerui::ArcSize::Small, huxerui::ArcDirection::Clockwise,
                      pointAt(mid));
            arc.ArcTo(radii, 0.0F, huxerui::ArcSize::Small, huxerui::ArcDirection::Clockwise,
                      pointAt(start + sweep));
        } else {
            arc.ArcTo(radii, 0.0F, huxerui::ArcSize::Small, huxerui::ArcDirection::Clockwise,
                      pointAt(start + sweep));
        }

        PaintMultiColorStrokePath(paint, arc, frame, spec_.palette, spec_.brightness,
                                  huxerui::StrokeStyle{.width = strokeWidth, .cap = huxerui::StrokeCap::Round});
    }

private:
    MultiColorProgressCircleArc spec_;
    float phase_ = 0.0F;
};

} // namespace detail

/// Circular loading indicator synchronized with the active accent color or multi-color palette.
inline huxerui::View FluxProgressCircle(
    std::optional<float> size = std::nullopt,
    std::optional<float> strokeWidth = std::nullopt,
    std::optional<huxerui::Semantics> semantics = std::nullopt) {
    return huxerui::Scope([size, strokeWidth, semantics = std::move(semantics)] {
        const auto& theme = huxerui::UseTheme();
        const auto& gradient = huxerui::UseEnvironment<FluxGradientStyle>();
        const bool background =
            huxerui::UseWindow().LifecycleState() == huxerui::WindowLifecycleState::Background;
        auto style = huxerui::UseEnvironment<huxerui::ProgressCircleStyle>();
        if (size.has_value()) {
            style.size = *size;
        }
        if (strokeWidth.has_value()) {
            style.stroke_width = *strokeWidth;
        } else if (size.has_value()) {
            style.stroke_width = std::clamp(style.size * 0.1F, 1.75F, style.stroke_width);
        }

        const double activeDuration =
            (theme.motion.reduced_motion || background) ? 0.0 : style.animation_duration;
        huxerui::View circle = huxerui::ProgressCircle();
        if (semantics.has_value()) {
            circle = std::move(circle).With(*semantics);
        }

        if (!gradient.enabled) {
            style.indicator_color = theme.colors.primary;
            style.animation_duration = activeDuration;
            return huxerui::ProvideEnvironment(style, std::move(circle));
        }

        const float arcStroke = style.stroke_width;
        const auto motion = style.indeterminate_motion;
        const float minArc = style.minimum_indeterminate_arc_fraction;
        const float maxArc = style.maximum_indeterminate_arc_fraction;
        style.indicator_color = huxerui::Color::Transparent();
        style.animation_duration = 0.0;
        return huxerui::ProvideEnvironment(
            style,
            std::move(circle).With(detail::MultiColorProgressCircleArc{
                .strokeWidth = arcStroke,
                .duration = activeDuration,
                .reducedMotion = theme.motion.reduced_motion,
                .motion = motion,
                .minArc = minArc,
                .maxArc = maxArc,
                .brightness = theme.brightness,
                .palette = ResolveGradientPalette(gradient),
            }));
    });
}

} // namespace clashflux::ui
